#include "native_atomic.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <variant>

#include <sys/types.h>

namespace angaria::detail::mapped
{
  namespace
  {
#if defined(__linux__) && defined(__x86_64__)
    constexpr std::uint32_t native_profile = 0x00010001;
#elif defined(__linux__) && defined(__aarch64__)
    constexpr std::uint32_t native_profile = 0x00010002;
#elif defined(__APPLE__) && defined(__x86_64__)
    constexpr std::uint32_t native_profile = 0x00020001;
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    constexpr std::uint32_t native_profile = 0x00020002;
#else
    constexpr std::uint32_t native_profile = 0;
#endif

#if defined(__GNUC__) || defined(__clang__)
    constexpr bool native_atomics = __atomic_always_lock_free(sizeof(std::uint64_t), nullptr);
#else
    constexpr bool native_atomics = false;
#endif

    constexpr bool native_representation = std::endian::native == std::endian::little &&
                                            sizeof(void*) == 8 && sizeof(off_t) == 8 &&
                                            std::numeric_limits<off_t>::is_signed &&
                                            alignof(std::uint64_t) <= 8;
  } // namespace

  Result<LiveCompatibility> qualify_native_profile()
  {
    if (native_profile == 0 || !native_representation || !native_atomics)
    {
      return Error{ErrorCode::IoError,
                   "Host requires a supported little-endian 64-bit profile with always-lock-free eight-byte atomics",
                   "native_atomic.profile"};
    }
    return LiveCompatibility{._liveAbiProfile = native_profile};
  }

  AtomicWord::AtomicWord(std::uint64_t* address) noexcept : _address(address)
  {
  }

  Result<AtomicWord> AtomicWord::bind(std::span<std::byte> bytes, std::size_t offset)
  {
    const auto supported = qualify_native_profile();
    if (const auto* error = std::get_if<Error>(&supported))
    {
      return *error;
    }
    if (!contains_range(bytes.size(), offset, sizeof(std::uint64_t)))
    {
      return Error{ErrorCode::InvalidArgument, "Atomic word is outside the supplied range", "native_atomic.word"};
    }
    auto* const address = bytes.data() + offset;
    if (reinterpret_cast<std::uintptr_t>(address) % 8 != 0)
    {
      return Error{ErrorCode::InvalidArgument, "Atomic word requires eight-byte alignment", "native_atomic.word"};
    }
    return AtomicWord{reinterpret_cast<std::uint64_t*>(address)};
  }

  std::uint64_t AtomicWord::load_acquire() const noexcept
  {
#if defined(__GNUC__) || defined(__clang__)
    if constexpr (native_atomics)
    {
      return __atomic_load_n(_address, __ATOMIC_ACQUIRE);
    }
#endif
    std::terminate();
  }

  void AtomicWord::store_release(std::uint64_t value) noexcept
  {
#if defined(__GNUC__) || defined(__clang__)
    if constexpr (native_atomics)
    {
      __atomic_store_n(_address, value, __ATOMIC_RELEASE);
      return;
    }
#endif
    std::terminate();
  }

  void AtomicWord::publish_control(std::uint64_t value) noexcept
  {
#if defined(__GNUC__) || defined(__clang__)
    if constexpr (native_atomics)
    {
      __atomic_store_n(_address, value, __ATOMIC_SEQ_CST);
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
      return;
    }
#endif
    std::terminate();
  }

  void native_gate_fence() noexcept
  {
#if defined(__GNUC__) || defined(__clang__)
    if constexpr (native_atomics)
    {
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
      return;
    }
#endif
    std::terminate();
  }
} // namespace angaria::detail::mapped
