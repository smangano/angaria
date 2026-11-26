// Setup creates the declared topology once. Feed and status run in independent
// processes and attach through that same TOML file. Every fallible Angaria call
// is checked here rather than hidden in a generic result/configuration helper.
#include "roles.h"

#include "angaria/runtime.h"
#include "angaria/payload.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace native_pipeline
{
  using namespace angaria;

  OperationResult initialize(const std::filesystem::path& configuration)
  {
    auto creation = Session::create_from_file(configuration, "Setup");
    if (const auto* error = std::get_if<Error>(&creation))
    {
      return *error;
    }
    auto created = std::get<CreatedSession>(std::move(creation));
    auto registry = created._session.registry();
    auto snapshot_result = registry.snapshot();
    if (const auto* error = std::get_if<Error>(&snapshot_result))
    {
      return *error;
    }
    const auto snapshot = std::get<RegistrySnapshot>(std::move(snapshot_result));
    if (snapshot._tasks.size() != 4 || snapshot._queues.size() != 2 ||
        snapshot._sources.size() != 2 || !snapshot._publishing.empty() ||
        snapshot._fields.size() != 7 || snapshot._messages.size() != 3)
    {
      return Error{ErrorCode::InvalidConfiguration,
          "This application needs four Tasks, two Queues/Sources, seven Fields, and three schemas",
          "pipeline.catalog"};
    }

    // These are the application's names and schema contracts, not alternative
    // configuration defaults. The TOML file remains the visible setup source.
    const std::array<std::string_view, 4> task_names{"Setup", "Feed", "Strategy", "Audit"};
    std::array<TaskId, 4> tasks{};
    for (std::size_t index = 0; index != task_names.size(); ++index)
    {
      const auto found = std::find_if(snapshot._tasks.begin(), snapshot._tasks.end(), [&](const auto& task)
      {
        return task._descriptor._name == task_names[index];
      });
      if (found == snapshot._tasks.end() || found->_descriptor._kind != TaskKind::Process)
      {
        return Error{ErrorCode::InvalidConfiguration, "The pipeline needs named Process Tasks", "pipeline.tasks"};
      }
      tasks[index] = found->_id;
    }
    const std::array<std::string_view, 2> source_names{"StrategyInputs", "AuditInputs"};
    const std::array<std::string_view, 2> queue_names{"Quotes", "Alerts"};
    for (std::size_t index = 0; index != source_names.size(); ++index)
    {
      const auto source = std::find_if(snapshot._sources.begin(), snapshot._sources.end(), [&](const auto& item)
      {
        return item._descriptor._name == source_names[index];
      });
      const auto queue = std::find_if(snapshot._queues.begin(), snapshot._queues.end(), [&](const auto& item)
      {
        return item._descriptor._name == queue_names[index];
      });
      if (source == snapshot._sources.end() || queue == snapshot._queues.end() ||
          source->_state != EntityState::Active || queue->_state != EntityState::Active ||
          source->_descriptor._owner != tasks[index + 2] || queue->_descriptor._producer != tasks[index + 1] ||
          queue->_descriptor._sourceSet != source->_id || source->_members != std::vector{queue->_id} ||
          source->_descriptor._selection != SelectionPolicy::Strict ||
          source->_descriptor._wait != WaitPolicy::Spin || queue->_descriptor._ringCapacity != 2 ||
          queue->_descriptor._delivery != DeliveryPolicy::RecoverFromLog)
      {
        return Error{ErrorCode::InvalidConfiguration,
            "Each pipeline Source needs its one Active recovery Queue with ring capacity two",
            "pipeline.connections"};
      }
    }

    const std::array<std::string_view, 7> field_names{"PriceTicks", "Quantity", "SourceTask",
        "SourceIncarnation", "SourceSequence", "SourceQueueSequence", "AboveLimit"};
    for (std::size_t index = 0; index != field_names.size(); ++index)
    {
      const auto id = FieldId{static_cast<std::uint32_t>(0xee000001 + index)};
      const auto found = std::find_if(snapshot._fields.begin(), snapshot._fields.end(), [&](const auto& field)
      {
        return field._descriptor._id == id;
      });
      const auto base = index == 2 ? EFieldBase::BASE_UINT16 :
          index == 6 ? EFieldBase::BASE_BYTE : EFieldBase::BASE_UINT64;
      const auto bytes = index == 2 ? 2u : index == 6 ? 1u : 8u;
      const auto logical = index == 6 ? FieldLogicalKind::Boolean : FieldLogicalKind::None;
      if (found == snapshot._fields.end() || found->_descriptor._name != field_names[index] ||
          found->_descriptor._base != base || found->_descriptor._size != bytes ||
          found->_descriptor._logical._kind != logical)
      {
        return Error{ErrorCode::InvalidConfiguration, "A pipeline Field has an unexpected definition", "pipeline.fields"};
      }
    }
    const auto price = FieldId{0xee000001};
    const auto quantity = FieldId{0xee000002};
    for (const auto& message : snapshot._messages)
    {
      const auto& descriptor = message._descriptor;
      std::vector<FieldId> expected;
      if (descriptor._name == "Quote" && descriptor._schema == SchemaId{MessageType{1}, 1})
      {
        expected = {price, quantity};
      }
      else if (descriptor._name == "Quote" && descriptor._schema == SchemaId{MessageType{1}, 2})
      {
        expected = {quantity, price};
      }
      else if (descriptor._name == "PriceAlert" && descriptor._schema == SchemaId{MessageType{2}, 1})
      {
        expected = {FieldId{0xee000003}, FieldId{0xee000004}, FieldId{0xee000005},
            FieldId{0xee000006}, price, quantity, FieldId{0xee000007}};
      }
      if (expected.empty() || descriptor._fields != expected)
      {
        return Error{ErrorCode::InvalidConfiguration, "A pipeline message has an unexpected exact schema", "pipeline.messages"};
      }
    }

    const auto identity = created._session.identity();
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::hex << std::setfill('0') << std::setw(16) << identity._high << std::setw(16) << identity._low;
    std::cout << "SESSION " << text.str() << '\n'
              << "INITIALIZED tasks=" << snapshot._tasks.size() << " queues=" << snapshot._queues.size()
              << " sources=" << snapshot._sources.size() << " schemas=" << snapshot._messages.size()
              << " ring=2\n";
    return std::monostate{};
  }

  OperationResult publish_quotes(const std::filesystem::path& configuration,
      std::uint32_t first, std::uint32_t count, bool hold)
  {
    if (first == 0 || count == 0 || first > max_quote_number || count > max_quote_number - first + 1)
    {
      return Error{ErrorCode::InvalidArgument, "Quote range must fit 1..1000", "pipeline.feed.range"};
    }
    auto attachment = Session::attach_from_file(configuration);
    if (const auto* error = std::get_if<Error>(&attachment))
    {
      return *error;
    }
    auto session = std::get<Session>(std::move(attachment));
    auto task_result = session.attach_task("Feed");
    if (const auto* error = std::get_if<Error>(&task_result))
    {
      return *error;
    }
    auto feed = std::get<Task>(std::move(task_result));
    auto writer_result = feed.open_writer("Quotes");
    if (const auto* error = std::get_if<Error>(&writer_result))
    {
      return *error;
    }
    auto writer = std::get<QueueWriter>(std::move(writer_result));
    auto registry = session.registry();
    auto schema_v1_result = registry.find_message("Quote", 1);
    if (const auto* error = std::get_if<Error>(&schema_v1_result))
    {
      return *error;
    }
    const auto schema_v1 = std::get<SchemaId>(schema_v1_result);
    auto schema_v2_result = registry.find_message("Quote", 2);
    if (const auto* error = std::get_if<Error>(&schema_v2_result))
    {
      return *error;
    }
    const auto schema_v2 = std::get<SchemaId>(schema_v2_result);
    auto codec_v1_result = BinaryPayloadCodec::from_registry(registry, schema_v1, max_payload_bytes);
    if (const auto* error = std::get_if<Error>(&codec_v1_result))
    {
      return *error;
    }
    const auto codec_v1 = std::get<BinaryPayloadCodec>(std::move(codec_v1_result));
    auto codec_v2_result = BinaryPayloadCodec::from_registry(registry, schema_v2, max_payload_bytes);
    if (const auto* error = std::get_if<Error>(&codec_v2_result))
    {
      return *error;
    }
    const auto codec_v2 = std::get<BinaryPayloadCodec>(std::move(codec_v2_result));
    if (schema_v1 != SchemaId{MessageType{1}, 1} || schema_v2 != SchemaId{MessageType{1}, 2} ||
        codec_v1.payload_size() != 16 || codec_v2.payload_size() != 16 ||
        codec_v1.fields().size() != 2 || codec_v2.fields().size() != 2 ||
        codec_v1.fields()[0]._id != FieldId{0xee000001} || codec_v1.fields()[1]._id != FieldId{0xee000002} ||
        codec_v2.fields()[0]._id != FieldId{0xee000002} || codec_v2.fields()[1]._id != FieldId{0xee000001})
    {
      return Error{ErrorCode::InvalidConfiguration, "Feed requires the two exact Quote layouts", "pipeline.feed.schemas"};
    }

    for (std::uint32_t offset = 0; offset != count; ++offset)
    {
      const auto number = first + offset;
      const auto price = std::uint64_t{10000} + number;
      const auto quantity = std::uint64_t{10} * number;
      const auto version = number % 2 == 1 ? 1u : 2u;
      const auto& codec = version == 1 ? codec_v1 : codec_v2;
      auto encoded = version == 1 ? codec.encode({price, quantity}) : codec.encode({quantity, price});
      if (const auto* error = std::get_if<Error>(&encoded))
      {
        return *error;
      }
      const auto bytes = std::get<std::vector<std::byte>>(std::move(encoded));
      auto identity_result = feed.next_publication_id();
      if (const auto* error = std::get_if<Error>(&identity_result))
      {
        return *error;
      }
      const auto publication = std::get<PublicationId>(identity_result);
      MessageHeader header;
      header._schema = codec.schema();
      header._payloadBytes = bytes.size();
      auto sent = writer.send(publication, MessageView{header, bytes});
      if (const auto* error = std::get_if<Error>(&sent))
      {
        if (error->_code != ErrorCode::OutcomeUnknown)
        {
          return *error;
        }
        // An uncertain result keeps its original identity. A new sequence
        // would mean a second publication rather than resolving this request.
        auto recovered = feed.publication_status(publication);
        if (const auto* recovery_error = std::get_if<Error>(&recovered))
        {
          std::cerr << "Keep publication task=" << static_cast<unsigned>(publication._producer._task)
                    << " incarnation=" << publication._producer._incarnation
                    << " sequence=" << publication._sequence << " for later lookup.\n";
          return *recovery_error;
        }
        sent = std::get<PublicationOutcome>(std::move(recovered));
      }
      const auto outcome = std::get<PublicationOutcome>(std::move(sent));
      if (outcome._status != AcceptanceStatus::Accepted)
      {
        return Error{ErrorCode::InvalidState, "Quote was not accepted; no replacement ID was sent", "pipeline.feed.send"};
      }
      std::cout << "FEED quote=" << number << " version=" << version << " price=" << price
                << " quantity=" << quantity << " task=" << static_cast<unsigned>(publication._producer._task)
                << " incarnation=" << publication._producer._incarnation
                << " publication=" << publication._sequence << '\n';
    }
    if (hold)
    {
      // The walkthrough kills Feed only after every reported send completed.
      // Enter/EOF merely closes this finished producer; it sends no extra data.
      std::cout << "READY feed\n" << std::flush;
      std::string line;
      std::getline(std::cin, line);
    }
    return std::monostate{};
  }

  OperationResult publication_status(const std::filesystem::path& configuration,
      std::string_view task_name, std::uint64_t incarnation, std::uint64_t sequence)
  {
    if (task_name.empty() || incarnation == 0 || sequence == 0)
    {
      return Error{ErrorCode::InvalidArgument, "Status needs a Task and positive incarnation/sequence", "pipeline.status"};
    }
    auto attachment = Session::attach_from_file(configuration);
    if (const auto* error = std::get_if<Error>(&attachment))
    {
      return *error;
    }
    auto session = std::get<Session>(std::move(attachment));
    // Status is observational after recovery, but acquiring this sender's
    // Task claim is a real attachment and advances its current incarnation.
    auto task_result = session.attach_task(task_name);
    if (const auto* error = std::get_if<Error>(&task_result))
    {
      return *error;
    }
    auto task = std::get<Task>(std::move(task_result));
    const PublicationId publication{{session.identity(), task.claim()._task, incarnation}, sequence};
    auto lookup = task.publication_status(publication);
    if (const auto* error = std::get_if<Error>(&lookup))
    {
      return *error;
    }
    const auto outcome = std::get<PublicationOutcome>(std::move(lookup));
    if (outcome._status == AcceptanceStatus::Partial)
    {
      return Error{ErrorCode::InvalidState, "Direct pipeline sending cannot have partial acceptance", "pipeline.status"};
    }
    std::cout << "STATUS task=" << task_name << " incarnation=" << incarnation << " publication=" << sequence
              << " accepted=" << (outcome._status == AcceptanceStatus::Accepted ? "yes" : "no")
              << " destinations=" << outcome._destinations.size() << '\n';
    return std::monostate{};
  }
}
