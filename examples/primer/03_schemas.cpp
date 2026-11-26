// A schema describes the ordered Fields in one version of a message. This
// example registers definitions; later lessons encode and deliver payloads.

#include "angaria/runtime.h"

#include <exception>
#include <iostream>
#include <utility>
#include <variant>

angaria::OperationResult print_schema(angaria::Registry registry, angaria::SchemaId id)
{
  auto message_result = registry.describe_message(id);
  if (const auto* error = std::get_if<angaria::Error>(&message_result))
  {
    return *error;
  }
  const auto message = std::get<angaria::MessageDescriptor>(std::move(message_result));
  std::cout << message._name << " v" << id._version << " Fields:";
  for (const auto field : message._fields)
  {
    auto descriptor_result = registry.describe_field(field);
    if (const auto* error = std::get_if<angaria::Error>(&descriptor_result))
    {
      return *error;
    }
    const auto descriptor = std::get<angaria::FieldDescriptor>(std::move(descriptor_result));
    std::cout << ' ' << descriptor._name;
  }
  std::cout << '\n';
  return std::monostate{};
}

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    std::cerr << "Usage: " << argv[0] << " <new-session-directory>\n";
    return 1;
  }
  try
  {
    angaria::SessionOptions options;
    options._name = "Versioned message definitions";
    options._directory = argv[1];
    options._maxLogBytes = 1024 * 1024;
    options._limits = angaria::CapacityLimits::empty().tasks(8).queues(8)
        .source_sets(4).publishing_sets(4).memberships_per_set(4)
        .fields(16).message_schemas(8);
    options._defaults._ringCapacity = 8;
    angaria::TaskDescriptor creator;
    creator._name = "Setup";
    auto created_result = angaria::Session::create(options, creator);
    if (const auto* error = std::get_if<angaria::Error>(&created_result))
    {
      if (error->_code == angaria::ErrorCode::Conflict)
      {
        std::cerr << "Conflict: ";
      }
      std::cerr << "Session creation failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    auto created = std::get<angaria::CreatedSession>(std::move(created_result));
    auto registry = created._creator.registry();

    // Field IDs are application-chosen full 32-bit values with the 0xEE prefix.
    // Fixed scalar sizes are encoded byte widths, not sizeof a C++ object.
    angaria::FieldDescriptor price;
    price._id = static_cast<angaria::FieldId>(0xee000001);
    price._name = "PriceTicks";
    price._base = angaria::EFieldBase::BASE_INT64;
    price._size = 8;
    auto price_registration_result = registry.register_field(price);
    if (const auto* error = std::get_if<angaria::Error>(&price_registration_result))
    {
      std::cerr << "Registering PriceTicks failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    angaria::FieldDescriptor quantity;
    quantity._id = static_cast<angaria::FieldId>(0xee000002);
    quantity._name = "Quantity";
    quantity._base = angaria::EFieldBase::BASE_UINT64;
    quantity._size = 8;
    auto quantity_registration_result = registry.register_field(quantity);
    if (const auto* error = std::get_if<angaria::Error>(&quantity_registration_result))
    {
      std::cerr << "Registering Quantity failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    angaria::FieldDescriptor instrument;
    instrument._id = static_cast<angaria::FieldId>(0xee000003);
    instrument._name = "Instrument";
    instrument._base = angaria::EFieldBase::BASE_UINT32;
    instrument._size = 4;
    auto instrument_registration_result = registry.register_field(instrument);
    if (const auto* error = std::get_if<angaria::Error>(&instrument_registration_result))
    {
      std::cerr << "Registering Instrument failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }

    angaria::MessageDescriptor quote;
    quote._schema = {static_cast<angaria::MessageType>(1), 1};
    quote._name = "Quote";
    quote._fields = {price._id, quantity._id};
    quote._doc = "A market quote";
    auto version_one_result = registry.register_message(quote);
    if (const auto* error = std::get_if<angaria::Error>(&version_one_result))
    {
      std::cerr << "Registering Quote v1 failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto version_one = std::get<angaria::SchemaId>(std::move(version_one_result));
    auto extended_quote = quote;
    extended_quote._schema._version = 2;
    extended_quote._fields.push_back(instrument._id);
    auto version_two_result = registry.register_message(extended_quote);
    if (const auto* error = std::get_if<angaria::Error>(&version_two_result))
    {
      std::cerr << "Registering Quote v2 failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto version_two = std::get<angaria::SchemaId>(std::move(version_two_result));

    // Registering a new version preserves the old version and its Field order.
    auto print_one_result = print_schema(created._session.registry(), version_one);
    if (const auto* error = std::get_if<angaria::Error>(&print_one_result))
    {
      std::cerr << "Printing Quote v1 failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    auto print_two_result = print_schema(created._session.registry(), version_two);
    if (const auto* error = std::get_if<angaria::Error>(&print_two_result))
    {
      std::cerr << "Printing Quote v2 failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    auto found_one_result = registry.find_message("Quote", 1);
    if (const auto* error = std::get_if<angaria::Error>(&found_one_result))
    {
      std::cerr << "Finding Quote v1 failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto found_one = std::get<angaria::SchemaId>(std::move(found_one_result));
    auto found_two_result = registry.find_message("Quote", 2);
    if (const auto* error = std::get_if<angaria::Error>(&found_two_result))
    {
      std::cerr << "Finding Quote v2 failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto found_two = std::get<angaria::SchemaId>(std::move(found_two_result));
    // Registration metadata records which Task introduced this definition.
    auto metadata_result = registry.metadata(found_one);
    if (const auto* error = std::get_if<angaria::Error>(&metadata_result))
    {
      std::cerr << "Reading Quote metadata failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto metadata = std::get<angaria::RegistrationMetadata>(std::move(metadata_result));
    auto retry_result = registry.register_message(quote);
    if (const auto* error = std::get_if<angaria::Error>(&retry_result))
    {
      std::cerr << "Retrying Quote registration failed: " << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")" << '\n';
      return 1;
    }
    const auto retry = std::get<angaria::SchemaId>(std::move(retry_result));
    std::cout << "Exact schema retry kept version: " << retry._version << '\n'
              << "Lookup of Quote v1 kept version: " << found_one._version << '\n'
              << "Lookup of Quote v2 kept version: " << found_two._version << '\n'
              << "Quote v1 registered by Task: " << static_cast<unsigned>(metadata._createdBy) << '\n';

    auto incompatible = quote;
    incompatible._doc = "A changed existing version";
    const auto conflict = registry.register_message(incompatible);
    const auto* error = std::get_if<angaria::Error>(&conflict);
    if (!error)
    {
      std::cerr << "Expected SchemaConflict, but the changed existing schema was accepted.\n";
      return 1;
    }
    if (error->_code != angaria::ErrorCode::SchemaConflict)
    {
      std::cerr << "Changed schema registration failed with an unexpected error: "
                << error->_message << " at " << error->_path
                << " (code " << static_cast<int>(error->_code) << ")\n";
      return 1;
    }
    std::cout << "Expected SchemaConflict: existing schema versions are immutable\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << argv[1] << ": " << error.what() << '\n';
    return 1;
  }
}
