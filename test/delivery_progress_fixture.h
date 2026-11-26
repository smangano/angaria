#pragma once

#include "angaria/detail/append_layout.h"
#include "angaria/runtime.h"
#include "delivery_state.h"
#include "native_fixture.h"
#include "publication_metadata.h"
#include "topology_state.h"
#include "wire_fixture.h"

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace angaria::test_support
{
  namespace mapped = angaria::detail::mapped;

  struct ProgressPublication
  {
    std::uint64_t _reference{};
    std::uint64_t _link{};
    std::uint64_t _receipt{};
    std::uint64_t _payloadOffset{};
    std::uint64_t _payloadBytes{};
    std::uint64_t _totalBytes{};
  };

  // Captured compact cleanup intent shared with the independent crash worker.
  // Replacement adds the same absolute successor diagnostics as attachment,
  // without claiming that these private commands establish an OS Task lease.
  inline mapped::TransactionPlan progress_claim_plan(detail::RuntimeAccess& access,
                                                        TaskId task, bool replace = false)
  {
    const auto loaded = detail::runtime_task(access, task);
    auto words = access.read_words(loaded._stateOffset, 10);
    const auto ending = words[0];
    mapped::TransactionPlan plan;
    plan._base = access._snapshot;
    plan._actor = task;
    plan._actorIncarnation = ending + (replace ? 1 : 0);
    plan._newRevision = access._snapshot._revision;
    const std::array<std::uint64_t, 2> release{ending,
        std::bit_cast<std::uint64_t>(std::int64_t{-19})};
    auto command = detail::runtime_write_words(loaded._stateOffset, release);
    command._opcode = mapped::RedoOpcode::ReleaseClaim;
    plan._commands.push_back(std::move(command));
    if (replace)
    {
      words[0] = ending + 1;
      words[1] = 0;
      ++words[2];
      words[3] = static_cast<std::uint64_t>(::getpid());
      words[4] = detail::runtime_thread_id();
      words[5] = std::bit_cast<std::uint64_t>(std::int64_t{-19});
      words[6] = 7;
      words[8] = 0;
      words[9] = words[5];
      plan._commands.push_back(detail::runtime_write_words(loaded._stateOffset, words));
    }
    return plan;
  }

  // Private full-domain qualification fixture, not application initialization.
  // Public APIs create the real Registry, claims, exact schema and Active Queue.
  // After all public owners close, an offline profile edit and private journal
  // appends install core Publications. Delivery methods remain unimplemented.
  class DeliveryProgressFixture
  {
  public:
    explicit DeliveryProgressFixture(unsigned publications = 3,
                                        TaskKind consumerKind = TaskKind::Process,
                                        std::size_t payloadBytes = 9)
    {
      auto pattern = (std::filesystem::temp_directory_path() / "angaria-delivery-progress-XXXXXX").string();
      if (::mkdtemp(pattern.data()) == nullptr)
      {
        throw std::runtime_error("Cannot create delivery progress fixture directory");
      }
      _root = pattern;
      _directory = _root / "session";
      _threadConsumer = consumerKind == TaskKind::Thread;
      try
      {
        {
          SessionOptions options;
          options._name = "Reserved delivery progress";
          options._directory = _directory;
          options._maxLogBytes = 2 * 1024 * 1024;
          options._limits = CapacityLimits::empty().tasks(4).queues(2).source_sets(2)
              .publishing_sets(1).memberships_per_set(2).message_schemas(1);
          options._defaults._ringCapacity = 2;
          TaskDescriptor creator;
          creator._name = "Setup";
          auto created = require_value(Session::create(options, creator));
          auto registry = created._creator.registry();
          TaskDescriptor producer;
          producer._name = "Feed";
          _producer = require_value(registry.register_task(producer));
          TaskDescriptor consumer;
          consumer._name = "Consumer";
          consumer._kind = consumerKind;
          if (_threadConsumer)
          {
            consumer._process = _parent;
          }
          _consumer = require_value(registry.register_task(consumer));
          SourceQueueSetDescriptor source;
          source._name = "Inputs";
          source._owner = _consumer;
          _source = require_value(registry.register_source_set(source));
          source._name = "OtherInputs";
          source._owner = _parent;
          _otherSource = require_value(registry.register_source_set(source));
          QueueDescriptor queue;
          queue._name = "FeedToConsumer";
          queue._producer = _producer;
          queue._sourceSet = _source;
          _queue = require_value(registry.register_queue(queue));
          MessageDescriptor message;
          message._schema = {MessageType{17}, 1};
          message._name = "Heartbeat";
          _schema = require_value(registry.register_message(message));
          require_value(registry.activate_source_set(_source));
          require_value(registry.activate_queue(_queue));
          {
            auto feed = require_value(created._session.attach_task(_producer));
            for (unsigned index = 0; index != publications; ++index)
            {
              require_value(feed.next_publication_id());
            }
            auto receiver = require_value(created._session.attach_task(_consumer));
          }
          auto state = open_runtime_state(_directory);
          detail::RuntimeAccess access(state);
          const auto topology = detail::runtime_topology(access);
          const auto& storedQueue = topology._queues.front();
          _queueState = storedQueue._stateOffset;
          _queueRevision = storedQueue._definition._registration._revision;
          _ring = access.read_words(_queueState + 16, 1)[0];
          for (const auto& storedSource : topology._sources)
          {
            if (storedSource._definition._id == _source)
            {
              _sourceState = storedSource._stateOffset;
            }
          }
          for (const auto& task : topology._tasks)
          {
            if (task._definition._id == _parent)
            {
              _parentState = task._stateOffset;
            }
            if (task._definition._id == _producer)
            {
              _producerState = task._stateOffset;
            }
            if (task._definition._id == _consumer)
            {
              _consumerState = task._stateOffset;
            }
          }
          _schemaReference = detail::runtime_schemas(access).front()._reference;
        }
        // Deliberate offline capability fixture, never a supported migration.
        auto file = require_value(mapped::NativeFile::open_existing(
            _directory / "session.ang", mapped::FileAccess::ReadWrite));
        std::array<std::byte, 8> bytes{};
        put_integer(bytes, 0, 2, 2);
        require_value(file.write_exact(10, std::span{bytes}.first(2)));
        put_integer(bytes, 0, 7);
        require_value(file.write_exact(56, bytes));
        file = {};
        _state = open_runtime_state(_directory);
        _configuration = _state->_controller->configuration();
        for (unsigned index = 0; index != publications; ++index)
        {
          append_publication(index + 1, payloadBytes);
        }
      }
      catch (...)
      {
        cleanup();
        throw;
      }
    }

    ~DeliveryProgressFixture()
    {
      cleanup();
    }

    DeliveryProgressFixture(const DeliveryProgressFixture&) = delete;
    DeliveryProgressFixture& operator=(const DeliveryProgressFixture&) = delete;

    TaskClaim consumer_claim() const
    {
      return {_configuration._session, _consumer, 1};
    }

    void mark_consumer_attached()
    {
      // Model a crashed claimant's diagnostic state without fabricating an OS
      // lease. Private progress accepts a captured TaskClaim, not a public Task.
      auto guard = require_value(_state->_controller->acquire_mutation());
      auto* file = require_value(_state->_controller->file(guard));
      store_word(*file, _consumerState + 48, 3);
      store_word(*file, _consumerState + 72, 0);
      if (_threadConsumer)
      {
        store_word(*file, _parentState + 48, 3);
        store_word(*file, _parentState + 72, 0);
      }
    }

    void fill_log()
    {
      // A valid retained Queue revision consumes the exact remaining bytes.
      // No byte limit or Session metadata is falsified to manufacture LogFull.
      detail::RuntimeAccess access(_state);
      const auto topology = detail::runtime_topology(access);
      const auto& queue = topology._queues.front();
      const auto remaining = _configuration._maxLogBytes - access._snapshot._tail;
      constexpr std::uint64_t manifestBytes = 192; // fixed64 + ref8 + redo48 + envelope/padding.
      constexpr std::uint64_t queueFixedBytes = 80;
      const auto fixed = mapped::record_envelope_bytes + queueFixedBytes + queue._definition._descriptor._name.size();
      if (remaining < manifestBytes + fixed)
      {
        throw std::runtime_error("Insufficient fixture headroom for exact-fill Queue revision");
      }
      auto definition = queue._definition;
      definition._registration._revision = access._snapshot._revision + 1;
      definition._descriptor._doc.assign(static_cast<std::size_t>(remaining - manifestBytes - fixed), 'x');
      mapped::JournalRecord record;
      record._kind = 3;
      record._previous = queue._reference;
      record._immutable = require_value(mapped::encode_queue_definition(definition, access._snapshot._tail));
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::Append;
      plan._actor = _parent;
      plan._actorIncarnation = 1;
      plan._newRevision = definition._registration._revision;
      plan._records.push_back(std::move(record));
      const std::array<std::uint64_t, 4> row{static_cast<std::uint16_t>(_queue),
          access._snapshot._tail, plan._newRevision, 2};
      plan._commands.push_back(detail::runtime_write_words(queue._indexOffset, row));
      const auto result = require_value(access._journal.execute(access._guard, plan));
      if (result._tail != _configuration._maxLogBytes)
      {
        throw std::runtime_error("Queue revision did not exactly fill fixture log");
      }
    }

    std::vector<std::byte> image() const
    {
      auto file = require_value(mapped::NativeFile::open_existing(
          _directory / "session.ang", mapped::FileAccess::ReadOnly));
      std::vector<std::byte> bytes(static_cast<std::size_t>(require_value(file.size())));
      require_value(file.read_exact(0, bytes));
      return bytes;
    }

    std::vector<std::byte> read(std::uint64_t offset, std::size_t count) const
    {
      auto file = require_value(mapped::NativeFile::open_existing(
          _directory / "session.ang", mapped::FileAccess::ReadOnly));
      std::vector<std::byte> bytes(count);
      require_value(file.read_exact(offset, bytes));
      return bytes;
    }

    void write_word(std::uint64_t offset, std::uint64_t value)
    {
      auto guard = require_value(_state->_controller->acquire_mutation());
      auto* file = require_value(_state->_controller->file(guard));
      store_word(*file, offset, value);
    }

    mapped::Receipt receipt(std::size_t index = 0) const
    {
      detail::RuntimeAccess access(_state);
      const auto words = access.read_words(_publications.at(index)._receipt, 16);
      std::array<std::byte, mapped::receipt_bytes> bytes{};
      for (std::size_t word = 0; word != words.size(); ++word)
      {
        put_integer(bytes, word * 8, words[word]);
      }
      return require_value(mapped::decode_receipt(bytes));
    }

    void validate() const
    {
      detail::RuntimeAccess access(_state);
      (void)detail::runtime_topology(access);
    }

    std::filesystem::path _root;
    std::filesystem::path _directory;
    std::shared_ptr<detail::SessionState> _state;
    mapped::SuperblockConfiguration _configuration;
    TaskId _parent{1};
    TaskId _producer{};
    TaskId _consumer{};
    SourceQueueSetId _source{};
    SourceQueueSetId _otherSource{};
    QueueId _queue{};
    SchemaId _schema{};
    std::uint64_t _queueState{};
    std::uint64_t _sourceState{};
    std::uint64_t _producerState{};
    std::uint64_t _consumerState{};
    std::uint64_t _parentState{};
    std::uint64_t _ring{};
    std::uint64_t _queueRevision{};
    std::uint64_t _schemaReference{};
    bool _threadConsumer{};
    std::vector<ProgressPublication> _publications;

  private:
    static void store_word(mapped::NativeFile& file, std::uint64_t offset, std::uint64_t value)
    {
      auto region = require_value(file.map_read_write(offset, 8));
      require_value(mapped::AtomicWord::bind(require_value(region.writable_bytes()), 0)).store_release(value);
    }

    void append_publication(std::uint64_t sequence, std::size_t payloadBytes)
    {
      detail::RuntimeAccess access(_state);
      const auto reference = access._snapshot._tail;
      const auto layout = require_value(mapped::plan_publication_layout(
          reference, 1, payloadBytes, _configuration._maxLogBytes));
      mapped::PublicationDefinition definition;
      definition._producer = _producer;
      definition._schema = _schema;
      definition._producerIncarnation = 1;
      definition._publicationSequence = sequence;
      definition._publishedAt = UtcTimestamp{std::chrono::nanoseconds{42}};
      definition._topologyRevision = _queueRevision;
      definition._schemaReference = _schemaReference;
      if (payloadBytes != 0)
      {
        definition._format = PayloadFormat::Text;
        definition._payload.assign(payloadBytes, std::byte{0xa5});
        definition._payload.front() = std::byte{0x41};
        definition._payload.back() = std::byte{0x5a};
      }
      definition._destinations = {mapped::DestinationDefinition{
          _queue, DestinationStatus::Accepted, ErrorCode::None, sequence,
          _publications.empty() ? 0 : _publications.back()._link, definition._publishedAt,
          layout._receiptsOffset}};
      mapped::JournalRecord record;
      record._kind = 9;
      record._immutable = require_value(mapped::encode_publication_body(
          definition, reference, _configuration._maxLogBytes));
      mapped::Receipt pending;
      pending._state = mapped::ReceiptState::Pending;
      const auto receiptBytes = require_value(mapped::encode_receipt(pending));
      record._mutable.assign(receiptBytes.begin(), receiptBytes.end());
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::Append;
      plan._subtype = mapped::TransactionSubtype::Publication;
      plan._actor = _producer;
      plan._actorIncarnation = 1;
      plan._newRevision = access._snapshot._revision;
      plan._records.push_back(std::move(record));
      const auto first = _publications.empty() ? layout._destinationsOffset : _publications.front()._link;
      const std::array<std::uint64_t, 7> producer{sequence, layout._destinationsOffset, _ring, 2, 1, 1, first};
      plan._commands.push_back(detail::runtime_write_words(_queueState, producer));
      const std::array<std::uint64_t, 4> consumer{0, first, 0, 0};
      plan._commands.push_back(detail::runtime_write_words(_queueState + 64, consumer));
      const mapped::RingSlot slot{sequence, layout._destinationsOffset, reference, 1 + (sequence - 1) / 2};
      const auto encodedSlot = require_value(mapped::encode_ring_slot(slot, 2, (sequence - 1) % 2));
      mapped::RedoCommand ring;
      ring._target = _ring + 64 + ((sequence - 1) % 2) * 32;
      ring._payload.assign(encodedSlot.begin(), encodedSlot.end());
      plan._commands.push_back(std::move(ring));
      require_value(access._journal.execute(access._guard, plan));
      _publications.push_back({reference, layout._destinationsOffset, layout._receiptsOffset,
          layout._payloadOffset, payloadBytes, layout._totalBytes});
    }

    void cleanup() noexcept
    {
      _state.reset();
      std::error_code ignored;
      std::filesystem::remove_all(_root, ignored);
    }
  };
} // namespace angaria::test_support
