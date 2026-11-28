#pragma once

// Include in one translation unit of a Linux test executable linked with
// --wrap=getpid. The wrapper forwards to the real libc function and counts
// calls from the capturing thread, including linked static Angaria objects.
// It does not infer syscall counts from optional Angaria diagnostics.

#include <cstdint>

#include <sys/types.h>

namespace angaria::test_support::getpid_capture_detail
{
  inline thread_local bool count_calls{};
  inline thread_local std::uint64_t captured_calls{};
}

extern "C" pid_t __real_getpid();

extern "C" pid_t __wrap_getpid()
{
  if (angaria::test_support::getpid_capture_detail::count_calls)
  {
    ++angaria::test_support::getpid_capture_detail::captured_calls;
  }
  return __real_getpid();
}

namespace angaria::test_support
{
  class GetpidCapture
  {
  public:
    GetpidCapture()
    {
      getpid_capture_detail::captured_calls = 0;
      getpid_capture_detail::count_calls = true;
    }

    ~GetpidCapture()
    {
      getpid_capture_detail::count_calls = false;
    }

    std::uint64_t stop() noexcept
    {
      getpid_capture_detail::count_calls = false;
      return getpid_capture_detail::captured_calls;
    }
  };
}
