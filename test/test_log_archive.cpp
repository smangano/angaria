// Public replay/debugging examples. The fixture completes ordinary Registry
// definitions before admission; every archive, filter, message and export call
// below uses public Angaria APIs and checks its Result.
#include "angaria/log.h"
#include "angaria/capture.h"
#include "public_runtime_fixture.h"
#include "result_assertions.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace support = angaria::test_support;
  using support::require_value;
  using support::expect_error;
}

TEST(LogArchive, LiveCaptureFreezesAcceptedMessagesWithoutAdvancingProcessing)
{
  support::PublicRuntimeFixture fixture;
  auto& created = fixture.created();
  auto writer = require_value(created._creator.open_writer(fixture.queues().front()));
  auto consumer = require_value(created._session.attach_task(fixture.consumer_ids().front()));
  auto source = require_value(consumer.open_source_set(fixture.sources().front()));
  auto bytes = fixture.payload(17);
  const auto firstId = require_value(created._creator.next_publication_id());
  require_value(writer.send(firstId, fixture.message(bytes)));
  auto received = source.receive();
  ASSERT_TRUE(std::holds_alternative<Delivery>(received));
  const auto originalToken = std::get<Delivery>(received)._token;

  auto archive = require_value(LogArchive::capture_live(created._session));
  EXPECT_EQ(archive.publication_count(), 1u);
  auto newerBytes = fixture.payload(23, 1);
  const auto secondId = require_value(created._creator.next_publication_id());
  require_value(writer.send(secondId, fixture.message(newerBytes, 1)));
  auto cursor = require_value(archive.publications());
  auto first = require_value(cursor.read_next());
  ASSERT_TRUE(first);
  EXPECT_EQ(first->_message._header._publication, firstId);
  EXPECT_EQ(first->_message.view()._header, first->_message._header);
  EXPECT_FALSE(first->_message._header._consumer);
  ASSERT_EQ(first->_destinations.size(), 1u);
  EXPECT_EQ(first->_destinations.front()._queue, fixture.queues().front());
  EXPECT_EQ(first->_destinations.front()._queueSequence, 1u);
  EXPECT_EQ(require_value(first->_message.decode(fixture.codecs().front())),
            std::vector<PayloadValue>{std::int64_t{17}});
  EXPECT_FALSE(require_value(cursor.read_next()));

  // Inspecting a message log grants no acknowledgment rights and leaves the
  // consumer's outstanding processing obligation exactly where it was.
  auto repeated = source.receive();
  ASSERT_TRUE(std::holds_alternative<Delivery>(repeated));
  EXPECT_EQ(std::get<Delivery>(repeated)._token, originalToken);
  require_value(source.acknowledge(originalToken));
  require_value(cursor.rewind());
  EXPECT_EQ(require_value(cursor.read_next())->_message._header, first->_message._header);
  auto newer = require_value(LogArchive::capture_live(created._session));
  EXPECT_EQ(newer.publication_count(), 2u);
}

TEST(LogArchive, FiltersAndIndependentCursorsKeepExactSchemaVersions)
{
  support::PublicRuntimeConfiguration configuration;
  configuration._outputs = 2;
  support::PublicRuntimeFixture fixture(configuration);
  auto& created = fixture.created();
  auto first = require_value(created._creator.open_writer(fixture.queues()[0]));
  auto second = require_value(created._creator.open_writer(fixture.queues()[1]));
  auto small = fixture.payload(1);
  auto large = fixture.payload(2, 1);
  require_value(first.send(require_value(created._creator.next_publication_id()), fixture.message(small)));
  const auto selectedId = require_value(created._creator.next_publication_id());
  require_value(second.send(selectedId, fixture.message(large, 1)));
  auto archive = require_value(LogArchive::capture_live(created._session));
  LogFilter filter;
  filter._schema = fixture.schemas()[1];
  filter._queue = fixture.queues()[1];
  auto filtered = require_value(archive.publications(filter));
  auto all = require_value(archive.publications());
  auto selected = require_value(filtered.read_next());
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->_message._header._publication, selectedId);
  EXPECT_EQ(require_value(selected->_message.decode(fixture.codecs()[1])),
            (std::vector<PayloadValue>{std::int64_t{2}, std::int64_t{3}}));
  EXPECT_FALSE(require_value(filtered.read_next()));
  EXPECT_EQ(require_value(all.read_next())->_message._header._schema, fixture.schemas()[0]);
  LogFilter absent;
  absent._producer = TaskId{65535};
  expect_error(archive.publications(absent), ErrorCode::NotFound);
  absent = {};
  absent._queue = QueueId{65535};
  expect_error(archive.publications(absent), ErrorCode::NotFound);
}

TEST(LogArchive, CaptureDoesNotAdmitAnUnsealedSession)
{
  support::PublicRuntimeFixture fixture;
  auto registry = fixture.created()._creator.registry();
  const auto before = require_value(registry.snapshot())._revision;
  expect_error(LogArchive::capture_live(fixture.created()._session), ErrorCode::InvalidState);
  EXPECT_EQ(require_value(registry.snapshot())._revision, before);
  MessageDescriptor another;
  another._name = "AnotherMessage";
  another._schema = {MessageType{2}, 1};
  another._fields = {FieldId{0xee000001}};
  EXPECT_TRUE(std::holds_alternative<SchemaId>(registry.register_message(another)));
}

TEST(LogArchive, ASessionHandleWithoutATaskCanCaptureWhileItsProducerIsLive)
{
  support::PublicRuntimeFixture fixture;
  auto& created = fixture.created();
  auto writer = require_value(created._creator.open_writer(fixture.queues().front()));
  auto bytes = fixture.payload(17);
  auto id = require_value(created._creator.next_publication_id());
  require_value(writer.send(id, fixture.message(bytes)));
  auto attached = require_value(Session::attach(fixture.directory()));
  auto archive = require_value(LogArchive::capture_live(attached));
  EXPECT_EQ(archive.session(), created._session.identity());
  EXPECT_EQ(archive.publication_count(), 1u);
  // The same live producer still owns its claim; capture acquired no producer
  // ownership and made no change to its incarnation or publication issuer.
  auto following = require_value(created._creator.next_publication_id());
  EXPECT_EQ(following._producer, id._producer);
  EXPECT_EQ(following._sequence, id._sequence + 1);
}

TEST(LogArchive, OwningRecordsRetainTheirHeaderPayloadAndDecoderAfterAllHandlesClose)
{
  std::optional<LogRecord> saved;
  std::optional<BinaryPayloadCodec> codec;
  SessionId identity;
  {
    support::PublicRuntimeFixture fixture;
    auto& created = fixture.created();
    identity = created._session.identity();
    codec = fixture.codecs().front();
    auto writer = require_value(created._creator.open_writer(fixture.queues().front()));
    auto bytes = fixture.payload(29);
    require_value(writer.send(require_value(created._creator.next_publication_id()), fixture.message(bytes)));
    auto archive = require_value(LogArchive::capture_live(created._session));
    auto cursor = require_value(archive.publications());
    saved = require_value(cursor.read_next());
  }
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->_message.schema_session(), identity);
  EXPECT_EQ(saved->_message.view()._header, saved->_message._header);
  EXPECT_EQ(require_value(saved->_message.decode(*codec)), std::vector<PayloadValue>{std::int64_t{29}});
}

TEST(LogArchive, ExportUsesTheExistingFlatCaptureEncodingAndInspectionShowsAcceptance)
{
  support::PublicRuntimeFixture fixture;
  auto& created = fixture.created();
  auto writer = require_value(created._creator.open_writer(fixture.queues().front()));
  auto bytes = fixture.payload(37);
  const auto id = require_value(created._creator.next_publication_id());
  require_value(writer.send(id, fixture.message(bytes)));
  auto archive = require_value(LogArchive::capture_live(created._session));
  auto cursor = require_value(archive.publications());
  auto original = require_value(cursor.read_next());
  ASSERT_TRUE(original);
  auto catalog = require_value(archive.catalog());
  EXPECT_EQ(catalog._messages.size(), 2u);
  std::stringstream stream;
  require_value(archive.export_capture(stream));
  auto reader = require_value(MessageCaptureReader::create(stream, 1048576, 1073741824));
  auto exported = require_value(reader.read_next());
  ASSERT_TRUE(exported);
  EXPECT_EQ(exported->_header, original->_message._header);
  EXPECT_EQ(exported->_payload, original->_message._payload);
  EXPECT_FALSE(require_value(reader.read_next()));
  std::ostringstream output;
  require_value(archive.inspect(output));
  EXPECT_NE(output.str().find("publications=1"), std::string::npos);
  EXPECT_NE(output.str().find("schema=1.1"), std::string::npos);
  EXPECT_NE(output.str().find("sequence=1"), std::string::npos);
  EXPECT_NE(output.str().find("\"Value\" int64 = 37"), std::string::npos);
  std::ostringstream broken;
  broken.setstate(std::ios::badbit);
  expect_error(archive.export_capture(broken), ErrorCode::IoError);
  expect_error(archive.inspect(broken), ErrorCode::IoError);
}

TEST(LogArchive, InspectionBudgetsSummaryAndWholeDecodedRecordsBeforeWriting)
{
  support::PublicRuntimeFixture fixture;
  auto& created = fixture.created();
  auto writer = require_value(created._creator.open_writer(fixture.queues().front()));
  auto bytes = fixture.payload(41);
  require_value(writer.send(require_value(created._creator.next_publication_id()), fixture.message(bytes)));
  auto archive = require_value(LogArchive::capture_live(created._session));
  std::ostringstream complete;
  require_value(archive.inspect(complete));
  const auto rendered = complete.str();
  const auto summaryEnd = rendered.find('\n') + 1;
  ASSERT_GT(summaryEnd, 1u);
  ASSERT_LT(summaryEnd, rendered.size());
  EXPECT_NE(rendered.find("\"Value\" int64 = 41"), std::string::npos);

  // Too little room for the summary writes nothing. A limit that admits the
  // summary but falls inside the decoded record must leave that record whole.
  for (const auto limit : {summaryEnd - 1, summaryEnd, rendered.size() - 1})
  {
    ArchiveLimits limits;
    limits._maxTextBytes = limit;
    auto bounded = require_value(LogArchive::capture_live(created._session, limits));
    std::ostringstream output;
    expect_error(bounded.inspect(output), ErrorCode::CapacityExceeded);
    EXPECT_EQ(output.str(), limit < summaryEnd ? std::string{} : rendered.substr(0, summaryEnd));
  }
  ArchiveLimits exact;
  exact._maxTextBytes = rendered.size();
  auto fitting = require_value(LogArchive::capture_live(created._session, exact));
  std::ostringstream output;
  require_value(fitting.inspect(output));
  EXPECT_EQ(output.str(), rendered);
}

TEST(LogArchive, ReservationAndRecordCopyLimitsRejectBeforeMaterialization)
{
  support::PublicRuntimeFixture fixture;
  auto& created = fixture.created();
  auto registry = created._creator.registry();
  MessageDescriptor large;
  large._name = "Large";
  large._schema = {MessageType{2}, 1};
  large._fields.assign(8, FieldId{0xee000001});
  const auto schema = require_value(registry.register_message(large));
  auto codec = require_value(BinaryPayloadCodec::from_registry(registry, schema, 1024));
  const std::vector<PayloadValue> values(8, std::int64_t{7});
  auto bytes = require_value(codec.encode(values));
  MessageHeader header;
  header._schema = schema;
  header._payloadBytes = bytes.size();
  auto writer = require_value(created._creator.open_writer(fixture.queues().front()));
  require_value(writer.send(require_value(created._creator.next_publication_id()), {header, bytes}));
  ArchiveLimits limits;
  limits._maxRecords = 256; // The producing Task reserves 256, its peer reserves 1.
  expect_error(LogArchive::capture_live(created._session, limits), ErrorCode::CapacityExceeded);
  limits = {};
  limits._maxArchiveBytes = 1048576; // The peer's ID-only file also counts.
  expect_error(LogArchive::capture_live(created._session, limits), ErrorCode::CapacityExceeded);
  limits = {};
  limits._maxRecordBytes = 192;
  auto archive = require_value(LogArchive::capture_live(created._session, limits));
  auto cursor = require_value(archive.publications());
  expect_error(cursor.read_next(), ErrorCode::CapacityExceeded);
  expect_error(cursor.rewind(), ErrorCode::InvalidState);
}

TEST(LogArchive, MultipleProducerCutsHaveDeterministicTaskOrderNotAnInventedGlobalOrder)
{
  support::PublicRuntimeFixture fixture;
  auto& created = fixture.created();
  auto registry = created._creator.registry();
  const auto laterTask = require_value(registry.register_task(TaskDescriptor{"OtherProducer"}));
  SourceQueueSetDescriptor source;
  source._name = "OtherInput";
  source._owner = fixture.consumer_ids().front();
  const auto sourceId = require_value(registry.register_source_set(source));
  QueueDescriptor queue;
  queue._name = "OtherQueue";
  queue._producer = laterTask;
  queue._sourceSet = sourceId;
  const auto queueId = require_value(registry.register_queue(queue));
  require_value(registry.activate_source_set(sourceId));
  require_value(registry.activate_queue(queueId));
  auto other = require_value(created._session.attach_task(laterTask));
  auto otherWriter = require_value(other.open_writer(queueId));
  auto writer = require_value(created._creator.open_writer(fixture.queues().front()));
  auto bytes = fixture.payload(17);
  const auto committedFirst = require_value(other.next_publication_id());
  require_value(otherWriter.send(committedFirst, fixture.message(bytes)));
  const auto committedSecond = require_value(created._creator.next_publication_id());
  require_value(writer.send(committedSecond, fixture.message(bytes)));
  auto archive = require_value(LogArchive::capture_live(created._session));
  auto cursor = require_value(archive.publications());
  EXPECT_EQ(require_value(cursor.read_next())->_message._header._publication, committedSecond);
  EXPECT_EQ(require_value(cursor.read_next())->_message._header._publication, committedFirst);
  EXPECT_FALSE(require_value(cursor.read_next()));
}
