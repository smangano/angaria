#pragma once

// Include this header in exactly one translation unit of an isolated test
// executable: replacement new/delete operators cannot be inline or shared by
// several translation units. They count successful C++ allocation requests on
// the capturing thread; they do not measure malloc calls or retained bytes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace angaria::test_support::allocation_capture_detail
{
  // Keep this counter independent of optional runtime diagnostics so allocation
  // guards also work in ordinary builds.
  inline thread_local bool count_allocations{};
  inline thread_local std::uint64_t captured_allocations{};

  inline void* allocate(std::size_t bytes, std::size_t alignment = 0)
  {
    for (;;)
    {
      void* result{};
      if (alignment == 0)
      {
        result = std::malloc(bytes == 0 ? 1 : bytes);
      }
      else if (::posix_memalign(&result, std::max(alignment, alignof(void*)), bytes == 0 ? 1 : bytes) != 0)
      {
        result = nullptr;
      }
      if (result)
      {
        if (count_allocations)
        {
          ++captured_allocations;
        }
        return result;
      }
      const auto handler = std::get_new_handler();
      if (!handler)
      {
        throw std::bad_alloc{};
      }
      handler();
    }
  }

  inline void* allocate_nothrow(std::size_t bytes, std::size_t alignment = 0) noexcept
  {
    try
    {
      return angaria::test_support::allocation_capture_detail::allocate(bytes, alignment);
    }
    catch (...)
    {
      return nullptr;
    }
  }
}

void* operator new(std::size_t bytes)
{
  return angaria::test_support::allocation_capture_detail::allocate(bytes);
}
void* operator new[](std::size_t bytes)
{
  return angaria::test_support::allocation_capture_detail::allocate(bytes);
}
void* operator new(std::size_t bytes, std::align_val_t alignment)
{
  return angaria::test_support::allocation_capture_detail::allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment)
{
  return angaria::test_support::allocation_capture_detail::allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
  return angaria::test_support::allocation_capture_detail::allocate_nothrow(bytes);
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept
{
  return angaria::test_support::allocation_capture_detail::allocate_nothrow(bytes);
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  return angaria::test_support::allocation_capture_detail::allocate_nothrow(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  return angaria::test_support::allocation_capture_detail::allocate_nothrow(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* address) noexcept
{
  std::free(address);
}
void operator delete[](void* address) noexcept
{
  std::free(address);
}
void operator delete(void* address, std::size_t) noexcept
{
  std::free(address);
}
void operator delete[](void* address, std::size_t) noexcept
{
  std::free(address);
}
void operator delete(void* address, std::align_val_t) noexcept
{
  std::free(address);
}
void operator delete[](void* address, std::align_val_t) noexcept
{
  std::free(address);
}
void operator delete(void* address, std::size_t, std::align_val_t) noexcept
{
  std::free(address);
}
void operator delete[](void* address, std::size_t, std::align_val_t) noexcept
{
  std::free(address);
}
void operator delete(void* address, const std::nothrow_t&) noexcept
{
  std::free(address);
}
void operator delete[](void* address, const std::nothrow_t&) noexcept
{
  std::free(address);
}
void operator delete(void* address, std::align_val_t, const std::nothrow_t&) noexcept
{
  std::free(address);
}
void operator delete[](void* address, std::align_val_t, const std::nothrow_t&) noexcept
{
  std::free(address);
}

namespace angaria::test_support
{
  class AllocationCapture
  {
  public:
    AllocationCapture()
    {
      allocation_capture_detail::captured_allocations = 0;
      allocation_capture_detail::count_allocations = true;
    }

    ~AllocationCapture()
    {
      allocation_capture_detail::count_allocations = false;
    }

    std::uint64_t stop() noexcept
    {
      allocation_capture_detail::count_allocations = false;
      return allocation_capture_detail::captured_allocations;
    }
  };
} // namespace angaria::test_support
