#include "publication_runtime.h"

#include "angaria/detail/append_layout.h"
#include "angaria/detail/wire_codes.h"
#include "angaria/payload.h"
#include "delivery_state.h"
#include "mapped_payload.h"
#include "payload_runtime.h"
#include "performance_diagnostics.h"
#include "registry_access.h"
#include "source_runtime_state.h"
#include "topology_state.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace angaria::detail
{
  namespace
  {
    void require(bool condition, ErrorCode code, const char* path, const char* message)
    {
      if (!condition)
      {
        runtime_fail(code, path, message);
      }
    }

    struct ProducerContext
    {
      TopologySnapshot _topology;
      std::vector<std::uint64_t> _words;
    };

    ProducerContext producer_context(RuntimeAccess& access, const TaskClaim& producer)
    {
      runtime_value(access._guard.check_owner());
      const auto& configuration = access._session->_controller->configuration();
      require((configuration._requiredFeatures & mapped::native_delivery_feature) != 0,
              ErrorCode::UnsupportedPolicy, "publication.feature", "Session does not enable native publication");
      require(producer._session == configuration._session && producer._incarnation != 0,
              ErrorCode::WrongOwner, "publication.producer", "Producer claim belongs to another Session or is empty");
      ProducerContext result;
      result._topology = runtime_topology(access);
      const auto& task = require_registered_entity(result._topology._tasks, producer._task,
          "publication.producer", "Producer Task identity is empty", "Producer Task is not registered",
          ErrorCode::WrongOwner, ErrorCode::WrongOwner);
      result._words = access.read_words(task._stateOffset, 32);
      require(result._words[0] == producer._incarnation && (result._words[6] & task_attached_flag) != 0,
              ErrorCode::WrongOwner, "publication.producer", "Producer claim no longer owns this Task");
      if (task._definition._descriptor._kind == TaskKind::Thread)
      {
        const auto& parent = require_registered_entity(result._topology._tasks,
            task._definition._descriptor._process.value_or(TaskId::Invalid), "publication.producer",
            "Thread producer has no parent Process", "Thread producer parent is not registered",
            ErrorCode::WrongOwner, ErrorCode::WrongOwner);
        const auto parentWords = access.read_words(parent._stateOffset, 32);
        require(parentWords[0] == result._words[7] && (parentWords[6] & task_attached_flag) != 0 &&
                    parentWords[3] == result._words[3],
                ErrorCode::WrongOwner, "publication.producer", "Thread producer belongs to an ended parent Process claim");
      }
      return result;
    }

    void check_publication_id(const ProducerContext& context, const TaskClaim& producer,
                                 PublicationId publication, bool fresh)
    {
      const auto& owner = publication._producer;
      require(owner._incarnation != 0 && publication._sequence != 0,
              ErrorCode::InvalidArgument, "publication.id", "Publication incarnation and sequence must be nonzero");
      require(owner._session == producer._session && owner._task == producer._task &&
                  (fresh ? owner._incarnation == producer._incarnation : owner._incarnation <= producer._incarnation),
              ErrorCode::WrongOwner, "publication.id", "Publication does not belong to the permitted producer claim");
      require(owner._incarnation != producer._incarnation || publication._sequence <= context._words[1],
              ErrorCode::InvalidArgument, "publication.id", "Publication sequence has not been issued by this claim");
    }

    struct StoredPublication
    {
      std::uint64_t _reference{};
      mapped::PublicationMetadata _metadata;
    };

    std::optional<StoredPublication> find_publication(RuntimeAccess& access, PublicationId publication)
    {
      ANGARIA_DIAGNOSTIC_SCOPE(PublicationLookup);
      ANGARIA_DIAGNOSTIC_COUNT(PublicationLookups, 1);
      for (const auto reference : access.record_references(9))
      {
        ANGARIA_DIAGNOSTIC_COUNT(LookupPublications, 1);
        auto metadata = runtime_publication_metadata(access, reference);
        if (metadata._producer == publication._producer._task &&
            metadata._producerIncarnation == publication._producer._incarnation &&
            metadata._publicationSequence == publication._sequence)
        {
          return StoredPublication{reference, std::move(metadata)};
        }
      }
      return std::nullopt;
    }

    PublicationReport output_too_small(PublicationId publication, std::uint64_t revision,
                                          std::uint32_t required)
    {
      return {publication, PublicationStatus::OutcomeUnknown, revision, {}, required,
              ErrorCode::OutputBufferTooSmall};
    }

    PublicationReport accepted_report(PublicationId publication, std::uint64_t revision,
                                         QueueId queue, std::span<DestinationOutcome> outcomes)
    {
      outcomes[0] = {queue, DestinationStatus::Accepted, ErrorCode::None};
      return {publication, PublicationStatus::Accepted, revision, outcomes.first(1), 1, ErrorCode::None};
    }

    PublicationReport stored_report(PublicationId publication, const mapped::PublicationMetadata& metadata,
                                       std::span<DestinationOutcome> outcomes)
    {
      const auto count = static_cast<std::uint32_t>(metadata._destinations.size());
      if (outcomes.size() < count)
      {
        return output_too_small(publication, metadata._topologyRevision, count);
      }
      // The core capability admits direct, one-destination Publications only.
      // Retained receipt progress does not alter the original acceptance result.
      return accepted_report(publication, metadata._topologyRevision,
                               metadata._destinations.front()._queue, outcomes);
    }

    void require_identical(RuntimeAccess& access, const StoredPublication& stored,
                               QueueId queue, MessageView message)
    {
      const auto& metadata = stored._metadata;
      const auto& header = message._header;
      require(metadata._publishingSet == PublishingQueueSetId::Invalid &&
                  metadata._destinations.size() == 1 && metadata._destinations.front()._queue == queue &&
                  metadata._schema == header._schema && metadata._format == header._format &&
                  metadata._flags == header._flags && metadata._payloadBytes == header._payloadBytes &&
                  metadata._payloadBytes == message._payload.size(),
              ErrorCode::Conflict, "publication.repeat", "Publication ID already identifies different endpoint or message content");
      const auto payload = runtime_value(map_publication_payload(access, stored._reference));
      const auto bytes = runtime_value(payload.bytes());
      require(std::equal(bytes.begin(), bytes.end(), message._payload.begin(), message._payload.end()),
              ErrorCode::Conflict, "publication.repeat", "Publication ID already identifies different payload bytes");
    }

    LoadedSchema validate_message(RuntimeAccess& access, MessageView message)
    {
      const auto& header = message._header;
      require(header._payloadBytes == message._payload.size(), ErrorCode::InvalidArgument,
              "message.payload_bytes", "Message header byte count must equal its payload span");
      require(wire::encode(header._format, wire::payload_formats).has_value(), ErrorCode::InvalidArgument,
              "message.format", "Message payload format is unknown");
      auto context = runtime_schema_context(access, header._schema);
      if (header._format == PayloadFormat::Binary)
      {
        const auto codec = runtime_payload_codec(context._schema, context._fields,
            access._session->_controller->configuration()._maxLogBytes);
        runtime_value(codec.validate(message._payload));
      }
      return std::move(context._schema);
    }
  } // namespace

  PublicationReport runtime_send(RuntimeAccess& access, const TaskClaim& producer,
                                    QueueId queue, PublicationId publication, MessageView message,
                                    std::span<DestinationOutcome> outcomes,
                                    const std::shared_ptr<TaskState>& actor)
  {
    ANGARIA_DIAGNOSTIC_SCOPE(PublicationPlan);
    const auto context = producer_context(access, producer);
    if (actor)
    {
      require(actor->_claim == producer, ErrorCode::WrongOwner, "publication.producer",
              "Publication actor does not match its captured producer claim");
      runtime_check_task(access, actor);
    }
    check_publication_id(context, producer, publication, true);
    const auto& loaded = require_registered_entity(context._topology._queues, queue,
        "publication.queue", "Queue identity must be nonzero", "Queue is not registered");
    require(loaded._definition._descriptor._producer == producer._task, ErrorCode::WrongOwner,
            "publication.queue", "Producer Task does not own this Queue");
    require(loaded._definition._state == EntityState::Active, ErrorCode::NotActive,
            "publication.queue", "Direct send requires an Active Queue");

    // A repeat is a lookup, not a fresh acceptance. In particular, exhausted
    // append space and fresh counters must not erase a recorded acceptance.
    if (const auto stored = find_publication(access, publication))
    {
      if (outcomes.empty())
      {
        return output_too_small(publication, stored->_metadata._topologyRevision, 1);
      }
      require_identical(access, *stored, queue, message);
      return stored_report(publication, stored->_metadata, outcomes);
    }

    // A recorded identical send is observational, just like status. Only new
    // acceptance advances Task delivery work and must settle abandoned Sources.
    if (actor)
    {
      runtime_reconcile_sources(access, actor);
    }

    const auto revision = loaded._definition._registration._revision;
    if (outcomes.empty())
    {
      return output_too_small(publication, revision, 1);
    }
    const auto schema = validate_message(access, message);
    const auto words = access.read_words(loaded._stateOffset, 12);
    require(words[0] != std::numeric_limits<std::uint64_t>::max() &&
                access._snapshot._generation != std::numeric_limits<std::uint64_t>::max(),
            ErrorCode::CapacityExceeded, "publication.counter", "Queue sequence or operation generation is exhausted");
    const auto reference = access._snapshot._tail;
    const auto maxLogBytes = access._session->_controller->configuration()._maxLogBytes;
    auto planned = mapped::plan_publication_layout(reference, 1, message._payload.size(), maxLogBytes);
    if (std::holds_alternative<Error>(planned))
    {
      runtime_fail(ErrorCode::LogFull, "session.max_log_bytes", "Complete Publication exceeds its byte ceiling");
    }
    const auto layout = std::get<mapped::PublicationLayout>(planned);
    mapped::AppendLayoutPlanner planner(reference);
    (void)planner.add_record(layout._immutableBytes, mapped::receipt_bytes);
    // Three WriteRanges: seven Queue producer words, the oldest pending link,
    // and four ring words. Each command has a 16-byte frame.
    constexpr std::uint64_t commandBytes = (16 + 7 * 8) + (16 + 8) + (16 + 4 * 8);
    require(planner.finish(commandBytes, maxLogBytes).has_value(), ErrorCode::LogFull,
            "session.max_log_bytes", "Complete Publication, receipt, manifest, and redo exceed the log byte ceiling");

    const auto sequence = words[0] + 1;
    const auto capacity = words[3];
    const auto index = (sequence - 1) % capacity;
    const auto publishedAt = UtcTimestamp{std::chrono::nanoseconds{runtime_now_ns()}};
    mapped::PublicationDefinition definition;
    definition._producer = producer._task;
    definition._schema = message._header._schema;
    definition._format = message._header._format;
    definition._flags = message._header._flags;
    definition._producerIncarnation = producer._incarnation;
    definition._publicationSequence = publication._sequence;
    definition._publishedAt = publishedAt;
    definition._topologyRevision = revision;
    definition._schemaReference = schema._reference;
    definition._destinations.push_back({queue, DestinationStatus::Accepted, ErrorCode::None,
        sequence, words[1], publishedAt, layout._receiptsOffset});
    // No payload-sized allocation happens until the entire append, including
    // the manifest and absolute redo, has passed the byte ceiling above.
    if (!message._payload.empty())
    {
      definition._payload.assign(message._payload.begin(), message._payload.end());
    }
    mapped::JournalRecord record;
    record._kind = 9;
    record._immutable = runtime_value(mapped::encode_publication_body(definition, reference, maxLogBytes));
    mapped::Receipt receipt;
    receipt._state = mapped::ReceiptState::Pending;
    const auto receiptBytes = runtime_value(mapped::encode_receipt(receipt));
    record._mutable.assign(receiptBytes.begin(), receiptBytes.end());

    const std::array<std::uint64_t, 7> producerWords{sequence, layout._destinationsOffset, words[2],
        capacity, producer._incarnation, 1, words[0] == 0 ? layout._destinationsOffset : words[6]};
    const std::array<std::uint64_t, 1> pending{words[9] == 0 ? layout._destinationsOffset : words[9]};
    const mapped::RingSlot slot{sequence, layout._destinationsOffset, reference, 1 + (sequence - 1) / capacity};
    const auto slotBytes = runtime_value(mapped::encode_ring_slot(slot, capacity, index));
    mapped::RedoCommand ring;
    ring._target = words[2] + mapped::record_envelope_bytes + index * mapped::ring_slot_bytes;
    ring._payload.assign(slotBytes.begin(), slotBytes.end());
    mapped::TransactionPlan plan;
    plan._base = access._snapshot;
    plan._kind = mapped::JournalKind::Append;
    plan._subtype = mapped::TransactionSubtype::Publication;
    plan._actor = producer._task;
    plan._actorIncarnation = producer._incarnation;
    plan._newRevision = access._snapshot._revision;
    plan._records.push_back(std::move(record));
    plan._commands.push_back(runtime_write_words(loaded._stateOffset, producerWords));
    plan._commands.push_back(runtime_write_words(loaded._stateOffset + 9 * 8, pending));
    plan._commands.push_back(std::move(ring));
    runtime_value(access.execute(plan));
    // Preparing this result cannot allocate or fail after committed authority.
    return accepted_report(publication, revision, queue, outcomes);
  }

  PublicationReport runtime_publication_status(RuntimeAccess& access, const TaskClaim& producer,
                                                  PublicationId publication,
                                                  std::span<DestinationOutcome> outcomes)
  {
    const auto context = producer_context(access, producer);
    check_publication_id(context, producer, publication, false);
    if (const auto stored = find_publication(access, publication))
    {
      return stored_report(publication, stored->_metadata, outcomes);
    }
    // Core history is never reclaimed. A recovered complete prefix therefore
    // proves absence, even for a retained earlier incarnation of this Task.
    return {publication, PublicationStatus::Rejected, access._snapshot._revision, {}, 0, ErrorCode::None};
  }
} // namespace angaria::detail
