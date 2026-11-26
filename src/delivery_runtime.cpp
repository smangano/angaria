#include "delivery_runtime.h"

#include "delivery_state.h"
#include "publication_metadata.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace angaria::detail
{
  namespace
  {
    void require(bool condition, const char* path, const char* message)
    {
      if (!condition)
      {
        runtime_fail(ErrorCode::IoError, path, message);
      }
    }

    bool zero(std::span<const std::uint64_t> words)
    {
      return std::all_of(words.begin(), words.end(), [](auto word)
      {
        return word == 0;
      });
    }

    std::vector<std::byte> atomic_bytes(RuntimeAccess& access, std::uint64_t offset,
                                        std::size_t words)
    {
      const auto values = access.read_words(offset, words);
      std::vector<std::byte> bytes(words * 8);
      for (std::size_t index = 0; index < words; ++index)
      {
        for (unsigned byte = 0; byte < 8; ++byte)
        {
          bytes[index * 8 + byte] = static_cast<std::byte>(values[index] >> (8 * byte));
        }
      }
      return bytes;
    }

    template <typename Definition>
    struct HistoricalDefinition
    {
      Definition _definition;
      std::uint64_t _reference{};
    };

    template <typename Definition>
    using History = std::map<decltype(Definition{}._id), std::vector<HistoricalDefinition<Definition>>>;

    template <typename Definition, typename Decoder>
    History<Definition> history(RuntimeAccess& access, std::uint16_t kind, Decoder decoder)
    {
      History<Definition> result;
      for (const auto reference : access.record_references(kind))
      {
        auto definition = runtime_value(decoder(access.immutable_record(reference, kind), reference));
        const auto id = definition._id;
        result[id].push_back(HistoricalDefinition<Definition>{std::move(definition), reference});
      }
      return result;
    }

    template <typename Definition>
    const Definition& effective(const History<Definition>& records, decltype(Definition{}._id) id,
                                  std::uint64_t publication, std::uint64_t revision)
    {
      const auto found = records.find(id);
      require(found != records.end(), "publication.topology", "Publication references an unknown topology identity");
      const auto& definitions = found->second;
      const auto after = std::lower_bound(definitions.begin(), definitions.end(), publication,
                                          [](const auto& entry, auto offset)
                                          {
                                            return entry._reference < offset;
                                          });
      require(after != definitions.begin(), "publication.topology", "Topology did not exist before Publication acceptance");
      const auto& definition = std::prev(after)->_definition;
      require(definition._registration._revision <= revision, "publication.topology",
              "Publication predates its effective topology definition");
      return definition;
    }

    struct TaskContext
    {
      const LoadedTask* _task{};
      std::vector<std::uint64_t> _words;
    };

    struct SourceContext
    {
      const LoadedSource* _source{};
      std::vector<std::uint64_t> _words;
    };

    struct DeliveryEntry
    {
      std::uint64_t _sequence{};
      std::uint64_t _publication{};
      std::uint64_t _link{};
      std::uint64_t _receiptReference{};
      std::uint64_t _producerIncarnation{};
      mapped::Receipt _receipt;
    };

    struct AcceptanceBatch
    {
      TaskId _actor{};
      std::uint64_t _incarnation{};
      std::uint64_t _publication{};
    };

    std::map<std::uint64_t, AcceptanceBatch> acceptance_batches(RuntimeAccess& access)
    {
      const auto publications = access.record_references(9);
      std::map<std::uint64_t, AcceptanceBatch> result;
      for (const auto reference : access.record_references(11))
      {
        const auto generation = access.record_envelope(reference, 11)._generation;
        const auto manifest = runtime_value(mapped::decode_manifest_body(access.immutable_record(reference, 11), reference));
        if (manifest._subtype == mapped::TransactionSubtype::Registry)
        {
          require(std::none_of(manifest._records.begin(), manifest._records.end(), [&](auto member)
          {
            return std::binary_search(publications.begin(), publications.end(), member);
          }), "publication.manifest", "Registry transaction contains Publication acceptance");
          continue;
        }
        if (manifest._subtype == mapped::TransactionSubtype::Retry)
        {
          runtime_fail(ErrorCode::UnsupportedPolicy, "publication.manifest", "Retry transactions require a later native capability");
        }
        require(manifest._subtype == mapped::TransactionSubtype::Publication &&
                    manifest._baseRevision == manifest._newRevision && manifest._records.size() == 1,
                "publication.manifest", "Direct acceptance requires its own Publication transaction without Registry changes");
        (void)access.record_envelope(manifest._records.front(), 9);
        require(result.emplace(generation, AcceptanceBatch{manifest._actor, manifest._actorIncarnation,
                                                            manifest._records.front()}).second,
                "publication.manifest", "Publication acceptance has duplicate transaction evidence");
      }
      return result;
    }

    void check_ring(RuntimeAccess& access, const std::vector<std::uint64_t>& state,
                      const std::vector<DeliveryEntry>& deliveries)
    {
      const auto capacity = state[3];
      const auto& envelope = access.record_envelope(state[2], 8);
      const auto words = (envelope._totalBytes - mapped::record_envelope_bytes) / 8;
      const auto last = state[0];
      const auto floor = last < capacity ? 1 : last - capacity + 1;
      // Bound temporary copies independently of configured ring capacity. A
      // large legal reservation is not a request to allocate that much memory.
      for (std::uint64_t offset = 0; offset < words;)
      {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(4096, words - offset));
        const auto bytes = atomic_bytes(access, state[2] + mapped::record_envelope_bytes + offset * 8, count);
        for (std::size_t word = 0; word < count; word += 4)
        {
          const auto index = (offset + word) / 4;
          const auto slotBytes = std::span{bytes}.subspan(word * 8, mapped::ring_slot_bytes);
          if (index >= capacity)
          {
            require(std::all_of(slotBytes.begin(), slotBytes.end(), [](auto byte)
            {
              return byte == std::byte{};
            }), "queue.ring", "Ring allocation padding must be zero");
            continue;
          }
          const auto slot = runtime_value(mapped::decode_ring_slot(slotBytes, capacity, index));
          const bool occupied = last >= capacity || index < last;
          require((slot._queueSequence != 0) == occupied, "queue.ring", "Ring omits or invents a current-window entry");
          if (occupied)
          {
            require(slot._queueSequence >= floor && slot._queueSequence <= last,
                    "queue.ring", "Ring entry is outside the authoritative latest window");
            const auto& delivery = deliveries[static_cast<std::size_t>(slot._queueSequence - 1)];
            require(slot._deliveryLink == delivery._link && slot._publicationReference == delivery._publication,
                    "queue.ring", "Ring references a different Queue delivery or Publication");
          }
        }
        offset += count;
      }
    }

    void check_queue(RuntimeAccess& access, const LoadedQueue& queue,
                       const std::vector<DeliveryEntry>& deliveries)
    {
      const auto state = access.read_words(queue._stateOffset, 32);
      require(state[0] == deliveries.size(), "queue.history", "Queue sequence root disagrees with complete retained history");
      if (deliveries.empty())
      {
        require(state[1] == 0 && zero(std::span{state}.subspan(4)), "queue.history",
                "Queue without Publications contains delivery progress");
      }
      else
      {
        require(state[1] == deliveries.back()._link && state[4] == deliveries.back()._producerIncarnation &&
                    state[5] == 1 && state[6] == deliveries.front()._link,
                "queue.history", "Queue latest link, producer incarnation, or retained-history floor disagrees");
        std::uint64_t terminal{};
        bool acknowledged{};
        std::uint64_t lastAcknowledgment{};
        for (const auto& delivery : deliveries)
        {
          const auto& receipt = delivery._receipt;
          if (receipt._state == mapped::ReceiptState::Acknowledged)
          {
            acknowledged = true;
            require(delivery._sequence - terminal == 1, "queue.progress",
                    "Acknowledgment skips an earlier nonterminal delivery");
            lastAcknowledgment = std::bit_cast<std::uint64_t>(receipt._acknowledgedAt.time_since_epoch().count());
            terminal = delivery._sequence;
          }
          else if (receipt._state == mapped::ReceiptState::Issued)
          {
            require(delivery._sequence - terminal == 1, "queue.progress",
                    "Issued receipt skips the oldest nonterminal Queue delivery");
          }
        }
        const auto pending = terminal == deliveries.size() ? 0 : deliveries[static_cast<std::size_t>(terminal)]._link;
        require(state[8] == terminal && state[9] == pending, "queue.progress",
                "Queue terminal watermark or oldest nonterminal link disagrees with receipts");
        require(state[11] == (acknowledged ? 1u : 0u) &&
                    state[10] == lastAcknowledgment,
                "queue.progress", "Queue acknowledgment time presence or retained receipt evidence disagrees");
      }
      check_ring(access, state, deliveries);
    }
  } // namespace

  void runtime_validate_delivery(RuntimeAccess& access, const TopologySnapshot& topology)
  {
    const auto& configuration = access._session->_controller->configuration();
    require((configuration._requiredFeatures & mapped::native_delivery_feature) != 0,
            "publication.feature", "Delivery history requires native_delivery_v1");
    std::map<TaskId, TaskContext> tasks;
    for (const auto& task : topology._tasks)
    {
      auto words = access.read_words(task._stateOffset, 32);
      require(words[8] == 0, "task.retirement", "Publication retirement requires a later native capability");
      tasks.emplace(task._definition._id, TaskContext{&task, std::move(words)});
    }
    std::map<SourceQueueSetId, SourceContext> sources;
    for (const auto& source : topology._sources)
    {
      sources.emplace(source._definition._id, SourceContext{&source, access.read_words(source._stateOffset, 32)});
    }
    const auto queueHistory = history<mapped::QueueDefinition>(access, 3, mapped::decode_queue_definition);
    const auto sourceHistory = history<mapped::SourceDefinition>(access, 4, mapped::decode_source_definition);
    const auto schemas = runtime_schemas(access);
    std::map<std::uint64_t, const LoadedSchema*> schemaReferences;
    for (const auto& schema : schemas)
    {
      schemaReferences.emplace(schema._reference, &schema);
    }
    const auto batches = acceptance_batches(access);
    std::set<std::tuple<TaskId, std::uint64_t, std::uint64_t>> identities;
    std::set<std::tuple<SourceQueueSetId, std::uint64_t, std::uint64_t>> issuances;
    std::map<TaskId, std::uint64_t> producerIncarnations;
    std::map<QueueId, std::vector<DeliveryEntry>> deliveries;
    std::map<SourceQueueSetId, std::pair<QueueId, std::size_t>> pins;
    for (const auto reference : access.record_references(9))
    {
      const auto& envelope = access.record_envelope(reference, 9);
      const auto publication = runtime_value(mapped::decode_publication_body(
          access.immutable_record(reference, 9), reference, configuration._maxLogBytes));
      if (publication._publishingSet != PublishingQueueSetId::Invalid)
      {
        runtime_fail(ErrorCode::UnsupportedPolicy, "publication.publishing", "Native Publishing acceptance requires a later capability");
      }
      const auto batch = batches.find(envelope._generation);
      require(envelope._previous == 0 && batch != batches.end() && batch->second._publication == reference &&
                  batch->second._actor == publication._producer && batch->second._incarnation == publication._producerIncarnation,
              "publication.manifest", "Publication identity disagrees with its committed acceptance batch");
      const auto producer = tasks.find(publication._producer);
      require(producer != tasks.end() && producer->second._task->_reference < reference &&
                  producer->second._task->_definition._registration._revision <= envelope._registryRevision &&
                  publication._producerIncarnation <= producer->second._words[0] &&
                  (publication._producerIncarnation != producer->second._words[0] || publication._publicationSequence <= producer->second._words[1]),
              "publication.producer", "Publication producer claim or issued identity is inconsistent");
      require(identities.emplace(publication._producer, publication._producerIncarnation, publication._publicationSequence).second,
              "publication.identity", "Publication identity was accepted more than once");
      auto& incarnation = producerIncarnations[publication._producer];
      require(publication._producerIncarnation >= incarnation, "publication.producer", "Producer incarnation moves backward in history");
      incarnation = publication._producerIncarnation;
      const auto schema = schemaReferences.find(publication._schemaReference);
      require(schema != schemaReferences.end() && schema->second->_definition._descriptor._schema == publication._schema &&
                  schema->second->_definition._registration._revision <= envelope._registryRevision,
              "publication.schema", "Publication does not reference its exact registered schema at acceptance");
      const auto& destination = publication._destinations.front();
      const auto& queue = effective(queueHistory, destination._queue, reference, envelope._registryRevision);
      const auto& source = effective(sourceHistory, queue._descriptor._sourceSet, reference, envelope._registryRevision);
      require(queue._state == EntityState::Active && source._state == EntityState::Active &&
                  queue._descriptor._producer == publication._producer &&
                  queue._registration._revision == publication._topologyRevision,
              "publication.topology", "Publication ownership, active topology, or original Queue revision disagrees");
      auto& queueDeliveries = deliveries[destination._queue];
      require(queueDeliveries.size() < std::numeric_limits<std::uint64_t>::max() &&
                  destination._queueSequence == queueDeliveries.size() + 1 &&
                  destination._previousDelivery == (queueDeliveries.empty() ? 0 : queueDeliveries.back()._link),
              "queue.history", "Queue sequence or predecessor breaks complete accepted history");
      const auto receipt = runtime_value(mapped::decode_receipt(atomic_bytes(access, destination._receiptReference, 16)));
      require(receipt._state != mapped::ReceiptState::Unused, "publication.receipt", "Accepted destination has no receipt obligation");
      if ((receipt._presence & 1) != 0)
      {
        const auto consumer = tasks.find(receipt._consumerTask);
        const auto currentSource = sources.find(receipt._sourceSet);
        require(receipt._sourceSet == queue._descriptor._sourceSet && consumer != tasks.end() && currentSource != sources.end() &&
                    receipt._consumerTask == source._descriptor._owner && receipt._consumerIncarnation <= consumer->second._words[0] &&
                    receipt._consumerIncarnation <= currentSource->second._words[1] &&
                    (receipt._consumerIncarnation != currentSource->second._words[1] || receipt._issuance <= currentSource->second._words[0]),
                "receipt.consumer", "Receipt consumer, Source, incarnation, or issuance is inconsistent");
        require(issuances.emplace(receipt._sourceSet, receipt._consumerIncarnation, receipt._issuance).second,
                "receipt.issuance", "One Source issuance identifies more than one receipt");
        if (receipt._state == mapped::ReceiptState::Issued)
        {
          require(receipt._consumerIncarnation == consumer->second._words[0] &&
                      (consumer->second._words[6] & task_attached_flag) != 0 &&
                      pins.emplace(receipt._sourceSet, std::pair{destination._queue, queueDeliveries.size()}).second,
                  "receipt.pin", "Issued receipt has a stale claim or a second pin in its Source");
          if (consumer->second._task->_definition._descriptor._kind == TaskKind::Thread)
          {
            const auto parent = tasks.find(consumer->second._task->_definition._descriptor._process.value_or(TaskId::Invalid));
            require(parent != tasks.end() && parent->second._words[0] == consumer->second._words[7] &&
                        (parent->second._words[6] & task_attached_flag) != 0 &&
                        parent->second._words[3] == consumer->second._words[3],
                    "receipt.pin", "Issued Thread receipt belongs to an ended parent Process claim");
          }
        }
      }
      queueDeliveries.push_back(DeliveryEntry{destination._queueSequence, reference,
          reference + mapped::record_envelope_bytes + mapped::publication_fixed_bytes,
          destination._receiptReference, publication._producerIncarnation, receipt});
      const auto padding = reference + envelope._totalBytes - destination._receiptReference - mapped::receipt_bytes;
      require(padding < mapped::section_alignment && zero(access.read_words(destination._receiptReference + mapped::receipt_bytes,
                                                                             static_cast<std::size_t>(padding / 8))),
              "publication.padding", "Publication allocation padding must be zero");
    }
    for (const auto& queue : topology._queues)
    {
      if (queue._definition._state == EntityState::Active)
      {
        check_queue(access, queue, deliveries[queue._definition._id]);
      }
    }
    for (const auto& [id, context] : sources)
    {
      const auto& state = context._words;
      const auto owner = tasks.find(context._source->_definition._descriptor._owner);
      require(state[1] == 0 ? state[0] == 0 :
                  (owner != tasks.end() && state[1] <= owner->second._words[0]),
              "source_set.claim", "Source issuance counter or recorded consumer incarnation is inconsistent");
      const auto pin = pins.find(id);
      if (pin == pins.end())
      {
        require(zero(std::span{state}.subspan(2, 6)), "source_set.pin", "Source slot has no matching Issued receipt");
        continue;
      }
      const auto& delivery = deliveries.at(pin->second.first)[pin->second.second];
      const auto& receipt = delivery._receipt;
      require(state[0] == receipt._issuance && state[1] == receipt._consumerIncarnation &&
                  state[2] == (static_cast<std::uint16_t>(pin->second.first) | std::uint64_t{1} << 16) &&
                  state[3] == delivery._sequence && state[4] == delivery._publication &&
                  state[5] == delivery._receiptReference && state[6] == std::bit_cast<std::uint64_t>(receipt._dequeuedAt.time_since_epoch().count()) &&
                  state[7] == delivery._link,
              "source_set.pin", "Source in-flight slot disagrees with the exact Issued receipt");
    }
  }
} // namespace angaria::detail
