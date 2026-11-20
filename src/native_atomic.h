#pragma once

#include "mapped_superblock.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace angaria::detail::mapped
{
  // Qualifies compiler/OS/CPU prerequisites, not a particular filesystem.
  Result<LiveCompatibility> qualify_native_profile();

  // A borrowed, naturally aligned eight-byte word in coherent mapped storage.
  // The caller retains the mapping and uses atomic access exclusively for this
  // word. These native compiler operations implement the qualified ABI contract,
  // rather than portable ISO C++ serialization or power-loss durability.
  class AtomicWord
  {
  public:
    static Result<AtomicWord> bind(std::span<std::byte> bytes, std::size_t offset);

    std::uint64_t load_acquire() const noexcept;
    void store_release(std::uint64_t value) noexcept;
    // Orders both the preceding body and effects following control publication.
    void publish_control(std::uint64_t value) noexcept;

  private:
    explicit AtomicWord(std::uint64_t* address) noexcept;
    std::uint64_t* _address;
  };

  // Full compiler/hardware ordering at mutation-gate acquisition and release.
  void native_gate_fence() noexcept;
} // namespace angaria::detail::mapped
