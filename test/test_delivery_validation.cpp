// Private native-delivery validation fixtures, not application send/receive
// examples. Public Registry APIs build the catalog; after all Task handles close,
// a test-only profile edit and journal append install future delivery records.
// Public Session attachment and Registry inspection must validate that history
// without changing a quiescent valid or rejected fixture.

#include "angaria/runtime.h"
#include "delivery_state.h"
#include "native_atomic.h"
#include "publication_metadata.h"
#include "topology_state.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace fs = std::filesystem;

  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  template <typename T>
  void expect_error(const Result<T>& result, ErrorCode code)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code) << error._path << ": " << error._message;
    EXPECT_FALSE(error._path.empty());
    EXPECT_FALSE(error._message.empty());
  }

  std::vector<std::byte> integer_bytes(std::uint64_t value, std::size_t width = 8)
  {
    std::vector<std::byte> bytes(width);
    for (std::size_t index = 0; index < width; ++index)
    {
      bytes[index] = static_cast<std::byte>((value >> (8 * index)) & 0xff);
    }
    return bytes;
  }

  std::uint64_t word(std::span<const std::byte> bytes)
  {
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8; ++index)
    {
      value |= std::uint64_t{std::to_integer<unsigned char>(bytes[index])} << (8 * index);
    }
    return value;
  }

  struct StoredPublication
  {
    std::uint64_t _reference{};
    std::uint64_t _link{};
    std::uint64_t _receipt{};
    std::uint64_t _totalBytes{};
    std::uint64_t _manifest{};
    std::uint64_t _payloadOffset{};
    std::uint64_t _payloadBytes{};
  };

  class PayloadReadGuard final : public runtime::RuntimeAccessReadObserver
  {
  public:
    explicit PayloadReadGuard(std::span<const StoredPublication> publications)
        : _publications(publications)
    {
    }

    OperationResult before_read(std::uint64_t offset, std::uint64_t length) override
    {
      for (const auto& publication : _publications)
      {
        if (length != 0 && publication._payloadBytes != 0 &&
            offset < publication._payloadOffset + publication._payloadBytes &&
            publication._payloadOffset < offset + length)
        {
          ++_rejectedReads;
          return Error{ErrorCode::IoError, "Metadata inspection attempted to read payload bytes", "test.payload_read"};
        }
      }
      ++_calls;
      _totalBytes += length;
      _maxBytes = std::max(_maxBytes, length);
      return std::monostate{};
    }

    std::uint64_t _calls{};
    std::uint64_t _totalBytes{};
    std::uint64_t _maxBytes{};
    std::uint64_t _rejectedReads{};

  private:
    std::span<const StoredPublication> _publications;
  };

  class DeliveryValidationTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-delivery-validation-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      _state.reset();
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    void create(unsigned publications = 1, std::uint64_t capacity = 2, bool reverseIds = false,
                  TaskKind consumerKind = TaskKind::Process, std::size_t payloadBytes = 0)
    {
      _capacity = capacity;
      _threadConsumer = consumerKind == TaskKind::Thread;
      // Setup and ID issuance are implemented public behavior. The producer
      // issues IDs before its claim closes; this fixture never calls send().
      {
        SessionOptions options;
        options._name = "Delivery history validation";
        options._directory = _directory;
        options._maxLogBytes = 2 * 1024 * 1024;
        options._limits = CapacityLimits::empty().tasks(4).queues(2).source_sets(2)
            .publishing_sets(1).memberships_per_set(2).message_schemas(2);
        options._defaults._ringCapacity = _capacity;
        TaskDescriptor creator;
        creator._name = "Setup";
        auto created = require_value(Session::create(options, creator));
        auto actor = created._creator.registry();
        TaskDescriptor producer;
        producer._name = "Feed";
        _producer = require_value(actor.register_task(producer));
        TaskDescriptor consumer;
        consumer._name = "Consumer";
        consumer._kind = consumerKind;
        if (_threadConsumer)
        {
          consumer._process = TaskId{1};
        }
        _consumer = require_value(actor.register_task(consumer));
        SourceQueueSetDescriptor source;
        source._name = "Inputs";
        source._owner = _consumer;
        _source = require_value(actor.register_source_set(source));
        source._name = "OtherInputs";
        source._owner = TaskId{1};
        _otherSource = require_value(actor.register_source_set(source));
        QueueDescriptor queue;
        queue._name = "FeedToConsumer";
        queue._producer = _producer;
        queue._sourceSet = _source;
        _queue = require_value(actor.register_queue(queue));
        MessageDescriptor message;
        message._schema = {MessageType{17}, 1};
        message._name = "Heartbeat";
        _schema = require_value(actor.register_message(message));
        message._schema._version = 2;
        _otherSchema = require_value(actor.register_message(message));
        _draftRevision = require_value(actor.snapshot())._revision;
        require_value(actor.activate_source_set(_source));
        require_value(actor.activate_queue(_queue));
        {
          auto feed = require_value(created._session.attach_task(_producer));
          for (unsigned index = 0; index < publications; ++index)
          {
            EXPECT_EQ(require_value(feed.next_publication_id())._sequence, index + 1);
          }
          auto receiver = require_value(created._session.attach_task(_consumer));
          EXPECT_EQ(receiver.claim()._incarnation, 1u);
        }
        auto state = std::make_shared<runtime::SessionState>();
        state->_directory = fs::canonical(_directory);
        state->_controller = require_value(mapped::SessionController::open_existing(_directory));
        EXPECT_EQ(state->_controller->configuration()._formatMinor, 1u);
        EXPECT_EQ(state->_controller->configuration()._requiredFeatures, 3u);
        runtime::RuntimeAccess access(state);
        const auto topology = runtime::runtime_topology(access);
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
          if (task._definition._id == TaskId{1})
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
        for (const auto& schema : runtime::runtime_schemas(access))
        {
          if (schema._definition._descriptor._schema == _schema)
          {
            _schemaReference = schema._reference;
          }
          else
          {
            _otherSchemaReference = schema._reference;
          }
        }
      }
      // Test-only admission fixture. Production creation remains 2.1; the
      // immutable header is edited only with every application owner closed.
      auto file = require_value(mapped::NativeFile::open_existing(
          _directory / "session.ang", mapped::FileAccess::ReadWrite));
      require_value(file.write_exact(10, integer_bytes(2, 2)));
      require_value(file.write_exact(56, integer_bytes(7)));
      file = {};
      reopen_state();
      for (unsigned index = 0; index < publications; ++index)
      {
        append_publication(index + 1, reverseIds ? publications - index : index + 1, payloadBytes);
      }
    }

    void reopen_state()
    {
      _state.reset();
      _state = std::make_shared<runtime::SessionState>();
      _state->_directory = fs::canonical(_directory);
      _state->_controller = require_value(mapped::SessionController::open_existing(_directory));
    }

    void append_publication(std::uint64_t sequence, std::uint64_t publicationSequence,
                              std::size_t payloadBytes = 0)
    {
      runtime::RuntimeAccess access(_state);
      const auto reference = access._snapshot._tail;
      constexpr std::uint64_t recordLimit = 2 * 1024 * 1024;
      const auto layout = require_value(mapped::plan_publication_layout(reference, 1, payloadBytes, recordLimit));
      mapped::PublicationDefinition definition;
      definition._producer = _producer;
      definition._schema = _schema;
      definition._producerIncarnation = 1;
      definition._publicationSequence = publicationSequence;
      definition._publishedAt = UtcTimestamp{std::chrono::nanoseconds{42}};
      definition._topologyRevision = _queueRevision;
      definition._schemaReference = _schemaReference;
      if (payloadBytes != 0)
      {
        // Opaque application Text bytes do not need the empty Heartbeat's Binary
        // layout. Nonzero sentinels expose accidental interpretation as padding.
        definition._format = PayloadFormat::Text;
        definition._payload.assign(payloadBytes, std::byte{0xa5});
        definition._payload.front() = std::byte{0x41};
        definition._payload.back() = std::byte{0x5a};
      }
      definition._destinations = {mapped::DestinationDefinition{
          _queue, DestinationStatus::Accepted, ErrorCode::None, sequence,
          _publications.empty() ? 0 : _publications.back()._link, definition._publishedAt,
          layout._receiptsOffset}};
      mapped::Receipt receipt;
      receipt._state = mapped::ReceiptState::Pending;
      const auto receiptBytes = require_value(mapped::encode_receipt(receipt));
      mapped::JournalRecord record;
      record._kind = 9;
      record._immutable = require_value(mapped::encode_publication_body(definition, reference, recordLimit));
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
      const std::array producer{sequence, layout._destinationsOffset, _ring, _capacity,
                                std::uint64_t{1}, std::uint64_t{1}, first};
      plan._commands.push_back(runtime::runtime_write_words(_queueState, producer));
      const std::array consumer{std::uint64_t{0}, first, std::uint64_t{0}, std::uint64_t{0}};
      plan._commands.push_back(runtime::runtime_write_words(_queueState + 64, consumer));
      const mapped::RingSlot slot{sequence, layout._destinationsOffset, reference,
                                   1 + (sequence - 1) / _capacity};
      const auto encodedSlot = require_value(mapped::encode_ring_slot(slot, _capacity, (sequence - 1) % _capacity));
      mapped::RedoCommand ring;
      ring._target = _ring + 64 + ((sequence - 1) % _capacity) * 32;
      ring._payload.assign(encodedSlot.begin(), encodedSlot.end());
      plan._commands.push_back(std::move(ring));
      const auto result = require_value(access._journal.execute(access._guard, plan));
      EXPECT_EQ(result._records.front(), reference);
      _publications.push_back({reference, layout._destinationsOffset, layout._receiptsOffset,
                                layout._totalBytes, result._manifest, layout._payloadOffset, payloadBytes});
    }

    void revise_queue_documentation()
    {
      // Private Registry append after publication: change only documentation,
      // retaining the old immutable definition used for acceptance context.
      runtime::RuntimeAccess access(_state);
      const auto topology = runtime::runtime_topology(access);
      const auto& queue = topology._queues.front();
      auto definition = queue._definition;
      definition._descriptor._doc = "Revised after publication";
      definition._registration._revision = access._snapshot._revision + 1;
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::Append;
      plan._actor = TaskId{1};
      plan._actorIncarnation = 1;
      plan._newRevision = definition._registration._revision;
      mapped::JournalRecord record;
      record._kind = 3;
      record._previous = queue._reference;
      record._immutable = require_value(mapped::encode_queue_definition(definition, access._snapshot._tail));
      plan._records.push_back(std::move(record));
      const std::array<std::uint64_t, 4> index{
          static_cast<std::uint16_t>(_queue), access._snapshot._tail, plan._newRevision, 2};
      plan._commands.push_back(runtime::runtime_write_words(queue._indexOffset, index));
      require_value(access._journal.execute(access._guard, plan));
    }

    void append_mislabeled_task(mapped::TransactionSubtype subtype)
    {
      // The Task body/index form a consistent catalog addition, but the batch
      // falsely claims delivery semantics while preserving the Registry revision.
      // This isolates transaction-kind validation from ordinary record framing.
      runtime::RuntimeAccess access(_state);
      mapped::TaskDefinition definition;
      definition._id = TaskId{4};
      definition._descriptor._name = "MislabeledTask";
      definition._registration = {TaskId{1}, UtcTimestamp{}, access._snapshot._revision};
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::Append;
      plan._subtype = subtype;
      plan._actor = TaskId{1};
      plan._actorIncarnation = 1;
      plan._newRevision = access._snapshot._revision;
      mapped::JournalRecord record;
      record._kind = 2;
      record._immutable = require_value(mapped::encode_task_definition(definition, access._snapshot._tail));
      plan._records.push_back(std::move(record));
      const std::array<std::uint64_t, 4> row{4, access._snapshot._tail, plan._newRevision, 0};
      plan._commands.push_back(runtime::runtime_write_words(
          access.section_offset(mapped::SectionKind::TaskIndex) + 3 * mapped::index_entry_bytes, row));
      require_value(access._journal.execute(access._guard, plan));
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

    void write(std::uint64_t offset, std::span<const std::byte> bytes)
    {
      auto guard = require_value(_state->_controller->acquire_mutation());
      auto* file = require_value(_state->_controller->file(guard));
      require_value(file->write_exact(offset, bytes));
    }

    void write_word(std::uint64_t offset, std::uint64_t value)
    {
      auto guard = require_value(_state->_controller->acquire_mutation());
      auto* file = require_value(_state->_controller->file(guard));
      auto mapping = require_value(file->map_read_write(offset, 8));
      auto bytes = require_value(mapping.writable_bytes());
      require_value(mapped::AtomicWord::bind(bytes, 0)).store_release(value);
    }

    void reject_word(std::uint64_t offset, std::uint64_t value, std::size_t width = 8,
                     ErrorCode code = ErrorCode::IoError)
    {
      SCOPED_TRACE(std::to_string(offset) + "/" + std::to_string(value));
      const auto original = read(offset, width);
      write(offset, integer_bytes(value, width));
      const auto before = image();
      expect_error(Session::attach(_directory), code);
      EXPECT_EQ(image(), before) << "Rejected attachment changed a recovered Session";
      write(offset, original);
    }

    void set_receipt(std::size_t index, const mapped::Receipt& receipt)
    {
      const auto bytes = require_value(mapped::encode_receipt(receipt));
      // Mutable state is installed word by word with the native adapter under
      // one held gate. This is a deliberate fixture, not a progress operation.
      auto guard = require_value(_state->_controller->acquire_mutation());
      auto* file = require_value(_state->_controller->file(guard));
      auto mapping = require_value(file->map_read_write(_publications.at(index)._receipt, 128));
      auto target = require_value(mapping.writable_bytes());
      for (std::size_t offset = 0; offset < bytes.size(); offset += 8)
      {
        require_value(mapped::AtomicWord::bind(target, offset)).store_release(word(std::span{bytes}.subspan(offset, 8)));
      }
    }

    mapped::Receipt historical_receipt(mapped::ReceiptState state = mapped::ReceiptState::Pending) const
    {
      mapped::Receipt receipt;
      receipt._state = state;
      receipt._consumerTask = _consumer;
      receipt._sourceSet = _source;
      receipt._presence = 1;
      receipt._consumerIncarnation = 1;
      receipt._issuance = 1;
      receipt._dequeuedAt = UtcTimestamp{std::chrono::nanoseconds{-7}};
      return receipt;
    }

    void install_issued(std::size_t index = 0)
    {
      set_receipt(index, historical_receipt(mapped::ReceiptState::Issued));
      // Model a process that stopped while owning this issuance. A recorded
      // attached bit is diagnostic; no live test Task or OS lease is fabricated.
      write_word(_consumerState + 48, 3);
      write_word(_consumerState + 72, 0);
      if (_threadConsumer)
      {
        write_word(_parentState + 48, 3);
        write_word(_parentState + 72, 0);
      }
      const auto& publication = _publications.at(index);
      const std::array<std::uint64_t, 8> source{
          1, 1, static_cast<std::uint16_t>(_queue) | (std::uint64_t{1} << 16), index + 1,
          publication._reference, publication._receipt, std::bit_cast<std::uint64_t>(std::int64_t{-7}),
          publication._link};
      for (std::size_t offset = 0; offset < source.size(); ++offset)
      {
        write_word(_sourceState + offset * 8, source[offset]);
      }
    }

    void acknowledge_all()
    {
      for (std::size_t index = 0; index < _publications.size(); ++index)
      {
        auto receipt = historical_receipt(mapped::ReceiptState::Acknowledged);
        receipt._presence = 7;
        receipt._issuance = index + 1;
        // Deliberately move UTC backward between acknowledgments. Queue order
        // identifies the latest acknowledgment; the largest UTC value does not.
        receipt._acknowledgedAt = UtcTimestamp{std::chrono::nanoseconds{-static_cast<std::int64_t>(index) * 10}};
        receipt._terminalAt = receipt._acknowledgedAt;
        set_receipt(index, receipt);
      }
      write_word(_sourceState, _publications.size());
      write_word(_sourceState + 8, 1);
      write_word(_queueState + 64, _publications.size());
      write_word(_queueState + 72, 0);
      write_word(_queueState + 80, std::bit_cast<std::uint64_t>(
          -static_cast<std::int64_t>(_publications.size() - 1) * 10));
      write_word(_queueState + 88, 1);
    }

    fs::path _root;
    fs::path _directory;
    std::shared_ptr<runtime::SessionState> _state;
    TaskId _producer{};
    TaskId _consumer{};
    SourceQueueSetId _source{};
    SourceQueueSetId _otherSource{};
    QueueId _queue{};
    SchemaId _schema{};
    SchemaId _otherSchema{};
    std::uint64_t _queueState{};
    std::uint64_t _sourceState{};
    std::uint64_t _producerState{};
    std::uint64_t _consumerState{};
    std::uint64_t _parentState{};
    std::uint64_t _ring{};
    std::uint64_t _queueRevision{};
    std::uint64_t _schemaReference{};
    std::uint64_t _otherSchemaReference{};
    std::uint64_t _draftRevision{};
    std::uint64_t _capacity{};
    bool _threadConsumer{};
    std::vector<StoredPublication> _publications;
  };

  TEST_F(DeliveryValidationTest, CommittedHistorySurvivesRingWrapAndSupportsRegistrySnapshots)
  {
    create(3);
    const auto before = image();
    auto attached = require_value(Session::attach(_directory));
    auto registry = attached.registry();
    EXPECT_EQ(require_value(registry.find_queue("FeedToConsumer")), _queue);
    EXPECT_EQ(require_value(registry.find_message("Heartbeat", 1)), _schema);
    const auto current = require_value(registry.snapshot());
    ASSERT_EQ(current._queues.size(), 1u);
    EXPECT_EQ(current._queues.front()._state, EntityState::Active);
    const auto historical = require_value(registry.snapshot(_draftRevision));
    ASSERT_EQ(historical._queues.size(), 1u);
    EXPECT_EQ(historical._queues.front()._state, EntityState::Draft);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, CoreProfileWithoutPublicationsRetainsExistingRegistryBehavior)
  {
    create(0);
    const auto before = image();
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().snapshot())._queues.size(), 1u);
    EXPECT_EQ(require_value(attached.registry().state(_queue)), EntityState::Active);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, MetadataInspectionNeverReadsOrAllocatesRetainedPayloadBytes)
  {
    const auto inspect = [&](std::size_t payloadBytes)
    {
      create(1, 2, false, TaskKind::Process, payloadBytes);
      const auto& publication = _publications.front();
      EXPECT_EQ(read(publication._payloadOffset, 1), std::vector<std::byte>{std::byte{0x41}});
      EXPECT_EQ(read(publication._payloadOffset + publication._payloadBytes - 1, 1),
                std::vector<std::byte>{std::byte{0x5a}});
      const auto before = image();
      PayloadReadGuard observer(_publications);
      std::array<std::uint64_t, 3> metrics{};
      {
        // This private observer rejects any immutable read intersecting payload
        // bytes before RuntimeAccess allocates its read buffer or performs I/O.
        // It makes this a deterministic regression, without RSS/timing limits.
        runtime::RuntimeAccess access(_state, true, nullptr, &observer);
        EXPECT_NO_THROW(static_cast<void>(runtime::runtime_topology(access)));
        EXPECT_EQ(observer._rejectedReads, 0u);
        EXPECT_LT(observer._maxBytes, 64u * 1024);
        EXPECT_LT(observer._totalBytes, 64u * 1024);
        metrics = {observer._calls, observer._totalBytes, observer._maxBytes};
        // Negative control: the observer really rejects a payload read, rather
        // than silently permitting it through an unused instrumentation hook.
        EXPECT_THROW(static_cast<void>(access.read_bytes(publication._payloadOffset, 1)), runtime::RuntimeFailure);
        EXPECT_EQ(observer._rejectedReads, 1u);
      }
      auto attached = require_value(Session::attach(_directory));
      auto registry = attached.registry();
      EXPECT_EQ(require_value(registry.find_queue("FeedToConsumer")), _queue);
      EXPECT_EQ(require_value(registry.source_members(_source)), std::vector<QueueId>{_queue});
      EXPECT_EQ(require_value(registry.snapshot())._queues.size(), 1u);
      EXPECT_EQ(image(), before);
      return metrics;
    };

    const auto small = inspect(3);
    _state.reset();
    fs::remove_all(_directory);
    _publications.clear();
    const auto large = inspect(1024 * 1024 + 3);
    // Both payloads need five immutable alignment bytes and the same metadata.
    // Metadata read volume and largest read do not grow with the payload plane.
    EXPECT_EQ(large, small);
  }

  TEST_F(DeliveryValidationTest, BoundedInspectionStillChecksPayloadSlicesAndImmutablePadding)
  {
    create(1, 2, false, TaskKind::Process, 3);
    const auto& publication = _publications.front();
    const auto payloadEnd = publication._payloadOffset + publication._payloadBytes;
    ASSERT_EQ(publication._receipt - payloadEnd, 5u);
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().find_queue("FeedToConsumer")), _queue);
    // Skipping application bytes must still reject noncanonical slices and each
    // nonzero alignment byte, preserving the recovered Session on every error.
    reject_word(publication._reference + 64 + 56, publication._payloadOffset + 1);
    reject_word(publication._reference + 64 + 64, publication._payloadBytes + 8);
    reject_word(publication._reference + 64 + 64, std::numeric_limits<std::uint64_t>::max());
    for (auto offset = payloadEnd; offset < publication._receipt; ++offset)
    {
      reject_word(offset, 1, 1);
    }
  }

  TEST_F(DeliveryValidationTest, IssuedPublicationIdsMayBeAcceptedInADifferentOrder)
  {
    // Queue order records acceptance order. Producer-issued IDs distinguish
    // requests and need not be accepted in the same order they were issued.
    create(2, 2, true);
    const auto before = image();
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().find_queue("FeedToConsumer")), _queue);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, LaterQueueDocumentationDoesNotChangeOriginalAcceptanceContext)
  {
    create();
    revise_queue_documentation();
    const auto before = image();
    auto attached = require_value(Session::attach(_directory));
    auto registry = attached.registry();
    EXPECT_EQ(require_value(registry.describe_queue(_queue))._doc, "Revised after publication");
    EXPECT_EQ(require_value(registry.metadata(_queue))._revision, _queueRevision + 1);
    const auto historical = require_value(registry.snapshot(_queueRevision));
    ASSERT_EQ(historical._queues.size(), 1u);
    EXPECT_TRUE(historical._queues.front()._descriptor._doc.empty());
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, ReleasedPendingRetainsConsumerHistoryWithoutAnInflightPin)
  {
    create();
    set_receipt(0, historical_receipt());
    write_word(_sourceState, 1);
    write_word(_sourceState + 8, 1);
    const auto before = image();
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().source_members(_source)), std::vector<QueueId>{_queue});
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, ReleasedPendingCanFollowAnAcknowledgedPrefix)
  {
    create(3);
    auto acknowledged = historical_receipt(mapped::ReceiptState::Acknowledged);
    acknowledged._presence = 7;
    set_receipt(0, acknowledged);
    auto released = historical_receipt();
    released._issuance = 2;
    set_receipt(1, released);
    write_word(_sourceState, 2);
    write_word(_sourceState + 8, 1);
    write_word(_queueState + 64, 1);
    write_word(_queueState + 72, _publications[1]._link);
    write_word(_queueState + 88, 1);
    // Sequence 1 terminated, then sequence 2 was received and released. Its
    // retained dequeue tags are valid at the oldest nonterminal position;
    // sequence 3 remains an ordinary Pending receipt without dequeue history.
    const auto before = image();
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().state(_queue)), EntityState::Active);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, ReleasedPendingCannotAppearBeyondAnEarlierPendingDelivery)
  {
    create(2);
    set_receipt(1, historical_receipt());
    write_word(_sourceState, 1);
    write_word(_sourceState + 8, 1);
    // Both receipts are Pending, but only sequence 2 claims a previous dequeue.
    // Releasing a delivery cannot move selection past still-pending sequence 1,
    // so individually valid receipt/context bytes cannot establish this history.
    const auto before = image();
    const auto attached = Session::attach(_directory);
    expect_error(attached, ErrorCode::IoError);
    if (const auto* error = std::get_if<Error>(&attached))
    {
      EXPECT_EQ(error->_path, "queue.progress");
    }
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, AcknowledgedReceiptAndContiguousWatermarkRemainInspectable)
  {
    create();
    auto receipt = historical_receipt(mapped::ReceiptState::Acknowledged);
    receipt._presence = 7;
    receipt._acknowledgedAt = UtcTimestamp{};
    receipt._terminalAt = UtcTimestamp{};
    set_receipt(0, receipt);
    write_word(_sourceState, 1);
    write_word(_sourceState + 8, 1);
    write_word(_queueState + 64, 1);
    write_word(_queueState + 72, 0);
    write_word(_queueState + 88, 1);
    const auto before = image();
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().state(_queue)), EntityState::Active);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, RetainedPublicationsCanBelongToAnEarlierProducerIncarnation)
  {
    create();
    // Replacement resets this claim's issued-ID counter, but accepted messages
    // from its predecessor remain retained and distinguishable by incarnation.
    write_word(_producerState, 2);
    write_word(_producerState + 8, 0);
    write_word(_producerState + 16, 1);
    const auto before = image();
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().find_task("Feed")), _producer);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, LastAcknowledgmentUsesQueueOrderWhenUtcMovesBackward)
  {
    create(2);
    acknowledge_all();
    const auto before = image();
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().state(_queue)), EntityState::Active);
    EXPECT_EQ(image(), before);
    // Epoch zero is the first acknowledgment's real time, but it cannot stand
    // in for the most recently acknowledged Queue sequence's time of -10.
    reject_word(_queueState + 80, 0);
  }

  TEST_F(DeliveryValidationTest, HistoricalReceiptsSurviveConsumerCounterResetOnReplacement)
  {
    create();
    set_receipt(0, historical_receipt());
    write_word(_consumerState, 2);
    write_word(_consumerState + 16, 1);
    write_word(_sourceState + 8, 2);
    const auto before = image();
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().find_task("Consumer")), _consumer);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, IssuedReceiptMatchesExactlyOneSourcePin)
  {
    create();
    install_issued();
    const auto before = image();
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().find_source_set("Inputs")), _source);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, IssuedThreadConsumerRequiresItsMatchingRecordedProcessClaim)
  {
    create(1, 2, false, TaskKind::Thread);
    install_issued();
    // A terminated process can leave matching attached diagnostics for itself
    // and its child. Inspection accepts this pending-cleanup state without
    // pretending the recorded PID or flags establish a live OS claim.
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().find_task("Consumer")), _consumer);
    reject_word(_parentState, 2);
    reject_word(_parentState + 48, 6);
    reject_word(_parentState + 24, word(read(_parentState + 24, 8)) + 1);
  }

  TEST_F(DeliveryValidationTest, AcknowledgedThreadReceiptSurvivesParentProcessReplacement)
  {
    create(1, 2, false, TaskKind::Thread);
    acknowledge_all();
    write_word(_parentState, 2);
    write_word(_parentState + 16, 1);
    const auto before = image();
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().find_task("Consumer")), _consumer);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, ProducerIdentityAndIssuedSequenceMustMatchRegisteredClaimHistory)
  {
    create();
    const auto body = _publications[0]._reference + 64;
    reject_word(body, 99, 2);
    reject_word(body + 16, 2);
    reject_word(body + 24, 2);
    reject_word(_producerState, 0);
    reject_word(_producerState + 8, 0);
  }

  TEST_F(DeliveryValidationTest, ExactSchemaReferenceAndVersionCannotDisagree)
  {
    create();
    const auto body = _publications[0]._reference + 64;
    reject_word(body + 72, _otherSchemaReference);
    reject_word(body + 72, _ring);
    reject_word(body + 8, 99, 4);
    reject_word(body + 4, 18, 2);
  }

  TEST_F(DeliveryValidationTest, AcceptanceMustUseTheRecordedActiveQueueRevisionAndOwner)
  {
    create();
    const auto body = _publications[0]._reference + 64;
    reject_word(body + 40, _draftRevision);
    reject_word(body + 40, _queueRevision + 1);
    reject_word(body, static_cast<std::uint16_t>(_consumer), 2);
    reject_word(_publications[0]._link, 99, 2);
  }

  TEST_F(DeliveryValidationTest, PublicationIdentityIsUniqueAcrossCommittedRecords)
  {
    create(2);
    reject_word(_publications[1]._reference + 64 + 24, 1);
  }

  TEST_F(DeliveryValidationTest, PublicationManifestActorMustMatchItsProducerClaim)
  {
    create();
    const auto body = _publications[0]._manifest + 64;
    reject_word(body + 2, static_cast<std::uint16_t>(_consumer), 2);
    reject_word(body + 8, 2);
  }

  TEST_F(DeliveryValidationTest, PublicationCannotMasqueradeAsRegistryOrRetryTransaction)
  {
    create();
    const auto body = _publications[0]._manifest + 64;
    reject_word(body, 1, 2);
    reject_word(body, 3, 2, ErrorCode::UnsupportedPolicy);
  }

  TEST_F(DeliveryValidationTest, PublicationManifestCannotNameAnOtherwiseValidTaskAddition)
  {
    create(0);
    append_mislabeled_task(mapped::TransactionSubtype::Publication);
    const auto before = image();
    expect_error(Session::attach(_directory), ErrorCode::IoError);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, RetryManifestIsUnsupportedEvenWithoutAPublicationRecord)
  {
    create(0);
    append_mislabeled_task(mapped::TransactionSubtype::Retry);
    const auto before = image();
    expect_error(Session::attach(_directory), ErrorCode::UnsupportedPolicy);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, QueueChainIsContiguousAndUsesTheExactPreviousDestination)
  {
    create(3);
    reject_word(_publications[1]._link + 8, 3);
    reject_word(_publications[2]._link + 16, _publications[0]._link);
    reject_word(_queueState, 2);
    reject_word(_queueState + 8, _publications[0]._link);
    reject_word(_queueState + 32, 2);
    reject_word(_queueState + 40, 2);
    reject_word(_queueState + 48, _publications[1]._link);
  }

  TEST_F(DeliveryValidationTest, RingReferencesMustNameTheirExactCommittedQueueDelivery)
  {
    create(3);
    // Slot zero currently caches sequence 3; replacing it with sequence 1 would
    // remain a plausible old slot but violates the authoritative latest window.
    const auto slot = _ring + 64;
    reject_word(slot, 1);
    reject_word(slot + 8, _publications[1]._link);
    reject_word(slot + 16, _publications[1]._reference);
    reject_word(slot + 24, 1);
  }

  TEST_F(DeliveryValidationTest, ReceiptReservedBytesAndConsumerReferencesAreValidated)
  {
    create();
    set_receipt(0, historical_receipt());
    write_word(_sourceState, 1);
    write_word(_sourceState + 8, 1);
    const auto receipt = _publications[0]._receipt;
    reject_word(receipt + 8, 99, 2);
    reject_word(receipt + 10, static_cast<std::uint16_t>(_otherSource), 2);
    reject_word(receipt + 16, 2);
    reject_word(receipt + 24, 2);
    reject_word(receipt + 56, 1);
    reject_word(receipt + 120, 1);
  }

  TEST_F(DeliveryValidationTest, CurrentReceiptHistoryCannotLoseItsSourceIssuanceEvidence)
  {
    create();
    set_receipt(0, historical_receipt());
    // The current consumer incarnation has used this Source. Clearing both
    // counter and incarnation cannot make that same receipt a historical one.
    const auto before = image();
    expect_error(Session::attach(_directory), ErrorCode::IoError);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, IssuedReceiptCannotExistWithoutItsMatchingSourcePin)
  {
    create();
    install_issued();
    for (const auto offset : {0u, 8u, 16u, 24u, 32u, 40u, 48u, 56u})
    {
      reject_word(_sourceState + offset, 0);
    }
    reject_word(_sourceState + 16, static_cast<std::uint16_t>(_queue) | (std::uint64_t{2} << 16));
    reject_word(_sourceState + 64, 1);
  }

  TEST_F(DeliveryValidationTest, PendingReceiptCannotRetainAnActiveSourcePin)
  {
    create();
    install_issued();
    reject_word(_publications[0]._receipt, 1);
  }

  TEST_F(DeliveryValidationTest, IssuedReceiptMustBeTheOldestNonterminalQueueDelivery)
  {
    create(2);
    install_issued(1);
    const auto before = image();
    expect_error(Session::attach(_directory), ErrorCode::IoError);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, OneSourceCannotPinTwoIssuedReceipts)
  {
    create(2);
    install_issued(0);
    // A second Issued row with the same identity cannot share a Source slot,
    // even though both receipt bodies are individually valid representations.
    set_receipt(1, historical_receipt(mapped::ReceiptState::Issued));
    const auto before = image();
    expect_error(Session::attach(_directory), ErrorCode::IoError);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, IssuedReceiptCannotBelongToAnEndedConsumerClaim)
  {
    create();
    install_issued();
    reject_word(_consumerState + 48, 6);
    reject_word(_consumerState, 2);
  }

  TEST_F(DeliveryValidationTest, TerminalWatermarkCannotSkipAPendingDelivery)
  {
    create(2);
    reject_word(_queueState + 64, 1);
    reject_word(_queueState + 72, _publications[1]._link);
    reject_word(_queueState + 80, 1);
    reject_word(_queueState + 88, 1);
  }

  TEST_F(DeliveryValidationTest, AnAcknowledgedDeliveryCannotAppearBeyondAPendingHole)
  {
    create(2);
    auto receipt = historical_receipt(mapped::ReceiptState::Acknowledged);
    receipt._presence = 7;
    set_receipt(1, receipt);
    write_word(_sourceState, 1);
    write_word(_sourceState + 8, 1);
    write_word(_queueState + 88, 1);
    const auto before = image();
    expect_error(Session::attach(_directory), ErrorCode::IoError);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, CoreProfileHasNoPublicationRetirementFloor)
  {
    create();
    reject_word(_producerState + 64, 1);
  }

  TEST_F(DeliveryValidationTest, MutableReservedRowsAndAllocationPaddingRemainZero)
  {
    create();
    for (const auto offset : {56u, 96u, 128u, 248u})
    {
      reject_word(_queueState + offset, 1);
    }
    for (const auto offset : {64u, 120u, 128u, 248u})
    {
      reject_word(_sourceState + offset, 1);
    }
    const auto& publication = _publications[0];
    reject_word(publication._reference + publication._totalBytes - 8, 1);
  }

  TEST_F(DeliveryValidationTest, AnAlreadyOpenRegistryAlsoRejectsLaterDeliveryCorruption)
  {
    create();
    auto attached = require_value(Session::attach(_directory));
    auto registry = attached.registry();
    write_word(_queueState + 8, _publications[0]._link + 8);
    const auto before = image();
    expect_error(registry.find_queue("FeedToConsumer"), ErrorCode::IoError);
    expect_error(registry.snapshot(), ErrorCode::IoError);
    EXPECT_EQ(image(), before);
  }

  TEST_F(DeliveryValidationTest, OddCapacityRingPaddingIsNotAnotherDeliverySlot)
  {
    create(2, 1);
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().find_queue("FeedToConsumer")), _queue);
    reject_word(_ring + 96, 1);
  }

  TEST_F(DeliveryValidationTest, LegacyProfileCannotInterpretNativePublicationHistory)
  {
    create();
    _state.reset();
    {
      auto file = require_value(mapped::NativeFile::open_existing(
          _directory / "session.ang", mapped::FileAccess::ReadWrite));
      require_value(file.write_exact(10, integer_bytes(1, 2)));
      require_value(file.write_exact(56, integer_bytes(3)));
    }
    const auto before = image();
    expect_error(Session::attach(_directory), ErrorCode::UnsupportedPolicy);
    EXPECT_EQ(image(), before);
  }
} // namespace
