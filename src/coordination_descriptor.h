#pragma once

#include "native_file.h"

#include <filesystem>
#include <memory>

namespace angaria::detail::mapped
{
  struct CoordinationRegistry;

  // Internal registered descriptor. Only this owner closes a coordination FD;
  // atfork invalidates child copies without unlocking the parent's description.
  struct CoordinationDescriptor
  {
    ~CoordinationDescriptor();
    int _fd{-1};
    CoordinationRegistry* _registry{};
    CoordinationDescriptor* _next{};
    bool _linked{};
    bool _exclusiveClaim{};
    FileIdentity _identity{};
  };

  Result<std::unique_ptr<CoordinationDescriptor>> open_coordination(
      int directory, const std::filesystem::path& name, const std::filesystem::path& diagnostic,
      bool isDirectory, bool create = false);
  bool coordination_busy(int error) noexcept;
  int lock_coordination(int descriptor, int operation) noexcept;
  bool owns_coordination(const CoordinationDescriptor& descriptor) noexcept;
  OperationResult claim_coordination(CoordinationDescriptor& descriptor,
                                     const std::filesystem::path& diagnostic);
} // namespace angaria::detail::mapped
