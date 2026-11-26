// Public direct-send examples: issue a publication identity, send application
// bytes, then keep that identity when asking whether acceptance committed.
// The private snapshots below qualify storage/order guarantees; applications
// use publication_status(), not native offsets, to resolve an uncertain result.

#include "angaria/payload.h"
#include "angaria/runtime.h"
#include "mapped_payload.h"
#include "native_fixture.h"
#include "process_support.h"
#include "publication_runtime.h"
#include "result_assertions.h"
#include "topology_state.h"
#include "wire_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;

  using support::expect_error;
  using support::require_value;

  constexpr SchemaId text_schema{MessageType{1}, 1};

  MessageView prepared(std::span<const std::byte> payload, SchemaId schema = text_schema,
                         PayloadFormat format = PayloadFormat::Text, std::uint32_t flags = 0)
  {
    MessageHeader header;
    header._schema = schema;
    header._format = format;
    header._payloadBytes = payload.size();
    header._flags = flags;
    return MessageView{std::move(header), payload};
  }

  void expect_accepted(const PublicationOutcome& outcome, PublicationId publication, QueueId queue)
  {
    EXPECT_EQ(outcome._publication, publication);
    EXPECT_EQ(outcome._status, AcceptanceStatus::Accepted);
    ASSERT_EQ(outcome._destinations.size(), 1u);
    EXPECT_EQ(outcome._destinations.front()._queue, queue);
    EXPECT_EQ(outcome._destinations.front()._status, DestinationStatus::Accepted);
    EXPECT_EQ(outcome._destinations.front()._reason, ErrorCode::None);
  }

  // A private observation seam rejects any immutable read request overlapping
  // the application bytes. Registry/history metadata reads remain available.
  class PublicationPayloadReadGuard final : public runtime::RuntimeAccessReadObserver
  {
  public:
    PublicationPayloadReadGuard(std::uint64_t offset, std::uint64_t bytes)
        : _begin(offset), _end(offset + bytes)
    {
    }

    OperationResult before_read(std::uint64_t offset, std::uint64_t bytes) override
    {
      if (bytes != 0 && offset < _end && _begin < offset + bytes)
      {
        ++_payloadRequests;
        return Error{ErrorCode::IoError, "Status attempted to read application payload bytes", "test.status_payload"};
      }
      ++_metadataRequests;
      return std::monostate{};
    }

    std::uint64_t _payloadRequests{};
    std::uint64_t _metadataRequests{};

  private:
    std::uint64_t _begin;
    std::uint64_t _end;
  };

  class PublicationApiTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-send-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    CreatedSession create() const
    {
      SessionOptions options;
      options._name = "Direct publication examples";
      options._directory = _directory;
      options._maxLogBytes = 2097152;
      options._limits = CapacityLimits::empty().tasks(8).queues(8).source_sets(8)
          .memberships_per_set(8).fields(16).message_schemas(8);
      options._defaults._ringCapacity = 2;
      TaskDescriptor creator;
      creator._name = "Producer";
      auto created = require_value(Session::create(options, creator));
      MessageDescriptor message;
      message._schema = text_schema;
      message._name = "Notice";
      require_value(created._creator.registry().register_message(message));
      return created;
    }

    static QueueId queue(Registry registry, const std::string& name = "Updates",
                           TaskId producer = TaskId{1}, TaskId consumer = TaskId{1})
    {
      SourceQueueSetDescriptor source;
      source._name = name + "Input";
      source._owner = consumer;
      const auto source_id = require_value(registry.register_source_set(source));
      QueueDescriptor descriptor;
      descriptor._name = name;
      descriptor._producer = producer;
      descriptor._sourceSet = source_id;
      const auto id = require_value(registry.register_queue(descriptor));
      require_value(registry.activate_source_set(source_id));
      require_value(registry.activate_queue(id));
      return id;
    }

    static SchemaId schema(Registry registry, FieldDescriptor field, std::uint16_t type = 2)
    {
      const auto field_id = require_value(registry.register_field(field));
      MessageDescriptor descriptor;
      descriptor._schema = SchemaId{MessageType{type}, 1};
      descriptor._name = field._name + "Message";
      descriptor._fields = {field_id};
      return require_value(registry.register_message(descriptor));
    }

    std::vector<runtime::mapped::PublicationMetadata> history() const
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory));
      // Resolve the whole native history before inspecting its metadata. This
      // is private evidence that normal public sends produced valid records.
      (void)runtime::runtime_topology(access);
      std::vector<runtime::mapped::PublicationMetadata> result;
      for (const auto reference : access.record_references(9))
      {
        result.push_back(runtime::runtime_publication_metadata(access, reference));
      }
      return result;
    }

    fs::path _root;
    fs::path _directory;
  };

  // The producer's prepared header describes its application bytes. Recorded
  // provenance comes from this send's explicit identity and the native clock;
  // stale provenance on an application-owned input is never copied to storage.
  TEST_F(PublicationApiTest, SendsApplicationBytesAndPreservesThePreparedHeader)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    const auto publication = require_value(created._creator.next_publication_id());
    const auto payload = support::octets({0x41, 0x00, 0xff, 0x42});
    auto message = prepared(payload, text_schema, PayloadFormat::Text, 0xa5000001);
    message._header._publication = PublicationId{TaskClaim{SessionId{9, 8}, TaskId{7}, 6}, 5};
    message._header._publishedAt = UtcTimestamp{std::chrono::nanoseconds{-100}};
    message._header._queue = QueueId{99};
    message._header._queueSequence = 55;
    message._header._consumer = TaskClaim{SessionId{8, 9}, TaskId{7}, 6};
    message._header._enqueuedAt = UtcTimestamp{std::chrono::nanoseconds{-90}};
    message._header._dequeuedAt = UtcTimestamp{std::chrono::nanoseconds{-80}};
    const auto original = message._header;
    const auto queue_revision = require_value(created._creator.registry().metadata(id))._revision;
    const auto outcome = require_value(writer.send(publication, message));
    expect_accepted(outcome, publication, id);
    EXPECT_EQ(outcome._topologyRevision, queue_revision);
    EXPECT_EQ(message._header, original);
    expect_accepted(require_value(created._creator.publication_status(publication)), publication, id);

    const auto publications = history();
    ASSERT_EQ(publications.size(), 1u);
    const auto& stored = publications.front();
    EXPECT_EQ(stored._producer, publication._producer._task);
    EXPECT_EQ(stored._producerIncarnation, publication._producer._incarnation);
    EXPECT_EQ(stored._publicationSequence, publication._sequence);
    EXPECT_EQ(stored._schema, text_schema);
    EXPECT_EQ(stored._format, PayloadFormat::Text);
    EXPECT_EQ(stored._flags, original._flags);
    EXPECT_EQ(stored._payloadBytes, payload.size());
    ASSERT_EQ(stored._destinations.size(), 1u);
    EXPECT_EQ(stored._destinations.front()._enqueuedAt, stored._publishedAt);
    EXPECT_EQ(stored._destinations.front()._queueSequence, 1u);
    runtime::RuntimeAccess access(support::open_runtime_state(_directory));
    const auto reference = access.record_references(9).front();
    auto mapping = require_value(runtime::map_publication_payload(access, reference));
    const auto bytes = require_value(mapping.bytes());
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), payload.begin(), payload.end()));
  }

  // An identical retry answers the earlier acceptance. It neither assigns a
  // second Queue sequence nor reads timestamps from the caller's old header.
  TEST_F(PublicationApiTest, IdenticalRepeatAndLookupDoNotAppendOrChangeDeliveryState)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    const auto publication = require_value(created._creator.next_publication_id());
    const auto payload = support::octets({1, 2, 3});
    auto message = prepared(payload);
    const auto accepted = require_value(writer.send(publication, message));
    const auto evidence = support::recovered_image(_directory);
    message._header._publication = publication;
    message._header._publishedAt = UtcTimestamp{std::chrono::nanoseconds{-500}};
    message._header._queue = id;
    message._header._queueSequence = 99;
    const auto repeated = require_value(writer.send(publication, message));
    expect_accepted(repeated, publication, id);
    EXPECT_EQ(repeated._topologyRevision, accepted._topologyRevision);
    const auto found = require_value(created._creator.publication_status(publication));
    expect_accepted(found, publication, id);
    EXPECT_EQ(found._topologyRevision, accepted._topologyRevision);
    EXPECT_EQ(support::recovered_image(_directory), evidence);
    ASSERT_EQ(history().size(), 1u);
  }

  // Public sending establishes the retained message. The private observer then
  // exercises the same gate-bound engine used by public publication_status(),
  // proving that outcome lookup needs metadata even for a large retained body.
  TEST_F(PublicationApiTest, PublicationStatusInspectsMetadataWithoutReadingTheRetainedPayload)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    const auto publication = require_value(created._creator.next_publication_id());
    const std::vector<std::byte> payload(262144, std::byte{0x5a});
    expect_accepted(require_value(writer.send(publication, prepared(payload))), publication, id);
    const auto retained = history();
    ASSERT_EQ(retained.size(), 1u);
    const auto claim = created._creator.claim();
    PublicationPayloadReadGuard observer(retained.front()._layout._payloadOffset,
                                          retained.front()._payloadBytes);
    const auto evidence = support::recovered_image(_directory);
    {
      runtime::RuntimeAccess access(support::open_runtime_state(_directory), true, nullptr, &observer);
      std::array<DestinationOutcome, 1> outcomes;
      const auto status = runtime::runtime_publication_status(access, claim, publication, outcomes);
      ASSERT_EQ(status._error, ErrorCode::None);
      EXPECT_EQ(status._status, PublicationStatus::Accepted);
      ASSERT_EQ(status._destinations.size(), 1u);
      EXPECT_EQ(status._destinations.front()._queue, id);
    }
    EXPECT_GT(observer._metadataRequests, 0u);
    EXPECT_EQ(observer._payloadRequests, 0u);
    EXPECT_EQ(support::recovered_image(_directory), evidence);
  }

  // Acceptance is tied to exact application contents and the selected endpoint.
  // A changed repeat is Conflict, even when its replacement body would itself
  // fail fresh Binary validation or refer to a nonexistent exact schema.
  TEST_F(PublicationApiTest, ChangedRepeatsConflictWithoutReplacingTheOriginalAcceptance)
  {
    auto created = create();
    const auto first = queue(created._creator.registry());
    const auto second = queue(created._creator.registry(), "OtherUpdates");
    auto writer = require_value(created._creator.open_writer(first));
    auto other = require_value(created._creator.open_writer(second));
    const auto publication = require_value(created._creator.next_publication_id());
    const auto payload = support::octets({1, 2, 3});
    const auto changed = support::octets({1, 2, 4});
    const auto message = prepared(payload);
    require_value(writer.send(publication, message));
    const auto evidence = support::recovered_image(_directory);
    expect_error(other.send(publication, message), ErrorCode::Conflict);
    expect_error(writer.send(publication, prepared(changed)), ErrorCode::Conflict);
    expect_error(writer.send(publication, prepared(payload, SchemaId{MessageType{1}, 2})), ErrorCode::Conflict);
    expect_error(writer.send(publication, prepared(payload, text_schema, PayloadFormat::Binary)), ErrorCode::Conflict);
    expect_error(writer.send(publication, prepared(payload, text_schema, PayloadFormat::Text, 1)), ErrorCode::Conflict);
    expect_accepted(require_value(created._creator.publication_status(publication)), publication, first);
    EXPECT_EQ(support::recovered_image(_directory), evidence);
  }

  // The buffered overload borrows the caller's result buffer. Too little room
  // is an operation error, including after acceptance; always inspect _error
  // before interpreting a low-level PublicationReport's _status.
  TEST_F(PublicationApiTest, BufferedOperationsRequireRoomWithoutChangingPriorAcceptance)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    const auto publication = require_value(created._creator.next_publication_id());
    const auto payload = support::octets({8});
    const auto message = prepared(payload);
    const auto before = support::recovered_image(_directory);
    auto report = writer.send(publication, message, {});
    EXPECT_EQ(report._error, ErrorCode::OutputBufferTooSmall);
    EXPECT_EQ(report._requiredCapacity, 1u);
    EXPECT_EQ(support::recovered_image(_directory), before);
    std::array<DestinationOutcome, 1> output;
    report = writer.send(publication, message, output);
    ASSERT_EQ(report._error, ErrorCode::None);
    EXPECT_EQ(report._status, PublicationStatus::Accepted);
    ASSERT_EQ(report._destinations.size(), 1u);
    EXPECT_EQ(report._destinations.data(), output.data());
    EXPECT_EQ(report._requiredCapacity, 1u);
    EXPECT_EQ(output.front()._queue, id);
    const auto accepted = support::recovered_image(_directory);
    report = writer.send(publication, message, {});
    EXPECT_EQ(report._error, ErrorCode::OutputBufferTooSmall);
    report = created._creator.publication_status(publication, {});
    EXPECT_EQ(report._error, ErrorCode::OutputBufferTooSmall);
    EXPECT_EQ(report._requiredCapacity, 1u);
    report = created._creator.publication_status(publication, output);
    ASSERT_EQ(report._error, ErrorCode::None);
    EXPECT_EQ(report._status, PublicationStatus::Accepted);
    EXPECT_EQ(report._destinations.data(), output.data());
    EXPECT_EQ(support::recovered_image(_directory), accepted);
  }

  // An issued identity can be queried before use. With no reclamation in this
  // milestone, its proven absence means Rejected with no destination result.
  TEST_F(PublicationApiTest, UnusedIssuedIdentityHasAProvablyAbsentOutcome)
  {
    auto created = create();
    queue(created._creator.registry());
    const auto publication = require_value(created._creator.next_publication_id());
    const auto evidence = support::recovered_image(_directory);
    const auto outcome = require_value(created._creator.publication_status(publication));
    EXPECT_EQ(outcome._publication, publication);
    EXPECT_EQ(outcome._status, AcceptanceStatus::Rejected);
    EXPECT_TRUE(outcome._destinations.empty());
    const auto report = created._creator.publication_status(publication, {});
    EXPECT_EQ(report._error, ErrorCode::None);
    EXPECT_EQ(report._status, PublicationStatus::Rejected);
    EXPECT_EQ(report._requiredCapacity, 0u);
    EXPECT_TRUE(report._destinations.empty());
    EXPECT_EQ(support::recovered_image(_directory), evidence);
  }

  TEST_F(PublicationApiTest, RejectsUnissuedMalformedAndForeignIdentitiesWithoutStorageChanges)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    const auto issued = require_value(created._creator.next_publication_id());
    const auto payload = support::octets({1});
    const auto message = prepared(payload);
    const auto evidence = support::recovered_image(_directory);
    auto invalid = issued;
    invalid._sequence = 0;
    expect_error(writer.send(invalid, message), ErrorCode::InvalidArgument);
    expect_error(created._creator.publication_status(invalid), ErrorCode::InvalidArgument);
    invalid = issued;
    ++invalid._sequence;
    expect_error(writer.send(invalid, message), ErrorCode::InvalidArgument);
    expect_error(created._creator.publication_status(invalid), ErrorCode::InvalidArgument);
    invalid = issued;
    invalid._producer._incarnation = 0;
    expect_error(writer.send(invalid, message), ErrorCode::InvalidArgument);
    invalid = issued;
    ++invalid._producer._incarnation;
    expect_error(writer.send(invalid, message), ErrorCode::WrongOwner);
    expect_error(created._creator.publication_status(invalid), ErrorCode::WrongOwner);
    invalid = issued;
    ++invalid._producer._session._low;
    expect_error(writer.send(invalid, message), ErrorCode::WrongOwner);
    expect_error(created._creator.publication_status(invalid), ErrorCode::WrongOwner);
    invalid = issued;
    invalid._producer._task = TaskId{2};
    expect_error(writer.send(invalid, message), ErrorCode::WrongOwner);
    expect_error(created._creator.publication_status(invalid), ErrorCode::WrongOwner);
    EXPECT_EQ(support::recovered_image(_directory), evidence);
  }

  // Issuing IDs and accepting messages are separate operations. Applications
  // may prepare two IDs and send them out of order; the Queue follows commit
  // order and still assigns the contiguous sequence 1, 2.
  TEST_F(PublicationApiTest, AcceptanceOrderIsIndependentOfIdentityIssuanceOrder)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    const auto first = require_value(created._creator.next_publication_id());
    const auto second = require_value(created._creator.next_publication_id());
    const auto payload = support::octets({3});
    expect_accepted(require_value(writer.send(second, prepared(payload))), second, id);
    expect_accepted(require_value(writer.send(first, prepared(payload))), first, id);
    const auto publications = history();
    ASSERT_EQ(publications.size(), 2u);
    EXPECT_EQ(publications[0]._publicationSequence, second._sequence);
    EXPECT_EQ(publications[0]._destinations.front()._queueSequence, 1u);
    EXPECT_EQ(publications[1]._publicationSequence, first._sequence);
    EXPECT_EQ(publications[1]._destinations.front()._queueSequence, 2u);
  }

  // The ring caches only two recent references. A consumer need not be running
  // for acceptance, and wrapping that cache cannot remove the older log entry.
  TEST_F(PublicationApiTest, OfflineConsumerAndWrappedRingRetainEveryAcceptedPublication)
  {
    auto created = create();
    TaskDescriptor consumer;
    consumer._name = "OfflineConsumer";
    const auto consumer_id = require_value(created._creator.registry().register_task(consumer));
    const auto id = queue(created._creator.registry(), "MarketData", TaskId{1}, consumer_id);
    auto writer = require_value(created._creator.open_writer(id));
    const auto payload = support::octets({0x61});
    std::vector<PublicationId> publications;
    for (unsigned index = 0; index != 7; ++index)
    {
      const auto publication = require_value(created._creator.next_publication_id());
      expect_accepted(require_value(writer.send(publication, prepared(payload))), publication, id);
      publications.push_back(publication);
    }
    EXPECT_FALSE(require_value(created._session.registry().task_status(consumer_id))._attached);
    for (const auto publication : publications)
    {
      expect_accepted(require_value(created._creator.publication_status(publication)), publication, id);
    }
    const auto retained = history();
    ASSERT_EQ(retained.size(), publications.size());
    for (std::size_t index = 0; index != retained.size(); ++index)
    {
      EXPECT_EQ(retained[index]._destinations.front()._queueSequence, index + 1);
    }
  }

  // Logical Task identity survives process restarts; the incarnation separates
  // each owner's issued IDs. Replacement may inspect old accepted or absent
  // IDs, but cannot turn an unused old ID into a new send.
  TEST_F(PublicationApiTest, ReplacementTaskCanLookUpOldIncarnationWithoutReusingItsUnusedIdentity)
  {
    PublicationId accepted;
    PublicationId absent;
    QueueId id{};
    {
      auto created = create();
      id = queue(created._creator.registry());
      auto writer = require_value(created._creator.open_writer(id));
      accepted = require_value(created._creator.next_publication_id());
      absent = require_value(created._creator.next_publication_id());
      const auto payload = support::octets({4});
      expect_accepted(require_value(writer.send(accepted, prepared(payload))), accepted, id);
    }
    auto session = require_value(Session::attach(_directory));
    auto replacement = require_value(session.attach_task(TaskId{1}));
    EXPECT_GT(replacement.claim()._incarnation, accepted._producer._incarnation);
    auto writer = require_value(replacement.open_writer(id));
    expect_accepted(require_value(replacement.publication_status(accepted)), accepted, id);
    const auto missing = require_value(replacement.publication_status(absent));
    EXPECT_EQ(missing._status, AcceptanceStatus::Rejected);
    EXPECT_TRUE(missing._destinations.empty());
    const auto evidence = support::recovered_image(_directory);
    const auto payload = support::octets({4});
    expect_error(writer.send(absent, prepared(payload)), ErrorCode::WrongOwner);
    expect_error(writer.send(accepted, prepared(payload)), ErrorCode::WrongOwner);
    EXPECT_EQ(support::recovered_image(_directory), evidence);
    const auto fresh = require_value(replacement.next_publication_id());
    EXPECT_EQ(fresh._sequence, 1u);
    expect_accepted(require_value(writer.send(fresh, prepared(payload))), fresh, id);
  }

  TEST_F(PublicationApiTest, ValidatesExactSchemaKnownFormatAndDeclaredPayloadLengthBeforeAcceptance)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    const auto publication = require_value(created._creator.next_publication_id());
    const auto payload = support::octets({0xff});
    const auto evidence = support::recovered_image(_directory);
    expect_error(writer.send(publication, prepared(payload, SchemaId{MessageType{1}, 2})), ErrorCode::NotFound);
    expect_error(writer.send(publication, prepared(payload, text_schema, static_cast<PayloadFormat>(99))), ErrorCode::InvalidArgument);
    auto wrong_length = prepared(payload);
    ++wrong_length._header._payloadBytes;
    expect_error(writer.send(publication, wrong_length), ErrorCode::InvalidArgument);
    expect_error(writer.send(publication, prepared(payload, text_schema, PayloadFormat::Binary)), ErrorCode::InvalidArgument);
    EXPECT_EQ(support::recovered_image(_directory), evidence);
    // Text is intentionally opaque; Angaria does not impose UTF-8 or a text
    // serializer on a correctly declared application byte span.
    expect_accepted(require_value(writer.send(publication, prepared(payload))), publication, id);
  }

  TEST_F(PublicationApiTest, ValidatesBooleanClosedEnumAndFixedAsciiBeforeAnyPublicationWrite)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    FieldDescriptor boolean;
    boolean._id = FieldId{0xee000001};
    boolean._name = "Ready";
    boolean._base = EFieldBase::BASE_BYTE;
    boolean._size = 1;
    boolean._logical._kind = FieldLogicalKind::Boolean;
    const auto bool_schema = schema(created._creator.registry(), boolean, 2);
    auto enumeration = boolean;
    enumeration._id = FieldId{0xee000002};
    enumeration._name = "Side";
    enumeration._logical._kind = FieldLogicalKind::Enumeration;
    enumeration._logical._enumerators = {{"Buy", std::uint64_t{1}}, {"Sell", std::uint64_t{2}}};
    const auto enum_schema = schema(created._creator.registry(), enumeration, 3);
    auto ascii = boolean;
    ascii._id = FieldId{0xee000003};
    ascii._name = "Ticker";
    ascii._base = EFieldBase::BASE_CHARS;
    ascii._size = 4;
    ascii._logical._kind = FieldLogicalKind::FixedAscii;
    const auto ascii_schema = schema(created._creator.registry(), ascii, 4);
    const auto publication = require_value(created._creator.next_publication_id());
    const auto evidence = support::recovered_image(_directory);
    const auto invalid_bool = support::octets({2});
    const auto invalid_enum = support::octets({3});
    const auto invalid_ascii = support::octets({0x41, 0, 0x80, 0});
    expect_error(writer.send(publication, prepared(invalid_bool, bool_schema, PayloadFormat::Binary)), ErrorCode::InvalidArgument);
    expect_error(writer.send(publication, prepared(invalid_enum, enum_schema, PayloadFormat::Binary)), ErrorCode::InvalidArgument);
    expect_error(writer.send(publication, prepared(invalid_ascii, ascii_schema, PayloadFormat::Binary)), ErrorCode::InvalidArgument);
    EXPECT_EQ(support::recovered_image(_directory), evidence);
    const auto valid = support::octets({1});
    expect_accepted(require_value(writer.send(publication, prepared(valid, bool_schema, PayloadFormat::Binary))), publication, id);
  }

  // Ordinary char bytes are not FixedAscii, and IEEE NaNs are valid float
  // payloads. Admission checks each registered Field's own representation.
  TEST_F(PublicationApiTest, BinaryAdmissionPreservesOrdinaryCharAndIeeeNanBits)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    FieldDescriptor character;
    character._id = FieldId{0xee000001};
    character._name = "RawCharacter";
    character._base = EFieldBase::BASE_CHAR;
    character._size = 1;
    const auto char_schema = schema(created._creator.registry(), character, 2);
    FieldDescriptor floating;
    floating._id = FieldId{0xee000002};
    floating._name = "OptionalPrice";
    floating._base = EFieldBase::BASE_FLOAT64;
    floating._size = 8;
    const auto float_schema = schema(created._creator.registry(), floating, 3);
    const auto raw_char = support::octets({0xff});
    const auto nan = support::octets({1, 0, 0, 0, 0, 0, 0xf8, 0x7f});
    const auto first = require_value(created._creator.next_publication_id());
    const auto second = require_value(created._creator.next_publication_id());
    expect_accepted(require_value(writer.send(first, prepared(raw_char, char_schema, PayloadFormat::Binary))), first, id);
    expect_accepted(require_value(writer.send(second, prepared(nan, float_schema, PayloadFormat::Binary))), second, id);
  }

  TEST_F(PublicationApiTest, EmptyPayloadAndEveryKnownOpaqueFormatCanBeAccepted)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    const auto empty = require_value(created._creator.next_publication_id());
    expect_accepted(require_value(writer.send(empty, prepared({}, text_schema, PayloadFormat::Binary))), empty, id);
    const auto malformed = support::octets({0xff, 0, '<', '{'});
    for (const auto format : {PayloadFormat::Json, PayloadFormat::Xml, PayloadFormat::Csv, PayloadFormat::Text})
    {
      const auto publication = require_value(created._creator.next_publication_id());
      expect_accepted(require_value(writer.send(publication, prepared(malformed, text_schema, format))), publication, id);
    }
    const auto publications = history();
    ASSERT_EQ(publications.size(), 5u);
    EXPECT_EQ(publications.front()._payloadBytes, 0u);
  }

  // Fresh acceptance needs room for its whole append, including its receipt
  // and journal manifest. A full log does not invalidate an earlier acceptance
  // or make querying/repeating it allocate another copy of the message.
  TEST_F(PublicationApiTest, LogFullRejectsFreshAcceptanceButIdenticalRepeatAndLookupStillWork)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    const auto accepted = require_value(created._creator.next_publication_id());
    const auto absent = require_value(created._creator.next_publication_id());
    const std::vector<std::byte> payload(1048576, std::byte{0x5a});
    const auto message = prepared(payload);
    expect_accepted(require_value(writer.send(accepted, message)), accepted, id);
    const auto evidence = support::recovered_image(_directory);
    expect_error(writer.send(absent, message), ErrorCode::LogFull);
    EXPECT_EQ(support::recovered_image(_directory), evidence);
    expect_accepted(require_value(writer.send(accepted, message)), accepted, id);
    expect_accepted(require_value(created._creator.publication_status(accepted)), accepted, id);
    EXPECT_EQ(require_value(created._creator.publication_status(absent))._status, AcceptanceStatus::Rejected);
    EXPECT_EQ(support::recovered_image(_directory), evidence);
  }

  // One producer process may send from several application threads. Two
  // separately opened writers for its Queue share the same claim; the Session
  // gate serializes acceptance and Queue order across both handles.
  TEST_F(PublicationApiTest, ProcessTaskWritersSerializeConcurrentApplicationThreads)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    auto second_writer = require_value(created._creator.open_writer(id));
    std::array<PublicationId, 8> publications;
    for (auto& publication : publications)
    {
      publication = require_value(created._creator.next_publication_id());
    }
    const auto payload = support::octets({1, 2});
    std::array<bool, 8> accepted{};
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index != publications.size(); ++index)
    {
      threads.emplace_back([&, index]
      {
        auto& selected = index % 2 == 0 ? writer : second_writer;
        auto result = selected.send(publications[index], prepared(payload));
        accepted[index] = std::holds_alternative<PublicationOutcome>(result) &&
            std::get<PublicationOutcome>(result)._status == AcceptanceStatus::Accepted;
      });
    }
    for (auto& thread : threads)
    {
      thread.join();
    }
    EXPECT_TRUE(std::all_of(accepted.begin(), accepted.end(), [](bool value) { return value; }));
    const auto stored = history();
    ASSERT_EQ(stored.size(), publications.size());
    for (std::size_t index = 0; index != stored.size(); ++index)
    {
      EXPECT_EQ(stored[index]._destinations.front()._queueSequence, index + 1);
    }
  }

  TEST_F(PublicationApiTest, ThreadOwnedWriterRejectsOtherThreadsWithoutChangingTheNativeSession)
  {
    auto created = create();
    TaskDescriptor descriptor;
    descriptor._name = "WorkerThread";
    descriptor._kind = TaskKind::Thread;
    descriptor._process = TaskId{1};
    const auto owner = require_value(created._creator.registry().register_task(descriptor));
    const auto id = queue(created._creator.registry(), "ThreadUpdates", owner);
    auto task = require_value(created._session.attach_task(owner));
    auto writer = require_value(task.open_writer(id));
    const auto publication = require_value(task.next_publication_id());
    const auto payload = support::octets({1});
    const auto evidence = support::recovered_image(_directory);
    std::thread other([&]
    {
      expect_error(writer.send(publication, prepared(payload)), ErrorCode::WrongOwner);
      expect_error(task.publication_status(publication), ErrorCode::WrongOwner);
    });
    other.join();
    EXPECT_EQ(support::recovered_image(_directory), evidence);
    expect_accepted(require_value(writer.send(publication, prepared(payload))), publication, id);
  }

  // Forked children must open their own Session/Task ownership. Merely holding
  // a parent's C++ object cannot authorize a send or a recovery lookup.
  TEST_F(PublicationApiTest, InheritedWriterAndTaskRejectBeforeTouchingNativeState)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    const auto publication = require_value(created._creator.next_publication_id());
    const auto payload = support::octets({1});
    const auto evidence = support::recovered_image(_directory);
    const auto child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0)
    {
      const auto send = writer.send(publication, prepared(payload));
      const auto lookup = created._creator.publication_status(publication);
      ::_exit(support::has_error(send, ErrorCode::WrongOwner) &&
          support::has_error(lookup, ErrorCode::WrongOwner) ? 0 : 1);
    }
    int status;
    try
    {
      status = support::wait_for_exit(child, "Cannot reap inherited-handle child",
          "Inherited-handle ownership rejection exceeded its deadline");
    }
    catch (...)
    {
      auto remaining = child;
      support::terminate(remaining,
          {"Cannot terminate inherited-handle child", "Cannot reap inherited-handle child",
           "Inherited-handle child termination expired", "Inherited-handle child was not killed"});
      throw;
    }
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(support::recovered_image(_directory), evidence);
    expect_accepted(require_value(writer.send(publication, prepared(payload))), publication, id);
  }

  TEST_F(PublicationApiTest, MovedFromWriterAndTaskReturnErrorsWithoutAffectingTheirNewOwners)
  {
    auto created = create();
    const auto id = queue(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(id));
    const auto publication = require_value(created._creator.next_publication_id());
    auto moved_writer = std::move(writer);
    auto moved_task = std::move(created._creator);
    const auto payload = support::octets({1});
    const auto evidence = support::recovered_image(_directory);
    expect_error(writer.send(publication, prepared(payload)), ErrorCode::InvalidState);
    expect_error(created._creator.publication_status(publication), ErrorCode::InvalidState);
    EXPECT_EQ(support::recovered_image(_directory), evidence);
    expect_accepted(require_value(moved_writer.send(publication, prepared(payload))), publication, id);
    expect_accepted(require_value(moved_task.publication_status(publication)), publication, id);
  }

  // This one test uses a deliberately offline historical-profile fixture, not
  // an application migration API. No native Publication exists before changing
  // the header. Legacy Registry/claim access remains available, while new
  // delivery operations must reject before writing receipt or Queue state.
  TEST_F(PublicationApiTest, LegacyProfilesPermitRegistryAccessButRejectNativeSendAndLookupWithoutWrites)
  {
    for (const auto minor : {0u, 1u})
    {
      SCOPED_TRACE(minor);
      _directory = _root / std::to_string(minor);
      QueueId id{};
      {
        auto created = create();
        id = queue(created._creator.registry());
      }
      {
        auto file = require_value(runtime::mapped::NativeFile::open_existing(
            _directory / "session.ang", runtime::mapped::FileAccess::ReadWrite));
        std::array<std::byte, 8> bytes{};
        support::put_integer(bytes, 0, minor, 2);
        require_value(file.write_exact(10, std::span{bytes}.first(2)));
        support::put_integer(bytes, 0, minor == 0 ? 1 : 3);
        require_value(file.write_exact(56, bytes));
      }
      auto session = require_value(Session::attach(_directory));
      EXPECT_EQ(require_value(session.registry().find_queue("Updates")), id);
      auto task = require_value(session.attach_task(TaskId{1}));
      auto writer = require_value(task.open_writer(id));
      const auto publication = require_value(task.next_publication_id());
      const auto payload = support::octets({1});
      const auto evidence = support::recovered_image(_directory);
      expect_error(writer.send(publication, prepared(payload)), ErrorCode::UnsupportedPolicy);
      expect_error(task.publication_status(publication), ErrorCode::UnsupportedPolicy);
      std::array<DestinationOutcome, 1> outcomes;
      EXPECT_EQ(writer.send(publication, prepared(payload), outcomes)._error, ErrorCode::UnsupportedPolicy);
      EXPECT_EQ(task.publication_status(publication, outcomes)._error, ErrorCode::UnsupportedPolicy);
      EXPECT_EQ(support::recovered_image(_directory), evidence);
    }
  }
} // namespace
