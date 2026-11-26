#pragma once

#include "result_helpers.h"
#include "native_atomic.h"
#include "runtime_state.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace angaria::test_support
{
  // Private test setup/observation only. These helpers open native resources;
  // their returned owning copies do not borrow a gate or create a Task claim.
  inline std::shared_ptr<detail::SessionState> open_runtime_state(const std::filesystem::path& directory)
  {
    auto state = std::make_shared<detail::SessionState>();
    state->_directory = std::filesystem::canonical(directory);
    state->_controller = require_value(detail::mapped::SessionController::open_existing(directory));
    return state;
  }

  inline detail::mapped::JournalSnapshot journal_snapshot(const std::filesystem::path& directory)
  {
    detail::RuntimeAccess access(open_runtime_state(directory));
    return access._snapshot;
  }

  inline std::vector<std::uint64_t> recovered_image(const std::filesystem::path& directory)
  {
    detail::RuntimeAccess access(open_runtime_state(directory));
    return access.read_words(0, static_cast<std::size_t>(access._snapshot._fileBytes / 8));
  }

  inline std::vector<std::string> claim_names(const std::filesystem::path& directory)
  {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory / "claims"))
    {
      names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
  }

  // Deliberate corruption seam; the calling fixture ensures a quiescent file.
  // This preserves real atomic writes and does not simulate journal progress.
  inline void set_atomic_word(const std::filesystem::path& directory, std::uint64_t offset,
                                std::uint64_t value)
  {
    auto file = require_value(detail::mapped::NativeFile::open_existing(directory / "session.ang",
                                                                       detail::mapped::FileAccess::ReadWrite));
    auto region = require_value(file.map_read_write(offset, 8));
    require_value(detail::mapped::AtomicWord::bind(require_value(region.writable_bytes()), 0)).store_release(value);
  }
} // namespace angaria::test_support
