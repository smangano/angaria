// Independent exec process for private producer-log qualification. Pipes carry
// test barriers only; accepted data always travels through the actual mapped log.
#include "producer_log_fixture.h"
#include "process_support.h"
#include "coordination_descriptor.h"
#include "native_claim.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <unistd.h>

namespace
{
  namespace recorded = angaria::detail::recorded;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;

  template <typename T>
  bool wrong_owner(const angaria::Result<T>& result)
  {
    const auto* error = std::get_if<angaria::Error>(&result);
    return error != nullptr && error->_code == angaria::ErrorCode::WrongOwner;
  }

  template <typename Check>
  void check_in_fork_child(Check&& check)
  {
    const auto child = ::fork();
    if (child == -1)
    {
      throw std::runtime_error("Cannot fork ownership-token observer");
    }
    if (child == 0)
    {
      ::alarm(3);
      try
      {
        ::_exit(check() ? 0 : 1);
      }
      catch (...)
      {
        ::_exit(2);
      }
    }
    const auto status = support::wait_for_exit(child, "Cannot reap ownership-token child", "Ownership-token child deadline");
    if (support::exit_code(status) != 0)
    {
      throw std::runtime_error("Forked ownership checks did not reject inherited state");
    }
  }

  void process_token_first(const std::filesystem::path& directory)
  {
    // This fresh exec has opened no NativeFile or NativeClaim. Binding a token
    // alone must install the handler, with no descriptor needed to keep it alive.
    if (mapped::owns_process(mapped::ProcessToken{}))
    {
      throw std::runtime_error("An empty process token must not own a process");
    }
    const auto token = support::require_value(mapped::bind_process_token(directory));
    const auto copied = token;
    if (!mapped::owns_process(token) || !mapped::owns_process(copied))
    {
      throw std::runtime_error("A bound process token must belong to this process");
    }
    check_in_fork_child([&]
    {
      return !mapped::owns_process(token) && !mapped::owns_process(copied) &&
          !mapped::owns_process(mapped::ProcessToken{}) &&
          wrong_owner(mapped::bind_process_token(directory)) &&
          wrong_owner(mapped::NativeClaim::create_exclusive(directory / "unexpected.claim"));
    });
    if (!mapped::owns_process(token) || !mapped::owns_process(copied))
    {
      throw std::runtime_error("Fork invalidated the parent's process token");
    }
    support::report('T');
  }

  void standalone_reader_first(const std::filesystem::path& directory)
  {
    // No writer or lifetime claim has initialized coordination in this exec.
    // Reader opening itself must bind fork identity before exposing mappings.
    const auto paths = support::producer_log_paths(directory);
    const auto options = support::producer_log_options();
    auto reader = support::require_value(recorded::ProducerLogReader::open(paths._log, options));
    const auto before = support::require_value(reader.capture());
    check_in_fork_child([&]
    {
      const bool rejected = wrong_owner(reader.committed_end()) && wrong_owner(reader.capture()) &&
          wrong_owner(recorded::ProducerLogReader::open(paths._log, options)) &&
          wrong_owner(mapped::NativeClaim::create_exclusive(directory / "unexpected.claim"));
      reader = recorded::ProducerLogReader{};
      return rejected;
    });
    const auto after = support::require_value(reader.capture());
    if (before.record_count() != after.record_count())
    {
      throw std::runtime_error("Child rejection changed the parent's replay cut");
    }
    support::report('R');
    support::report(static_cast<std::uint64_t>(after.record_count()));
  }

  class PauseAtCheckpoint final : public recorded::ProducerLogObserver
  {
  public:
    explicit PauseAtCheckpoint(recorded::ProducerLogCheckpoint point) : _point(point)
    {
    }

    angaria::OperationResult checkpoint(recorded::ProducerLogCheckpoint point) override
    {
      if (!_stopped && point == _point)
      {
        _stopped = true;
        support::report('R');
        support::wait_for_parent();
      }
      return std::monostate{};
    }

  private:
    recorded::ProducerLogCheckpoint _point;
    bool _stopped{};
  };
}

int main(int argc, char** argv)
{
  try
  {
    if (argc != 3)
    {
      throw std::runtime_error("usage: producer_log_worker directory partial|before|after|hold|process_token|reader_first");
    }
    ::alarm(15);
    const std::filesystem::path directory{argv[1]};
    const std::string mode{argv[2]};
    if (mode == "process_token")
    {
      process_token_first(directory);
      return 0;
    }
    if (mode == "reader_first")
    {
      standalone_reader_first(directory);
      return 0;
    }
    if (mode != "partial" && mode != "before" && mode != "after" && mode != "hold")
    {
      throw std::runtime_error("Unknown producer-log worker checkpoint");
    }
    const auto point = mode == "partial" ? recorded::ProducerLogCheckpoint::PartialMaterialization :
                       mode == "before" ? recorded::ProducerLogCheckpoint::BeforeCommit :
                                          recorded::ProducerLogCheckpoint::AfterCommit;
    PauseAtCheckpoint observer{point};
    const auto options = support::producer_log_options();
    auto writer = support::require_value(recorded::ProducerLog::open_writer(
        support::producer_log_paths(directory), options, mode == "hold" ? nullptr : &observer));
    const auto claim = support::require_value(writer.claim());
    support::report('O');
    support::report(claim._incarnation);
    if (mode == "hold")
    {
      support::wait_for_parent();
      return 0;
    }

    const auto publication = support::require_value(writer.next_publication_id());
    const std::array payload{std::byte{0x31}, std::byte{0x42}, std::byte{0x53}, std::byte{0x64}};
    const auto appended = support::require_value(writer.append(
        support::producer_append_request(publication, payload)));
    support::report('A');
    support::report(appended._recordOrdinal);
    support::report(appended._queueSequence);
  }
  catch (const std::exception& error)
  {
    std::cerr << "producer-log worker: " << error.what() << '\n';
    return 1;
  }
}
