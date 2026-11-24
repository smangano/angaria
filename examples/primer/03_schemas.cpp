// A schema describes the ordered Fields in one version of a message. This
// example registers definitions; payload encoding and delivery come later.

#include "example_support.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <variant>

void print_schema(angaria::Registry registry, angaria::SchemaId id)
{
  const auto message = primer::value(registry.describe_message(id));
  std::cout << message._name << " v" << id._version << " Fields:";
  for (const auto field : message._fields)
  {
    std::cout << ' ' << primer::value(registry.describe_field(field))._name;
  }
  std::cout << '\n';
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
    const auto options = primer::session_options(argv[1], "Versioned message definitions");
    angaria::TaskDescriptor creator;
    creator._name = "Setup";
    auto created = primer::value(angaria::Session::create(options, creator));
    auto registry = created._creator.registry();

    // Field IDs are application-chosen full 32-bit values with the 0xEE prefix.
    // Fixed scalar sizes are encoded byte widths, not sizeof a C++ object.
    angaria::FieldDescriptor price;
    price._id = static_cast<angaria::FieldId>(0xee000001);
    price._name = "PriceTicks";
    price._base = angaria::EFieldBase::BASE_INT64;
    price._size = 8;
    primer::value(registry.register_field(price));
    angaria::FieldDescriptor quantity;
    quantity._id = static_cast<angaria::FieldId>(0xee000002);
    quantity._name = "Quantity";
    quantity._base = angaria::EFieldBase::BASE_UINT64;
    quantity._size = 8;
    primer::value(registry.register_field(quantity));
    angaria::FieldDescriptor instrument;
    instrument._id = static_cast<angaria::FieldId>(0xee000003);
    instrument._name = "Instrument";
    instrument._base = angaria::EFieldBase::BASE_UINT32;
    instrument._size = 4;
    primer::value(registry.register_field(instrument));

    angaria::MessageDescriptor quote;
    quote._schema = {static_cast<angaria::MessageType>(1), 1};
    quote._name = "Quote";
    quote._fields = {price._id, quantity._id};
    quote._doc = "A market quote";
    const auto version_one = primer::value(registry.register_message(quote));
    auto extended_quote = quote;
    extended_quote._schema._version = 2;
    extended_quote._fields.push_back(instrument._id);
    const auto version_two = primer::value(registry.register_message(extended_quote));

    // Registering a new version preserves the old version and its Field order.
    print_schema(created._session.registry(), version_one);
    print_schema(created._session.registry(), version_two);
    const auto found_one = primer::value(registry.find_message("Quote", 1));
    const auto found_two = primer::value(registry.find_message("Quote", 2));
    // Registration metadata records which Task introduced this definition.
    const auto metadata = primer::value(registry.metadata(found_one));
    const auto retry = primer::value(registry.register_message(quote));
    std::cout << "Exact schema retry kept version: " << retry._version << '\n'
              << "Lookup of Quote v1 kept version: " << found_one._version << '\n'
              << "Lookup of Quote v2 kept version: " << found_two._version << '\n'
              << "Quote v1 registered by Task: " << static_cast<unsigned>(metadata._createdBy) << '\n';

    auto incompatible = quote;
    incompatible._doc = "A changed existing version";
    const auto conflict = registry.register_message(incompatible);
    const auto* error = std::get_if<angaria::Error>(&conflict);
    if (!error || error->_code != angaria::ErrorCode::SchemaConflict)
    {
      throw std::runtime_error("Expected SchemaConflict for a changed existing schema");
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
