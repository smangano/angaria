#include "angaria/types.h"

#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/interprocess/permissions.hpp>

#include <charconv>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace angaria
{
  enum class EMsgType : uint16_t
  {
    Reading = 1
  };
} // namespace angaria

namespace
{
  namespace bip = boost::interprocess;

  struct MessageSchema
  {
    angaria::EMsgType _type;
    std::string_view _name;
    std::string_view _fieldName;
  };

  // The single schema is shared by the producer and consumer commands.
  constexpr MessageSchema READING_SCHEMA{angaria::EMsgType::Reading, "Reading", "value"};

  struct QueueMessage
  {
    std::int32_t _type;
    std::int32_t _value;
  };

  static_assert(sizeof(QueueMessage) == 2 * sizeof(std::int32_t));

  constexpr auto READING_TYPE = static_cast<std::int32_t>(READING_SCHEMA._type);
  constexpr bip::message_queue::size_type QUEUE_CAPACITY = 2;
  constexpr unsigned int MESSAGE_PRIORITY = 0;

  std::string queue_name(std::string_view session)
  {
    constexpr std::string_view validCharacters =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    if (session.empty() || session.size() > 16 || session.find_first_not_of(validCharacters) != session.npos)
    {
      throw std::invalid_argument("session must contain 1 to 16 letters, digits, underscores, or dashes");
    }
    return "angaria-demo-" + std::string(session);
  }

  std::int32_t parse_value(std::string_view text)
  {
    std::int32_t value{};
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end)
    {
      throw std::invalid_argument("value must be a signed 32-bit integer");
    }
    return value;
  }

  void send_reading(bip::message_queue& queue, std::int32_t value)
  {
    const QueueMessage message{READING_TYPE, value};
    if (!queue.try_send(&message, sizeof(message), MESSAGE_PRIORITY))
    {
      throw std::runtime_error("queue is full");
    }
  }

  void receive_reading(bip::message_queue& queue)
  {
    QueueMessage message{};
    bip::message_queue::size_type size{};
    unsigned int priority{};
    if (!queue.try_receive(&message, sizeof(message), size, priority))
    {
      throw std::runtime_error("queue is empty");
    }
    if (size != sizeof(message) || priority != MESSAGE_PRIORITY || message._type != READING_TYPE)
    {
      throw std::runtime_error("unrecognized queue message");
    }

    std::cout << READING_SCHEMA._name << ' ' << READING_SCHEMA._fieldName << '=' << message._value << '\n';
  }

  void run_command(std::string_view command, const std::string& queueName, std::string_view value)
  {
    if (command == "create")
    {
      const bip::permissions permissions(0600);
      bip::message_queue queue(bip::create_only, queueName.c_str(), QUEUE_CAPACITY, sizeof(QueueMessage),
                               permissions);
      return;
    }
    if (command == "remove")
    {
      if (!bip::message_queue::remove(queueName.c_str()))
      {
        throw std::runtime_error("could not remove the demo queue");
      }
      return;
    }

    bip::message_queue queue(bip::open_only, queueName.c_str());
    if (queue.get_max_msg() != QUEUE_CAPACITY || queue.get_max_msg_size() != sizeof(QueueMessage))
    {
      throw std::runtime_error("queue does not match the demo configuration");
    }
    if (command == "send")
    {
      send_reading(queue, parse_value(value));
    }
    else
    {
      receive_reading(queue);
    }
  }
} // namespace

int main(int argc, char** argv)
{
  const std::string_view command = argc > 1 ? argv[1] : "";
  const bool validCommand =
      command == "send" ? argc == 4
                        : (command == "create" || command == "receive" || command == "remove") && argc == 3;
  if (!validCommand)
  {
    std::cerr << "Usage: angaria_queue_demo create|receive|remove SESSION\n"
                 "       angaria_queue_demo send SESSION VALUE\n";
    return 2;
  }

  try
  {
    run_command(command, queue_name(argv[2]), argc == 4 ? argv[3] : "");
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "angaria_queue_demo: " << error.what() << '\n';
    return 1;
  }
}
