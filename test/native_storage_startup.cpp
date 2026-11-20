#include "native_file.h"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  namespace fs = std::filesystem;

  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  void require(bool condition, const char* message)
  {
    if (!condition)
    {
      throw std::runtime_error(message);
    }
  }

  struct StartupStorage
  {
    StartupStorage()
    {
      try
      {
        auto pattern = (fs::temp_directory_path() / "angaria-startup-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        require(directory != nullptr, "could not create startup-test directory");
        _root = directory;
        _path = _root / "before-main.ang";
        _file = require_value(mapped::NativeFile::create_exclusive(_path, 128));
        require_value(_file.resize(64));
        _region = require_value(_file.map_read_write(3, 8));
        auto bytes = require_value(_region.writable_bytes());
        bytes.front() = std::byte{0x42};
        bytes.back() = std::byte{0x7d};
      }
      catch (const std::exception& error)
      {
        _error = error.what();
      }
      catch (...)
      {
        _error = "unexpected exception during before-main initialization";
      }
    }

    ~StartupStorage()
    {
      _region = mapped::MappedRegion{};
      _file = mapped::NativeFile{};
      std::error_code ignored;
      if (!_root.empty())
      {
        fs::remove_all(_root, ignored);
      }
    }

    fs::path _root;
    fs::path _path;
    mapped::NativeFile _file;
    mapped::MappedRegion _region;
    std::string _error;
  };

  // Intentionally invoke the library from another translation unit's global
  // constructor. This executable is separate from GTest discovery.
  StartupStorage startup_storage;
} // namespace

int main()
{
  try
  {
    if (!startup_storage._error.empty())
    {
      throw std::runtime_error("before-main native storage initialization failed: " + startup_storage._error);
    }
    const auto bytes = require_value(startup_storage._region.bytes());
    require(bytes.size() == 8, "startup mapping lost its requested range");
    require(bytes.front() == std::byte{0x42} && bytes.back() == std::byte{0x7d},
            "startup mapping lost its written bytes");
    require(require_value(startup_storage._file.size()) == 64, "startup file has the wrong size");
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
