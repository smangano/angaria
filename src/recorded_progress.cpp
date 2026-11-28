#include "recorded_progress.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"
#include "native_atomic.h"
#include "native_claim.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

namespace angaria::detail::recorded
{
  namespace
  {
    constexpr std::string_view progress_magic = "ANGAPROG";
    constexpr std::uint16_t progress_format_major = 1;
    constexpr std::uint16_t progress_format_minor = 0;
    constexpr std::uint16_t progress_synchronization = 2;
    using HeaderBytes = std::array<std::byte, progress_header_bytes>;
    using StateBytes = std::array<std::byte, progress_state_bytes>;
    using ReceiptBytes = std::array<std::byte, progress_receipt_bytes>;
    using ImageBytes = std::array<std::byte, progress_image_bytes>;

    template <typename T>
    T value(Result<T> result)
    {
      return value_or_throw<CodecFailure>(std::move(result));
    }

    void require(bool condition, ErrorCode code, std::string_view message)
    {
      codec_require(condition, code, "recorded_progress", message);
    }

    template <typename T, typename Function>
    Result<T> protect(Function&& operation)
    {
      try
      {
        return operation();
      }
      catch (const CodecFailure& failure)
      {
        return failure._error;
      }
      catch (const std::bad_alloc&)
      {
        return Error{ErrorCode::CapacityExceeded, "Cannot allocate progress owner", "recorded_progress"};
      }
      catch (...)
      {
        return Error{ErrorCode::IoError, "Progress operation did not return normally", "recorded_progress"};
      }
    }

    std::uint64_t checked_size(const ProgressOptions& options)
    {
      require(!options._path.empty() && options._session != SessionId{} &&
                  options._consumer != TaskId::Invalid && options._source != SourceQueueSetId::Invalid &&
                  options._queue != QueueId::Invalid && options._connectionGeneration != 0 &&
                  options._connectionIdentity._inode != 0 && options._publicationCapacity != 0 &&
                  options._publicationCapacity <= (mapped::max_log_bytes - progress_receipts_offset) / progress_receipt_bytes,
              ErrorCode::InvalidArgument, "Invalid progress identity or receipt capacity");
      require(value(mapped::qualify_native_profile())._liveAbiProfile == options._liveAbiProfile,
              ErrorCode::UnsupportedPolicy, "Unsupported progress native profile");
      const auto size = progress_receipts_offset + options._publicationCapacity * progress_receipt_bytes;
      require(fits_host_size(size), ErrorCode::CapacityExceeded, "Progress reservation exceeds host capacity");
      return size;
    }

    HeaderBytes header_bytes(const ProgressOptions& options, mapped::FileIdentity identity)
    {
      HeaderBytes bytes{};
      for (std::size_t index = 0; index != progress_magic.size(); ++index)
      {
        bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(progress_magic[index]));
      }
      write_unsigned_le<std::uint16_t>(bytes, 8, progress_format_major);
      write_unsigned_le<std::uint16_t>(bytes, 10, progress_format_minor);
      write_unsigned_le<std::uint32_t>(bytes, 12, progress_receipts_offset);
      write_unsigned_le<std::uint64_t>(bytes, 16, options._session._high);
      write_unsigned_le<std::uint64_t>(bytes, 24, options._session._low);
      write_unsigned_le<std::uint16_t>(bytes, 32, static_cast<std::uint16_t>(options._consumer));
      write_unsigned_le<std::uint16_t>(bytes, 34, static_cast<std::uint16_t>(options._source));
      write_unsigned_le<std::uint16_t>(bytes, 36, static_cast<std::uint16_t>(options._queue));
      write_unsigned_le<std::uint64_t>(bytes, 40, options._connectionGeneration);
      write_unsigned_le<std::uint64_t>(bytes, 48, options._connectionIdentity._device);
      write_unsigned_le<std::uint64_t>(bytes, 56, options._connectionIdentity._inode);
      write_unsigned_le<std::uint64_t>(bytes, 64, options._publicationCapacity);
      write_unsigned_le<std::uint64_t>(bytes, 72, identity._device);
      write_unsigned_le<std::uint64_t>(bytes, 80, identity._inode);
      write_unsigned_le<std::uint64_t>(bytes, 88, progress_receipts_offset + options._publicationCapacity * progress_receipt_bytes);
      write_unsigned_le<std::uint32_t>(bytes, 96, options._liveAbiProfile);
      write_unsigned_le<std::uint16_t>(bytes, 100, progress_synchronization);
      write_unsigned_le<std::uint16_t>(bytes, 102, 8);
      write_unsigned_le<std::uint64_t>(bytes, 104, 1); // Acknowledged progress feature.
      return bytes;
    }

    std::uint64_t time_word(const std::optional<UtcTimestamp>& time)
    {
      return time ? std::bit_cast<std::uint64_t>(time->time_since_epoch().count()) : 0;
    }

    std::optional<UtcTimestamp> read_time(std::span<const std::byte> bytes, std::size_t offset, bool present)
    {
      const auto word = read_unsigned_le<std::uint64_t>(bytes, offset);
      require(present || word == 0, ErrorCode::IoError, "Absent progress time has nonzero bytes");
      return present ? std::optional{UtcTimestamp{std::chrono::nanoseconds{std::bit_cast<std::int64_t>(word)}}} : std::nullopt;
    }

    void validate_source(const ProgressState& source, std::uint64_t capacity, ErrorCode code)
    {
      require(source._acknowledged <= capacity && source._credit <= capacity &&
                  source._credit >= source._acknowledged && source._credit - source._acknowledged <= 1 &&
                  bool(source._lastAcknowledgedAt) == (source._acknowledged != 0),
              code, "Progress Queue watermarks or acknowledgment presence are invalid");
      if (source._actorIncarnation == 0)
      {
        require(source == ProgressState{}, code, "Unattached progress contains consumer state");
      }
      if (source._pinSequence != 0)
      {
        require(source._actorIncarnation != 0 && source._acknowledged < capacity &&
                    source._pinSequence == source._acknowledged + 1 && source._pinSequence == source._credit &&
                    source._pinIssuance != 0 && source._pinIssuance == source._issuance && bool(source._dequeuedAt),
                code, "Progress pin is not the oldest pending delivery");
      }
      else
      {
        require(source._pinIssuance == 0 && !source._dequeuedAt, code, "Idle progress retains an active pin");
      }
    }

    StateBytes encode_source(const ProgressState& source)
    {
      StateBytes bytes{};
      write_unsigned_le<std::uint64_t>(bytes, 0, source._actorIncarnation);
      write_unsigned_le<std::uint64_t>(bytes, 8, source._issuance);
      write_unsigned_le<std::uint64_t>(bytes, 16, source._pinSequence);
      write_unsigned_le<std::uint64_t>(bytes, 24, source._pinIssuance);
      write_unsigned_le<std::uint64_t>(bytes, 32, source._acknowledged);
      write_unsigned_le<std::uint64_t>(bytes, 40, source._credit);
      write_unsigned_le<std::uint64_t>(bytes, 48, (source._dequeuedAt ? 1u : 0u) | (source._lastAcknowledgedAt ? 2u : 0u));
      write_unsigned_le<std::uint64_t>(bytes, 56, time_word(source._dequeuedAt));
      write_unsigned_le<std::uint64_t>(bytes, 64, time_word(source._lastAcknowledgedAt));
      return bytes;
    }

    ProgressState decode_source(std::span<const std::byte> bytes, std::uint64_t capacity)
    {
      const auto flags = read_unsigned_le<std::uint64_t>(bytes, 48);
      require(flags <= 3 && all_zero(bytes.subspan(72)), ErrorCode::IoError, "Invalid progress Source encoding");
      ProgressState source;
      source._actorIncarnation = read_unsigned_le<std::uint64_t>(bytes, 0);
      source._issuance = read_unsigned_le<std::uint64_t>(bytes, 8);
      source._pinSequence = read_unsigned_le<std::uint64_t>(bytes, 16);
      source._pinIssuance = read_unsigned_le<std::uint64_t>(bytes, 24);
      source._acknowledged = read_unsigned_le<std::uint64_t>(bytes, 32);
      source._credit = read_unsigned_le<std::uint64_t>(bytes, 40);
      source._dequeuedAt = read_time(bytes, 56, (flags & 1) != 0);
      source._lastAcknowledgedAt = read_time(bytes, 64, (flags & 2) != 0);
      validate_source(source, capacity, ErrorCode::IoError);
      return source;
    }

    void validate_receipt(const ProgressReceipt& receipt, ErrorCode code)
    {
      require(receipt._state == ProgressReceiptState::Pending || receipt._state == ProgressReceiptState::Issued ||
                  receipt._state == ProgressReceiptState::Acknowledged,
              code, "Unsupported progress receipt state");
      if (receipt._consumerIncarnation == 0)
      {
        require(receipt == ProgressReceipt{}, code, "Never-issued receipt contains issuance fields");
      }
      else
      {
        require(receipt._issuance != 0 && bool(receipt._dequeuedAt) &&
                    bool(receipt._acknowledgedAt) == (receipt._state == ProgressReceiptState::Acknowledged),
                code, "Receipt issuance or timestamp presence is invalid");
      }
    }

    ReceiptBytes encode_receipt(const ProgressReceipt& receipt)
    {
      ReceiptBytes bytes{};
      write_unsigned_le<std::uint64_t>(bytes, 0, static_cast<std::uint64_t>(receipt._state));
      write_unsigned_le<std::uint64_t>(bytes, 8, receipt._consumerIncarnation);
      write_unsigned_le<std::uint64_t>(bytes, 16, receipt._issuance);
      write_unsigned_le<std::uint64_t>(bytes, 24, (receipt._dequeuedAt ? 1u : 0u) | (receipt._acknowledgedAt ? 2u : 0u));
      write_unsigned_le<std::uint64_t>(bytes, 32, time_word(receipt._dequeuedAt));
      write_unsigned_le<std::uint64_t>(bytes, 40, time_word(receipt._acknowledgedAt));
      return bytes;
    }

    ProgressReceipt decode_receipt(std::span<const std::byte> bytes)
    {
      const auto flags = read_unsigned_le<std::uint64_t>(bytes, 24);
      require(flags <= 3 && all_zero(bytes.subspan(48)), ErrorCode::IoError, "Invalid progress receipt encoding");
      ProgressReceipt receipt;
      receipt._state = static_cast<ProgressReceiptState>(read_unsigned_le<std::uint64_t>(bytes, 0));
      receipt._consumerIncarnation = read_unsigned_le<std::uint64_t>(bytes, 8);
      receipt._issuance = read_unsigned_le<std::uint64_t>(bytes, 16);
      receipt._dequeuedAt = read_time(bytes, 32, (flags & 1) != 0);
      receipt._acknowledgedAt = read_time(bytes, 40, (flags & 2) != 0);
      validate_receipt(receipt, ErrorCode::IoError);
      return receipt;
    }

    void validate_image(const ProgressImage& image, const ProgressOptions& options, ErrorCode code)
    {
      require(image._generation != 0 && image._receiptSequence <= options._publicationCapacity,
              code, "Invalid progress operation generation or receipt sequence");
      validate_source(image._source, options._publicationCapacity, code);
      validate_receipt(image._receipt, code);
      const auto& source = image._source;
      const auto& receipt = image._receipt;
      if (image._receiptSequence == 0)
      {
        require(receipt == ProgressReceipt{} && source._pinSequence == 0,
                code, "Progress operation omits a changed pin receipt");
      }
      else
      {
        require(receipt._consumerIncarnation != 0 && receipt._consumerIncarnation <= source._actorIncarnation &&
                    image._receiptSequence <= source._credit,
                code, "Changed receipt is outside consumer progress");
        if (receipt._state == ProgressReceiptState::Issued)
        {
          require(image._receiptSequence == source._pinSequence && receipt._consumerIncarnation == source._actorIncarnation &&
                      receipt._issuance == source._pinIssuance && receipt._dequeuedAt == source._dequeuedAt,
                  code, "Issued receipt differs from the Source pin");
        }
        else if (receipt._state == ProgressReceiptState::Acknowledged)
        {
          require(source._pinSequence == 0 && image._receiptSequence == source._acknowledged &&
                      receipt._acknowledgedAt == source._lastAcknowledgedAt,
                  code, "Terminal receipt differs from Queue acknowledgment progress");
        }
        else
        {
          require(source._pinSequence == 0 && source._acknowledged < options._publicationCapacity &&
                      image._receiptSequence == source._acknowledged + 1,
                  code, "Released receipt is not the oldest pending delivery");
        }
      }
    }

    ImageBytes encode_image(const ProgressImage& image, const ProgressOptions& options)
    {
      ImageBytes bytes{};
      write_unsigned_le<std::uint64_t>(bytes, 0, image._generation);
      write_unsigned_le<std::uint64_t>(bytes, 8, options._session._high);
      write_unsigned_le<std::uint64_t>(bytes, 16, options._session._low);
      write_unsigned_le<std::uint64_t>(bytes, 24, options._connectionGeneration);
      write_unsigned_le<std::uint16_t>(bytes, 32, static_cast<std::uint16_t>(options._queue));
      write_unsigned_le<std::uint16_t>(bytes, 34, static_cast<std::uint16_t>(options._source));
      write_unsigned_le<std::uint16_t>(bytes, 36, static_cast<std::uint16_t>(options._consumer));
      write_unsigned_le<std::uint64_t>(bytes, 40, image._receiptSequence);
      const auto source = encode_source(image._source);
      const auto receipt = encode_receipt(image._receipt);
      std::copy(source.begin(), source.end(), bytes.begin() + 48);
      std::copy(receipt.begin(), receipt.end(), bytes.begin() + 128);
      return bytes;
    }

    ProgressImage decode_image(std::span<const std::byte> bytes, const ProgressOptions& options, std::uint64_t selected)
    {
      require(read_unsigned_le<std::uint64_t>(bytes, 0) == selected &&
                  read_unsigned_le<std::uint64_t>(bytes, 8) == options._session._high &&
                  read_unsigned_le<std::uint64_t>(bytes, 16) == options._session._low &&
                  read_unsigned_le<std::uint64_t>(bytes, 24) == options._connectionGeneration &&
                  read_unsigned_le<std::uint16_t>(bytes, 32) == static_cast<std::uint16_t>(options._queue) &&
                  read_unsigned_le<std::uint16_t>(bytes, 34) == static_cast<std::uint16_t>(options._source) &&
                  read_unsigned_le<std::uint16_t>(bytes, 36) == static_cast<std::uint16_t>(options._consumer) &&
                  all_zero(bytes.subspan(38, 2)) && all_zero(bytes.subspan(192)),
              ErrorCode::IoError, "Selected progress image has invalid generation or context");
      ProgressImage image;
      image._generation = selected;
      image._source = decode_source(bytes.subspan(48, progress_state_bytes), options._publicationCapacity);
      image._receiptSequence = read_unsigned_le<std::uint64_t>(bytes, 40);
      image._receipt = decode_receipt(bytes.subspan(128, progress_receipt_bytes));
      validate_image(image, options, ErrorCode::IoError);
      return image;
    }

    void validate_transition(const ProgressSnapshot& before, const ProgressImage& image)
    {
      const auto& old = before._source;
      const auto& after = image._source;
      require(before._generation != std::numeric_limits<std::uint64_t>::max() &&
                  image._generation == before._generation + 1 && after._actorIncarnation >= old._actorIncarnation &&
                  after._acknowledged >= old._acknowledged && after._acknowledged - old._acknowledged <= 1 &&
                  after._credit >= old._credit && after._credit - old._credit <= 1,
              ErrorCode::InvalidArgument, "Progress operation skips or reverses a counter");
      if (after._actorIncarnation != old._actorIncarnation)
      {
        require(after._issuance == 0 && after._pinSequence == 0 && after._acknowledged == old._acknowledged &&
                    after._credit == old._credit && after._lastAcknowledgedAt == old._lastAcknowledgedAt &&
                    image._receiptSequence == old._pinSequence &&
                    (old._pinSequence == 0 || (image._receipt._state == ProgressReceiptState::Pending &&
                     image._receipt._consumerIncarnation == old._actorIncarnation &&
                     image._receipt._issuance == old._pinIssuance && image._receipt._dequeuedAt == old._dequeuedAt)),
                ErrorCode::InvalidArgument, "Consumer replacement does not release the predecessor pin");
      }
      else if (old._pinSequence == 0 && after._pinSequence != 0)
      {
        require(old._issuance != std::numeric_limits<std::uint64_t>::max() && after._issuance == old._issuance + 1 &&
                    after._acknowledged == old._acknowledged && after._lastAcknowledgedAt == old._lastAcknowledgedAt &&
                    image._receiptSequence == after._pinSequence && image._receipt._state == ProgressReceiptState::Issued,
                ErrorCode::InvalidArgument, "Fresh progress issuance is inconsistent");
      }
      else if (old._pinSequence != 0 && after._pinSequence == 0)
      {
        const bool acknowledged = image._receipt._state == ProgressReceiptState::Acknowledged;
        require(after._issuance == old._issuance && after._credit == old._credit &&
                    image._receiptSequence == old._pinSequence &&
                    image._receipt._consumerIncarnation == old._actorIncarnation &&
                    image._receipt._issuance == old._pinIssuance && image._receipt._dequeuedAt == old._dequeuedAt &&
                    after._acknowledged == (acknowledged ? old._pinSequence : old._acknowledged) &&
                    (acknowledged || after._lastAcknowledgedAt == old._lastAcknowledgedAt),
                ErrorCode::InvalidArgument, "Progress resolution differs from the current pin");
      }
      else
      {
        require(false, ErrorCode::InvalidArgument, "Progress operation does not change attachment or a pin");
      }
    }
  } // namespace

  struct ProgressStore::State
  {
    // The actual-inode claim outlives the writable mapping.
    mapped::NativeClaim _claim;
    mapped::NativeFile _file;
    mapped::MappedRegion _mapping;
    ProgressOptions _options;
    std::span<std::byte> _bytes;
    std::optional<mapped::AtomicWord> _selector;
    std::optional<mapped::AtomicWord> _applied;
    ProgressSnapshot _current;
    ProgressObserver* _observer{};
    std::thread::id _thread{std::this_thread::get_id()};
    bool _failed{};
    bool _busy{};

    void check(bool allowFailed = false) const
    {
      value(_claim.check_owner());
      require(_thread == std::this_thread::get_id(), ErrorCode::WrongOwner, "Progress owner belongs to another thread");
      require(!_busy, ErrorCode::InvalidState, "Progress operation cannot reenter");
      require(allowFailed || !_failed, ErrorCode::IoError, "Progress requires settlement after an interrupted commit");
    }

    void checkpoint(ProgressCheckpoint point)
    {
      if (_observer)
      {
        value(_observer->checkpoint(point));
      }
    }

    std::span<std::byte> receipt_bytes(std::uint64_t sequence) const
    {
      require(sequence != 0 && sequence <= _options._publicationCapacity, ErrorCode::InvalidArgument, "Receipt sequence exceeds its reservation");
      return _bytes.subspan(progress_receipts_offset + static_cast<std::size_t>(sequence - 1) * progress_receipt_bytes,
                            progress_receipt_bytes);
    }

    void install(const ProgressImage& image)
    {
      if (image._receiptSequence != 0)
      {
        const auto receipt = encode_receipt(image._receipt);
        auto destination = receipt_bytes(image._receiptSequence);
        std::copy_n(receipt.begin(), 8, destination.begin());
        checkpoint(ProgressCheckpoint::PartialReceipt);
        std::copy(receipt.begin() + 8, receipt.end(), destination.begin() + 8);
      }
      checkpoint(ProgressCheckpoint::ReceiptInstalled);
      const auto source = encode_source(image._source);
      auto destination = _bytes.subspan(progress_state_offset, progress_state_bytes);
      // Include issuance/pin/acknowledgment words, not only incarnation: the
      // checkpoint must expose genuinely partial ordinary progress updates.
      std::copy_n(source.begin(), 40, destination.begin());
      checkpoint(ProgressCheckpoint::PartialSource);
      std::copy(source.begin() + 40, source.end(), destination.begin() + 40);
      checkpoint(ProgressCheckpoint::SourceInstalled);
      checkpoint(ProgressCheckpoint::BeforeApplied);
      _applied->publish_control(image._generation);
      _current = {image._generation, image._source};
      checkpoint(ProgressCheckpoint::AppliedPublished);
    }

    void settle()
    {
      const auto selected = _selector->load_acquire();
      const auto applied = _applied->load_acquire();
      require(applied <= selected && (selected == applied || selected - applied == 1),
              ErrorCode::IoError, "Progress applied generation differs from its selector");
      if (selected == 0)
      {
        require(all_zero(_bytes.subspan(progress_state_offset, progress_state_bytes)),
                ErrorCode::IoError, "Unselected progress contains working state");
        _current = {};
        return;
      }
      const auto image = decode_image(_bytes.subspan(progress_image_offset + (selected & 1) * progress_image_bytes,
                                                      progress_image_bytes), _options, selected);
      if (applied != selected)
      {
        // Selector publication committed this complete absolute after-image.
        // Repeating installation is safe even if the predecessor stopped
        // midway through a receipt or working-state store.
        install(image);
      }
      else
      {
        require(decode_source(_bytes.subspan(progress_state_offset, progress_state_bytes), _options._publicationCapacity) == image._source,
                ErrorCode::IoError, "Installed Source differs from selected progress");
        if (image._receiptSequence != 0)
        {
          require(decode_receipt(receipt_bytes(image._receiptSequence)) == image._receipt,
                  ErrorCode::IoError, "Installed receipt differs from selected progress");
        }
        _current = {selected, image._source};
      }
    }
  };

  ProgressStore::ProgressStore() noexcept = default;
  ProgressStore::~ProgressStore() = default;
  ProgressStore::ProgressStore(ProgressStore&&) noexcept = default;
  ProgressStore& ProgressStore::operator=(ProgressStore&&) noexcept = default;
  ProgressStore::ProgressStore(std::unique_ptr<State> state) noexcept : _state(std::move(state))
  {
  }

  OperationResult ProgressStore::create(const ProgressOptions& options)
  {
    return protect<std::monostate>([&]
    {
      const auto size = checked_size(options);
      auto file = value(mapped::NativeFile::create_exclusive(options._path, size));
      value(file.reserve(size));
      const auto identity = value(file.identity());
      require(identity != options._connectionIdentity, ErrorCode::ConfigurationMismatch, "Progress file aliases its connection");
      auto mapping = value(file.map_read_write(0, size));
      auto bytes = value(mapping.writable_bytes());
      std::fill(bytes.begin(), bytes.end(), std::byte{});
      const auto header = header_bytes(options, identity);
      std::copy(header.begin(), header.end(), bytes.begin());
      value(mapped::AtomicWord::bind(bytes, progress_selector_offset)).publish_control(0);
      value(mapped::AtomicWord::bind(bytes, progress_applied_offset)).publish_control(0);
      return std::monostate{};
    });
  }

  Result<ProgressStore> ProgressStore::open(const ProgressOptions& options, ProgressObserver* observer)
  {
    return protect<ProgressStore>([&]
    {
      const auto size = checked_size(options);
      auto state = std::make_unique<State>();
      state->_options = options;
      state->_claim = value(mapped::NativeClaim::acquire_existing(options._path));
      state->_file = value(mapped::NativeFile::open_existing(options._path, mapped::FileAccess::ReadWrite, size));
      const auto identity = value(state->_file.identity());
      require(identity != options._connectionIdentity && value(state->_file.size()) == size,
              ErrorCode::ConfigurationMismatch, "Progress reservation or resource binding differs");
      HeaderBytes actual{};
      value(state->_file.read_exact(0, actual));
      require(std::equal(progress_magic.begin(), progress_magic.end(), actual.begin(), [](char expected, std::byte stored)
              {
                return static_cast<unsigned char>(expected) == std::to_integer<unsigned char>(stored);
              }), ErrorCode::IoError, "Invalid acknowledged progress magic");
      require(read_unsigned_le<std::uint16_t>(actual, 8) == progress_format_major &&
                  read_unsigned_le<std::uint16_t>(actual, 10) == progress_format_minor &&
                  read_unsigned_le<std::uint32_t>(actual, 96) == options._liveAbiProfile &&
                  read_unsigned_le<std::uint16_t>(actual, 100) == progress_synchronization &&
                  read_unsigned_le<std::uint16_t>(actual, 102) == 8 && read_unsigned_le<std::uint64_t>(actual, 104) == 1,
              ErrorCode::UnsupportedPolicy, "Unsupported acknowledged progress format or synchronization");
      require(actual == header_bytes(options, identity), ErrorCode::ConfigurationMismatch, "Progress differs from its sealed connection context");
      state->_mapping = value(state->_file.map_read_write(0, size));
      state->_bytes = value(state->_mapping.writable_bytes());
      state->_selector.emplace(value(mapped::AtomicWord::bind(state->_bytes, progress_selector_offset)));
      state->_applied.emplace(value(mapped::AtomicWord::bind(state->_bytes, progress_applied_offset)));
      state->_observer = observer;
      state->settle();
      return ProgressStore(std::move(state));
    });
  }

  Result<ProgressSnapshot> ProgressStore::snapshot() const
  {
    return protect<ProgressSnapshot>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Progress owner is empty or moved-from");
      _state->check();
      return _state->_current;
    });
  }

  Result<ProgressReceipt> ProgressStore::receipt(std::uint64_t sequence) const
  {
    return protect<ProgressReceipt>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Progress owner is empty or moved-from");
      _state->check();
      const auto receipt = decode_receipt(_state->receipt_bytes(sequence));
      const auto& source = _state->_current._source;
      require(receipt._consumerIncarnation <= source._actorIncarnation &&
                  (sequence > source._acknowledged || receipt._state == ProgressReceiptState::Acknowledged) &&
                  (sequence <= source._acknowledged || receipt._state != ProgressReceiptState::Acknowledged) &&
                  (receipt._state != ProgressReceiptState::Issued || sequence == source._pinSequence),
              ErrorCode::IoError, "Receipt differs from committed Source progress");
      if (receipt._state == ProgressReceiptState::Issued)
      {
        require(receipt._consumerIncarnation == source._actorIncarnation && receipt._issuance == source._pinIssuance &&
                    receipt._dequeuedAt == source._dequeuedAt,
                ErrorCode::IoError, "Receipt differs from the committed pin");
      }
      return receipt;
    });
  }

  OperationResult ProgressStore::commit(const ProgressImage& image)
  {
    bool committed{};
    bool entered{};
    try
    {
      require(bool(_state), ErrorCode::InvalidState, "Progress owner is empty or moved-from");
      _state->check();
      require(_state->_current._generation != std::numeric_limits<std::uint64_t>::max(),
              ErrorCode::CapacityExceeded, "Progress generation is exhausted");
      validate_image(image, _state->_options, ErrorCode::InvalidArgument);
      validate_transition(_state->_current, image);
      const auto encoded = encode_image(image, _state->_options);
      _state->_busy = true;
      entered = true;
      auto destination = _state->_bytes.subspan(progress_image_offset + (image._generation & 1) * progress_image_bytes,
                                                progress_image_bytes);
      std::copy_n(encoded.begin(), 64, destination.begin());
      _state->checkpoint(ProgressCheckpoint::PartialImage);
      std::copy(encoded.begin() + 64, encoded.end(), destination.begin() + 64);
      _state->checkpoint(ProgressCheckpoint::ImageWritten);
      _state->checkpoint(ProgressCheckpoint::BeforeSelector);
      // This primitive orders complete image materialization, dependent row
      // stores, and later reuse of the previous selected slot in both directions.
      _state->_selector->publish_control(image._generation);
      committed = true;
      _state->checkpoint(ProgressCheckpoint::SelectorPublished);
      _state->install(image);
      _state->_busy = false;
      return std::monostate{};
    }
    catch (const CodecFailure& failure)
    {
      if (entered)
      {
        _state->_busy = false;
        _state->_failed = _state->_failed || committed;
      }
      return committed ? Error{ErrorCode::OutcomeUnknown, "Progress committed before its result; settle the Source", "recorded_progress"}
                       : failure._error;
    }
    catch (...)
    {
      if (entered)
      {
        _state->_busy = false;
        _state->_failed = _state->_failed || committed;
      }
      return Error{committed ? ErrorCode::OutcomeUnknown : ErrorCode::IoError,
                   "Progress did not return normally; settle the Source", "recorded_progress"};
    }
  }

  OperationResult ProgressStore::recover()
  {
    return protect<std::monostate>([&]
    {
      require(bool(_state), ErrorCode::InvalidState, "Progress owner is empty or moved-from");
      _state->check(true);
      _state->_busy = true;
      try
      {
        _state->settle();
        _state->_failed = false;
        _state->_busy = false;
      }
      catch (...)
      {
        _state->_busy = false;
        _state->_failed = true;
        throw;
      }
      return std::monostate{};
    });
  }
} // namespace angaria::detail::recorded
