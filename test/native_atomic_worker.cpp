// Independent exec worker for test_native_atomic.cpp. Reopening the file gives
// the atomic tests a separate process/mapping without inherited C++ state.
// Publication mode handshakes each generation; crash mode runs until SIGKILL.

#include "native_atomic.h"
#include "native_file.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  using Clock = std::chrono::steady_clock;

  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  void wait_for(mapped::AtomicWord word, std::uint64_t expected, Clock::time_point deadline)
  {
    while (word.load_acquire() != expected)
    {
      if (Clock::now() >= deadline)
      {
        throw std::runtime_error("atomic worker timed out waiting for acknowledgment");
      }
      std::this_thread::yield();
    }
  }
} // namespace

int main(int argc, char** argv)
{
  try
  {
    if (argc != 3)
    {
      throw std::runtime_error("usage: native_atomic_worker publication|crash file");
    }
    auto file = require_value(mapped::NativeFile::open_existing(argv[2], mapped::FileAccess::ReadWrite));
    auto region = require_value(file.map_read_write(0, 64));
    const auto bytes = require_value(region.writable_bytes());
    auto control = require_value(mapped::AtomicWord::bind(bytes, 0));
    auto acknowledgment = require_value(mapped::AtomicWord::bind(bytes, 8));
    const auto deadline = Clock::now() + std::chrono::seconds{5};
    const std::string_view mode{argv[1]};
    // The acknowledgment prevents reuse of ordinary body bytes until the
    // observer has read them. File growth is the externally observed effect
    // following control publication, rather than another ordinary memory read.
    if (mode == "publication")
    {
      for (std::uint64_t generation = 1; generation <= 256; ++generation)
      {
        wait_for(acknowledgment, generation - 1, deadline);
        for (std::size_t index = 16; index < 48; ++index)
        {
          bytes[index] = static_cast<std::byte>((generation + 17 * (index - 16)) & 0xff);
        }
        control.publish_control(generation);
        // This following filesystem effect must not precede control publication.
        require_value(file.resize(64 + generation));
      }
      wait_for(acknowledgment, 256, deadline);
      return 0;
    }
    // Tell the parent that stores have begun, then alternate complete patterns
    // until the parent kills this process. No destructor participates in the
    // retained-word observation after termination.
    if (mode == "crash")
    {
      control.store_release(0xaaaaaaaa55555555);
      control.publish_control(0x55555555aaaaaaaa);
      acknowledgment.store_release(1);
      while (Clock::now() < deadline)
      {
        for (unsigned iteration = 0; iteration < 1024; ++iteration)
        {
          control.store_release(0xaaaaaaaa55555555);
          control.publish_control(0x55555555aaaaaaaa);
        }
      }
      throw std::runtime_error("atomic crash worker was not terminated");
    }
    throw std::runtime_error("unknown atomic worker mode");
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
