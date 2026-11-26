// Real operating-system I/O failure qualification. Only this independent child
// lowers its file-size limit; public send must recover a denied ftruncate as a
// known rejection. No journal fault hook or malformed Session bytes are used.
#include "angaria/runtime.h"
#include "process_support.h"
#include "result_helpers.h"

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace support = angaria::test_support;

  class FileSizeLimit
  {
  public:
    explicit FileSizeLimit(std::uintmax_t bytes)
    {
      if (bytes > std::numeric_limits<rlim_t>::max() || ::getrlimit(RLIMIT_FSIZE, &_previousLimit) == -1)
      {
        throw std::runtime_error("Cannot capture the child file-size limit");
      }
      struct sigaction ignored{};
      ignored.sa_handler = SIG_IGN;
      if (sigemptyset(&ignored.sa_mask) == -1 || ::sigaction(SIGXFSZ, &ignored, &_previousSignal) == -1)
      {
        throw std::system_error(errno, std::generic_category(), "Ignore child file-size signal");
      }
      _signalChanged = true;
      auto limited = _previousLimit;
      limited.rlim_cur = static_cast<rlim_t>(bytes);
      if (::setrlimit(RLIMIT_FSIZE, &limited) == -1)
      {
        const auto error = errno;
        ::sigaction(SIGXFSZ, &_previousSignal, nullptr);
        _signalChanged = false;
        throw std::system_error(error, std::generic_category(), "Limit child file growth");
      }
      _limitChanged = true;
    }

    ~FileSizeLimit()
    {
      if (_limitChanged)
      {
        ::setrlimit(RLIMIT_FSIZE, &_previousLimit);
      }
      if (_signalChanged)
      {
        ::sigaction(SIGXFSZ, &_previousSignal, nullptr);
      }
    }

    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;

    void restore()
    {
      if (_limitChanged && ::setrlimit(RLIMIT_FSIZE, &_previousLimit) == -1)
      {
        throw std::system_error(errno, std::generic_category(), "Restore child file-size limit");
      }
      _limitChanged = false;
      if (_signalChanged && ::sigaction(SIGXFSZ, &_previousSignal, nullptr) == -1)
      {
        throw std::system_error(errno, std::generic_category(), "Restore child file-size signal");
      }
      _signalChanged = false;
      struct rlimit restored{};
      if (::getrlimit(RLIMIT_FSIZE, &restored) == -1 || restored.rlim_cur != _previousLimit.rlim_cur ||
          restored.rlim_max != _previousLimit.rlim_max)
      {
        throw std::runtime_error("Child file-size limit was not restored");
      }
    }

  private:
    struct rlimit _previousLimit{};
    struct sigaction _previousSignal{};
    bool _limitChanged{};
    bool _signalChanged{};
  };
}

int main(int argc, char** argv)
{
  try
  {
    if (argc != 4)
    {
      throw std::runtime_error("usage: native_transport_io_worker directory prior-incarnation prior-sequence");
    }
    ::alarm(8);
    auto session = support::require_value(Session::attach(argv[1]));
    auto producer = support::require_value(session.attach_task("Producer"));
    auto writer = support::require_value(producer.open_writer("Updates"));
    const auto schema = support::require_value(session.registry().find_message("Sample", 1));
    const auto codec = support::require_value(BinaryPayloadCodec::from_registry(session.registry(), schema, 4));
    const auto bytes = support::require_value(codec.encode({std::uint32_t{99}}));
    MessageHeader header;
    header._schema = schema;
    header._payloadBytes = bytes.size();
    header._flags = 0x49;
    const MessageView message{header, bytes};
    const auto publication = support::require_value(producer.next_publication_id());
    const PublicationId prior{{session.identity(), producer.claim()._task, std::stoull(argv[2])},
                              std::stoull(argv[3])};
    support::report('I');
    support::report(publication._producer._incarnation);
    support::report(publication._sequence);
    support::wait_for_parent();

    const auto path = std::filesystem::path{argv[1]} / "session.ang";
    const auto originalBytes = std::filesystem::file_size(path);
    FileSizeLimit limit(originalBytes);
    const auto rejected = writer.send(publication, message);
    const auto* error = std::get_if<Error>(&rejected);
    const auto expected = "resize native file: " + std::error_code(EFBIG, std::generic_category()).message();
    if (!error || error->_code != ErrorCode::IoError || error->_message != expected)
    {
      throw std::runtime_error("Public send did not report the actual EFBIG file-growth failure");
    }
    const auto absent = support::require_value(producer.publication_status(publication));
    const auto retained = support::require_value(producer.publication_status(prior));
    if (absent._status != AcceptanceStatus::Rejected || !absent._destinations.empty() ||
        retained._status != AcceptanceStatus::Accepted || std::filesystem::file_size(path) != originalBytes)
    {
      throw std::runtime_error("Denied file growth changed the accepted publication history");
    }
    // Restore both process settings before the parent authorizes a retry. A
    // signal/limit left behind must not make the successful retry misleading.
    limit.restore();
    support::report('F');
    support::report(static_cast<std::uint64_t>(error->_code));
    support::report(static_cast<std::uint64_t>(originalBytes));
    support::wait_for_parent();

    const auto accepted = support::require_value(writer.send(publication, message));
    const auto repeated = support::require_value(writer.send(publication, message));
    if (accepted._status != AcceptanceStatus::Accepted || repeated._status != AcceptanceStatus::Accepted ||
        accepted._publication != publication || repeated._publication != publication ||
        accepted._topologyRevision != repeated._topologyRevision)
    {
      throw std::runtime_error("Retrying the original identity did not retain one acceptance");
    }
    support::report('S');
    support::wait_for_parent();
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
