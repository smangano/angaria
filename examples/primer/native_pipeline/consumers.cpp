// Strategy and Audit are independent applications using the public mapped
// transport. The only shared application configuration is the explicit TOML.
#include "roles.h"

#include "angaria/payload.h"
#include "angaria/runtime.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace native_pipeline
{
  using namespace angaria;

  OperationResult process_quotes(const std::filesystem::path& configuration,
      std::uint32_t count, bool hold_first)
  {
    if (count == 0 || count > max_quote_number)
    {
      return Error{ErrorCode::InvalidArgument, "Quote count must be between 1 and 1000", "pipeline.strategy.count"};
    }
    auto attachment = Session::attach_from_file(configuration);
    if (const auto* error = std::get_if<Error>(&attachment))
    {
      return *error;
    }
    auto session = std::get<Session>(std::move(attachment));
    auto task_result = session.attach_task("Strategy");
    if (const auto* error = std::get_if<Error>(&task_result))
    {
      return *error;
    }
    auto strategy = std::get<Task>(std::move(task_result));
    const auto claim = strategy.claim();
    auto registry = session.registry();
    auto feed_result = registry.find_task("Feed");
    if (const auto* error = std::get_if<Error>(&feed_result))
    {
      return *error;
    }
    const auto feed = std::get<TaskId>(feed_result);
    auto queue_result = registry.find_queue("Quotes");
    if (const auto* error = std::get_if<Error>(&queue_result))
    {
      return *error;
    }
    const auto quote_queue = std::get<QueueId>(queue_result);
    auto v1_result = registry.find_message("Quote", 1);
    if (const auto* error = std::get_if<Error>(&v1_result))
    {
      return *error;
    }
    const auto quote_v1 = std::get<SchemaId>(v1_result);
    auto v2_result = registry.find_message("Quote", 2);
    if (const auto* error = std::get_if<Error>(&v2_result))
    {
      return *error;
    }
    const auto quote_v2 = std::get<SchemaId>(v2_result);
    auto v1_codec_result = BinaryPayloadCodec::from_registry(registry, quote_v1, max_payload_bytes);
    if (const auto* error = std::get_if<Error>(&v1_codec_result))
    {
      return *error;
    }
    const auto v1_codec = std::get<BinaryPayloadCodec>(std::move(v1_codec_result));
    auto v2_codec_result = BinaryPayloadCodec::from_registry(registry, quote_v2, max_payload_bytes);
    if (const auto* error = std::get_if<Error>(&v2_codec_result))
    {
      return *error;
    }
    const auto v2_codec = std::get<BinaryPayloadCodec>(std::move(v2_codec_result));
    const auto v1_fields = v1_codec.fields();
    const auto v2_fields = v2_codec.fields();
    if (quote_v1 != SchemaId{MessageType{1}, 1} || quote_v2 != SchemaId{MessageType{1}, 2} ||
        v1_fields.size() != 2 || v2_fields.size() != 2 ||
        v1_fields[0]._id != FieldId{0xee000001} || v1_fields[1]._id != FieldId{0xee000002} ||
        v2_fields[0]._id != FieldId{0xee000002} || v2_fields[1]._id != FieldId{0xee000001} ||
        v1_codec.payload_size() != 16 || v2_codec.payload_size() != 16)
    {
      return Error{ErrorCode::SchemaConflict, "Expected the two documented Quote layouts", "pipeline.quote.schema"};
    }
    auto alert_result = registry.find_message("PriceAlert", 1);
    if (const auto* error = std::get_if<Error>(&alert_result))
    {
      return *error;
    }
    const auto alert_schema = std::get<SchemaId>(alert_result);
    auto alert_codec_result = BinaryPayloadCodec::from_registry(registry, alert_schema, max_payload_bytes);
    if (const auto* error = std::get_if<Error>(&alert_codec_result))
    {
      return *error;
    }
    const auto alert_codec = std::get<BinaryPayloadCodec>(std::move(alert_codec_result));
    const std::array expected_alert_fields{FieldId{0xee000003}, FieldId{0xee000004},
        FieldId{0xee000005}, FieldId{0xee000006}, FieldId{0xee000001},
        FieldId{0xee000002}, FieldId{0xee000007}};
    const auto alert_fields = alert_codec.fields();
    if (alert_schema != SchemaId{MessageType{2}, 1} ||
        alert_fields.size() != expected_alert_fields.size() || alert_codec.payload_size() != 43)
    {
      return Error{ErrorCode::SchemaConflict, "Expected the documented PriceAlert layout", "pipeline.alert.schema"};
    }
    for (std::size_t index = 0; index != expected_alert_fields.size(); ++index)
    {
      if (alert_fields[index]._id != expected_alert_fields[index])
      {
        return Error{ErrorCode::SchemaConflict, "PriceAlert Field order differs from the application contract", "pipeline.alert.fields"};
      }
    }
    auto opening = strategy.open_source_set("StrategyInputs");
    if (const auto* error = std::get_if<Error>(&opening))
    {
      return *error;
    }
    auto inputs = std::get<SourceQueueSet>(std::move(opening));
    auto writer_result = strategy.open_writer("Alerts");
    if (const auto* error = std::get_if<Error>(&writer_result))
    {
      return *error;
    }
    auto alerts = std::get<QueueWriter>(std::move(writer_result));
    std::cout << "READY strategy task=" << static_cast<unsigned>(claim._task)
              << " incarnation=" << claim._incarnation << std::endl;

    for (std::uint32_t processed = 0; processed != count; ++processed)
    {
      // A finite wait handles a live Feed without depending on arbitrary sleeps.
      // Its reserved sixteen-message Queue also holds the offline walkthrough backlog.
      auto event = inputs.receive(std::chrono::seconds{5});
      if (const auto* error = std::get_if<Error>(&event))
      {
        return *error;
      }
      if (std::holds_alternative<ReceiveTimeout>(event))
      {
        return Error{ErrorCode::InvalidState, "Timed out waiting for the requested Quotes", "pipeline.strategy.receive"};
      }
      if (std::holds_alternative<DeliveryGap>(event))
      {
        return Error{ErrorCode::InvalidState, "This market monitor requires recovery delivery", "pipeline.strategy.gap"};
      }
      auto current = std::get<Delivery>(std::move(event));
      const auto header = current._message._header;
      if (!header._queue || *header._queue != quote_queue || !header._queueSequence ||
          *header._queueSequence == 0 || *header._queueSequence != current._token._sequence ||
          !header._consumer || *header._consumer != claim || current._token._consumer != claim ||
          current._token._sourceSet != inputs.id() || current._token._queue != quote_queue ||
          !header._enqueuedAt || !header._dequeuedAt || header._format != PayloadFormat::Binary ||
          header._publication._producer._session != session.identity() ||
          header._publication._producer._task != feed || header._publication._producer._incarnation == 0 ||
          header._publication._sequence == 0 || header._payloadBytes != current._message._payload.size())
      {
        return Error{ErrorCode::InvalidState, "Quote is missing the expected standard delivery header", "pipeline.quote.header"};
      }
      if (header._schema != quote_v1 && header._schema != quote_v2)
      {
        return Error{ErrorCode::SchemaConflict, "Strategy does not understand this exact Quote schema", "pipeline.quote.schema"};
      }
      auto copy_result = current._message.copy_message(max_payload_bytes);
      if (const auto* error = std::get_if<Error>(&copy_result))
      {
        return *error;
      }
      const auto saved = std::get<Message>(std::move(copy_result));
      if (saved._header != header)
      {
        return Error{ErrorCode::InvalidState, "Copying changed the received MessageHeader", "pipeline.quote.copy"};
      }
      // Both versions occupy sixteen bytes. Only the recorded exact schema,
      // not payload length or the latest registered version, selects the order.
      const auto& codec = header._schema == quote_v1 ? v1_codec : v2_codec;
      auto decoded_result = saved.decode(codec);
      if (const auto* error = std::get_if<Error>(&decoded_result))
      {
        return *error;
      }
      const auto values = std::get<std::vector<PayloadValue>>(std::move(decoded_result));
      const auto price_index = header._schema == quote_v1 ? 0u : 1u;
      const auto quantity_index = header._schema == quote_v1 ? 1u : 0u;
      if (values.size() != 2 || !std::holds_alternative<std::uint64_t>(values[price_index]) ||
          !std::holds_alternative<std::uint64_t>(values[quantity_index]))
      {
        return Error{ErrorCode::SchemaConflict, "Quote requires uint64 PriceTicks and Quantity", "pipeline.quote.values"};
      }
      const auto price = std::get<std::uint64_t>(values[price_index]);
      const auto quantity = std::get<std::uint64_t>(values[quantity_index]);
      const auto publication = header._publication;
      const auto queue_sequence = *header._queueSequence;
      std::cout << "BORROW queue_sequence=" << queue_sequence
                << " source_task=" << static_cast<unsigned>(publication._producer._task)
                << " source_incarnation=" << publication._producer._incarnation
                << " source_publication=" << publication._sequence
                << " version=" << header._schema._version
                << " price=" << price << " quantity=" << quantity
                << " issuance=" << current._token._issuance << '\n';
      if (processed == 0)
      {
        std::cout << "HEADER publication_ns=" << header._publishedAt.time_since_epoch().count()
                  << " enqueue_ns=" << header._enqueuedAt->time_since_epoch().count()
                  << " dequeue_ns=" << header._dequeuedAt->time_since_epoch().count()
                  << " copied=yes\n";
        if (hold_first)
        {
          // The readiness marker is before any output identity or publication.
          // A killed Strategy therefore leaves this input unfinished and no
          // application output to duplicate in this particular walkthrough.
          std::cout << "READY strategy-before-forward" << std::endl;
          std::string continuation;
          if (!std::getline(std::cin, continuation))
          {
            auto closed = inputs.close();
            if (const auto* error = std::get_if<Error>(&closed))
            {
              return *error;
            }
            return Error{ErrorCode::InvalidState, "Input closed before forwarding; the Quote remains pending", "pipeline.strategy.control"};
          }
        }
        const auto old_token = current._token;
        auto released = inputs.release(old_token);
        if (const auto* error = std::get_if<Error>(&released))
        {
          return *error;
        }
        // The old MessageView has expired. saved owns its bytes and complete
        // original header, so it is still safe to inspect while receiving again.
        auto redelivery = inputs.receive(std::chrono::seconds{5});
        if (const auto* error = std::get_if<Error>(&redelivery))
        {
          return *error;
        }
        if (std::holds_alternative<ReceiveTimeout>(redelivery) || std::holds_alternative<DeliveryGap>(redelivery))
        {
          return Error{ErrorCode::InvalidState, "Released recovery delivery was not offered again", "pipeline.strategy.redelivery"};
        }
        current = std::get<Delivery>(std::move(redelivery));
        auto expected_header = saved._header;
        expected_header._dequeuedAt = current._message._header._dequeuedAt;
        if (current._token == old_token || current._token._sequence != old_token._sequence ||
            current._message._header != expected_header ||
            std::vector<std::byte>(current._message._payload.begin(), current._message._payload.end()) != saved._payload)
        {
          return Error{ErrorCode::InvalidState, "Redelivery did not preserve the original Quote with a new token", "pipeline.strategy.redelivery"};
        }
        const auto new_token = current._token;
        std::cout << "REDELIVER queue_sequence=" << queue_sequence
                  << " old_issuance=" << old_token._issuance
                  << " new_issuance=" << new_token._issuance << '\n';
        auto stale = inputs.acknowledge(old_token);
        const auto* stale_error = std::get_if<Error>(&stale);
        if (!stale_error || stale_error->_code != ErrorCode::StaleDelivery)
        {
          if (stale_error)
          {
            return *stale_error;
          }
          return Error{ErrorCode::InvalidState, "An old token unexpectedly acknowledged redelivery", "pipeline.strategy.stale"};
        }
        auto repeated = inputs.receive(std::chrono::seconds{5});
        if (const auto* error = std::get_if<Error>(&repeated))
        {
          return *error;
        }
        if (!std::holds_alternative<Delivery>(repeated))
        {
          return Error{ErrorCode::InvalidState, "Rejecting an old token changed the current borrow", "pipeline.strategy.stale"};
        }
        current = std::get<Delivery>(std::move(repeated));
        if (current._token != new_token)
        {
          return Error{ErrorCode::InvalidState, "Rejecting an old token replaced the current issuance", "pipeline.strategy.stale"};
        }
        std::cout << "STALE token rejected\n";
      }

      const auto above_limit = price >= alert_threshold;
      auto encoded_result = alert_codec.encode({static_cast<std::uint16_t>(publication._producer._task),
          publication._producer._incarnation, publication._sequence, queue_sequence,
          price, quantity, above_limit});
      if (const auto* error = std::get_if<Error>(&encoded_result))
      {
        return *error;
      }
      const auto bytes = std::get<std::vector<std::byte>>(std::move(encoded_result));
      auto identity_result = strategy.next_publication_id();
      if (const auto* error = std::get_if<Error>(&identity_result))
      {
        return *error;
      }
      const auto output = std::get<PublicationId>(identity_result);
      MessageHeader output_header;
      output_header._schema = alert_schema;
      output_header._payloadBytes = bytes.size();
      auto sent = alerts.send(output, MessageView{output_header, bytes});
      if (const auto* error = std::get_if<Error>(&sent))
      {
        if (error->_code != ErrorCode::OutcomeUnknown)
        {
          return *error;
        }
        // Resolve this same output identity. An uncertain send must not become
        // a second publication under a new identity, or an acknowledged input.
        auto status = strategy.publication_status(output);
        if (const auto* recovery_error = std::get_if<Error>(&status))
        {
          std::cerr << "Keep Strategy publication Task=" << static_cast<unsigned>(output._producer._task)
                    << " incarnation=" << output._producer._incarnation
                    << " sequence=" << output._sequence << " for later lookup.\n";
          return *recovery_error;
        }
        sent = std::get<PublicationOutcome>(std::move(status));
      }
      const auto outcome = std::get<PublicationOutcome>(std::move(sent));
      if (outcome._status != AcceptanceStatus::Accepted || outcome._publication != output)
      {
        return Error{ErrorCode::InvalidState, "Alert acceptance was not established; input is not acknowledged", "pipeline.strategy.send"};
      }
      // Sending the alert and acknowledging its input are separate commits.
      // A crash between them can repeat application work after replacement.
      auto acknowledged = inputs.acknowledge(current._token);
      if (const auto* error = std::get_if<Error>(&acknowledged))
      {
        return *error;
      }
      std::cout << "FORWARD source_queue_sequence=" << queue_sequence
                << " source_incarnation=" << publication._producer._incarnation
                << " source_publication=" << publication._sequence
                << " version=" << header._schema._version
                << " price=" << price << " quantity=" << quantity
                << " above_limit=" << (above_limit ? "true" : "false")
                << " task=" << static_cast<unsigned>(output._producer._task)
                << " incarnation=" << output._producer._incarnation
                << " publication=" << output._sequence << '\n';
    }
    auto closed = inputs.close();
    if (const auto* error = std::get_if<Error>(&closed))
    {
      return *error;
    }
    std::cout << "STRATEGY completed=" << count << '\n';
    return std::monostate{};
  }

  OperationResult audit_alerts(const std::filesystem::path& configuration,
      std::uint32_t count)
  {
    if (count == 0 || count > max_quote_number)
    {
      return Error{ErrorCode::InvalidArgument, "Alert count must be between 1 and 1000", "pipeline.audit.count"};
    }
    auto attachment = Session::attach_from_file(configuration);
    if (const auto* error = std::get_if<Error>(&attachment))
    {
      return *error;
    }
    auto session = std::get<Session>(std::move(attachment));
    auto task_result = session.attach_task("Audit");
    if (const auto* error = std::get_if<Error>(&task_result))
    {
      return *error;
    }
    auto audit = std::get<Task>(std::move(task_result));
    const auto claim = audit.claim();
    auto registry = session.registry();
    auto feed_result = registry.find_task("Feed");
    if (const auto* error = std::get_if<Error>(&feed_result))
    {
      return *error;
    }
    const auto feed = std::get<TaskId>(feed_result);
    auto strategy_result = registry.find_task("Strategy");
    if (const auto* error = std::get_if<Error>(&strategy_result))
    {
      return *error;
    }
    const auto strategy = std::get<TaskId>(strategy_result);
    auto queue_result = registry.find_queue("Alerts");
    if (const auto* error = std::get_if<Error>(&queue_result))
    {
      return *error;
    }
    const auto alert_queue = std::get<QueueId>(queue_result);
    auto schema_result = registry.find_message("PriceAlert", 1);
    if (const auto* error = std::get_if<Error>(&schema_result))
    {
      return *error;
    }
    const auto schema = std::get<SchemaId>(schema_result);
    auto codec_result = BinaryPayloadCodec::from_registry(registry, schema, max_payload_bytes);
    if (const auto* error = std::get_if<Error>(&codec_result))
    {
      return *error;
    }
    const auto codec = std::get<BinaryPayloadCodec>(std::move(codec_result));
    const std::array expected_fields{FieldId{0xee000003}, FieldId{0xee000004},
        FieldId{0xee000005}, FieldId{0xee000006}, FieldId{0xee000001},
        FieldId{0xee000002}, FieldId{0xee000007}};
    const auto fields = codec.fields();
    if (schema != SchemaId{MessageType{2}, 1} || fields.size() != expected_fields.size() || codec.payload_size() != 43)
    {
      return Error{ErrorCode::SchemaConflict, "Audit expects the documented PriceAlert layout", "pipeline.audit.schema"};
    }
    for (std::size_t index = 0; index != expected_fields.size(); ++index)
    {
      if (fields[index]._id != expected_fields[index])
      {
        return Error{ErrorCode::SchemaConflict, "PriceAlert Field order differs from the application contract", "pipeline.audit.fields"};
      }
    }
    auto opening = audit.open_source_set("AuditInputs");
    if (const auto* error = std::get_if<Error>(&opening))
    {
      return *error;
    }
    auto inputs = std::get<SourceQueueSet>(std::move(opening));
    std::cout << "READY audit task=" << static_cast<unsigned>(claim._task)
              << " incarnation=" << claim._incarnation << std::endl;

    for (std::uint32_t processed = 0; processed != count; ++processed)
    {
      auto event = inputs.receive(std::chrono::seconds{5});
      if (const auto* error = std::get_if<Error>(&event))
      {
        return *error;
      }
      if (std::holds_alternative<ReceiveTimeout>(event))
      {
        return Error{ErrorCode::InvalidState, "Timed out waiting for the requested PriceAlerts", "pipeline.audit.receive"};
      }
      if (std::holds_alternative<DeliveryGap>(event))
      {
        return Error{ErrorCode::InvalidState, "Audit requires recovery delivery", "pipeline.audit.gap"};
      }
      const auto current = std::get<Delivery>(std::move(event));
      const auto header = current._message._header;
      if (header._schema != schema || header._format != PayloadFormat::Binary ||
          !header._queue || *header._queue != alert_queue || !header._queueSequence ||
          *header._queueSequence == 0 || *header._queueSequence != current._token._sequence ||
          !header._consumer || *header._consumer != claim || current._token._consumer != claim ||
          current._token._sourceSet != inputs.id() || current._token._queue != alert_queue ||
          !header._enqueuedAt || !header._dequeuedAt ||
          header._publication._producer._session != session.identity() ||
          header._publication._producer._task != strategy || header._publication._producer._incarnation == 0 ||
          header._publication._sequence == 0 || header._payloadBytes != current._message._payload.size())
      {
        return Error{ErrorCode::InvalidState, "PriceAlert is missing the expected standard delivery header", "pipeline.audit.header"};
      }
      auto copy_result = current._message.copy_message(max_payload_bytes);
      if (const auto* error = std::get_if<Error>(&copy_result))
      {
        return *error;
      }
      const auto saved = std::get<Message>(std::move(copy_result));
      if (saved._header != header)
      {
        return Error{ErrorCode::InvalidState, "Copying changed the received MessageHeader", "pipeline.audit.copy"};
      }
      auto decoded_result = saved.decode(codec);
      if (const auto* error = std::get_if<Error>(&decoded_result))
      {
        return *error;
      }
      const auto values = std::get<std::vector<PayloadValue>>(std::move(decoded_result));
      if (values.size() != 7 || !std::holds_alternative<std::uint16_t>(values[0]) ||
          !std::holds_alternative<std::uint64_t>(values[1]) || !std::holds_alternative<std::uint64_t>(values[2]) ||
          !std::holds_alternative<std::uint64_t>(values[3]) || !std::holds_alternative<std::uint64_t>(values[4]) ||
          !std::holds_alternative<std::uint64_t>(values[5]) || !std::holds_alternative<bool>(values[6]))
      {
        return Error{ErrorCode::SchemaConflict, "PriceAlert Field types differ from the application contract", "pipeline.audit.values"};
      }
      const auto source_task = std::get<std::uint16_t>(values[0]);
      const auto source_incarnation = std::get<std::uint64_t>(values[1]);
      const auto source_publication = std::get<std::uint64_t>(values[2]);
      const auto source_queue_sequence = std::get<std::uint64_t>(values[3]);
      const auto price = std::get<std::uint64_t>(values[4]);
      const auto quantity = std::get<std::uint64_t>(values[5]);
      const auto above_limit = std::get<bool>(values[6]);
      if (source_task != static_cast<std::uint16_t>(feed) || source_incarnation == 0 ||
          source_publication == 0 || source_queue_sequence == 0 || above_limit != (price >= alert_threshold))
      {
        return Error{ErrorCode::InvalidState, "PriceAlert has invalid Feed provenance or threshold result", "pipeline.audit.alert"};
      }
      auto acknowledged = inputs.acknowledge(current._token);
      if (const auto* error = std::get_if<Error>(&acknowledged))
      {
        return *error;
      }
      // Feed and Strategy identities are deliberately printed separately.
      // The shared Session supplies the SessionId for both publications.
      std::cout << "AUDIT input_queue_sequence=" << *header._queueSequence
                << " source_task=" << source_task
                << " source_incarnation=" << source_incarnation
                << " source_publication=" << source_publication
                << " source_queue_sequence=" << source_queue_sequence
                << " price=" << price << " quantity=" << quantity
                << " above_limit=" << (above_limit ? "true" : "false")
                << " producer_task=" << static_cast<unsigned>(header._publication._producer._task)
                << " producer_incarnation=" << header._publication._producer._incarnation
                << " publication=" << header._publication._sequence << '\n';
    }
    auto closed = inputs.close();
    if (const auto* error = std::get_if<Error>(&closed))
    {
      return *error;
    }
    std::cout << "AUDIT completed=" << count << '\n';
    return std::monostate{};
  }
}
