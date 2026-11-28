#pragma once

// Private single-input Source progress. Message acceptance stays in the
// producer-owned log; this file stores only consumer-owned processing state.
#include "angaria/identities.h"
#include "angaria/results.h"
#include "angaria/descriptors.h"
#include "native_file.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace angaria::detail::recorded
{
  inline constexpr std::size_t progress_header_bytes = 256;
  inline constexpr std::size_t progress_selector_offset = 256;
  inline constexpr std::size_t progress_applied_offset = 512;
  inline constexpr std::size_t progress_state_offset = 1024;
  inline constexpr std::size_t progress_state_bytes = 80;
  inline constexpr std::size_t progress_image_offset = 4096;
  inline constexpr std::size_t progress_image_bytes = 256;
  inline constexpr std::size_t progress_receipts_offset = 8192;
  inline constexpr std::size_t progress_receipt_bytes = 64;

  struct ProgressOptions
  {
    std::filesystem::path _path;
    SessionId _session{};
    TaskId _consumer{};
    SourceQueueSetId _source{};
    QueueId _queue{};
    std::uint64_t _connectionGeneration{};
    mapped::FileIdentity _connectionIdentity{};
    std::uint64_t _publicationCapacity{};
    std::uint32_t _liveAbiProfile{};
  };

  enum class ProgressReceiptState : std::uint64_t
  {
    Pending,
    Issued,
    Acknowledged
  };

  struct ProgressState
  {
    std::uint64_t _actorIncarnation{};
    std::uint64_t _issuance{};
    std::uint64_t _pinSequence{};
    std::uint64_t _pinIssuance{};
    std::uint64_t _acknowledged{};
    std::uint64_t _credit{};
    std::optional<UtcTimestamp> _dequeuedAt;
    std::optional<UtcTimestamp> _lastAcknowledgedAt;

    bool operator==(const ProgressState&) const = default;
  };

  struct ProgressReceipt
  {
    ProgressReceiptState _state{ProgressReceiptState::Pending};
    std::uint64_t _consumerIncarnation{};
    std::uint64_t _issuance{};
    std::optional<UtcTimestamp> _dequeuedAt;
    std::optional<UtcTimestamp> _acknowledgedAt;

    bool operator==(const ProgressReceipt&) const = default;
  };

  struct ProgressImage
  {
    std::uint64_t _generation{};
    ProgressState _source;
    // Zero changes no receipt; otherwise this is its accepted Queue sequence.
    std::uint64_t _receiptSequence{};
    ProgressReceipt _receipt;
  };

  struct ProgressSnapshot
  {
    std::uint64_t _generation{};
    ProgressState _source;
  };

  enum class ProgressCheckpoint
  {
    PartialImage,
    ImageWritten,
    BeforeSelector,
    SelectorPublished,
    PartialReceipt,
    ReceiptInstalled,
    PartialSource,
    SourceInstalled,
    BeforeApplied,
    AppliedPublished
  };

  class ProgressObserver
  {
  public:
    virtual ~ProgressObserver() = default;
    virtual OperationResult checkpoint(ProgressCheckpoint point) = 0;
  };

  class ProgressStore
  {
  public:
    ProgressStore() noexcept;
    ~ProgressStore();
    ProgressStore(ProgressStore&&) noexcept;
    ProgressStore& operator=(ProgressStore&&) noexcept;
    ProgressStore(const ProgressStore&) = delete;
    ProgressStore& operator=(const ProgressStore&) = delete;

    static OperationResult create(const ProgressOptions& options);
    // Caller already holds the matching connection's consumer claim. This
    // additional actual-inode claim prevents sharing a progress owner.
    static Result<ProgressStore> open(const ProgressOptions& options, ProgressObserver* observer = nullptr);
    Result<ProgressSnapshot> snapshot() const;
    // Receipt presence does not independently establish message acceptance.
    Result<ProgressReceipt> receipt(std::uint64_t sequence) const;
    OperationResult commit(const ProgressImage& image);
    // Settles the selected operation, without choosing or promoting an image.
    // Hint publication belongs to the endpoint, including when already applied.
    OperationResult recover();

  private:
    struct State;
    explicit ProgressStore(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> _state;
  };
} // namespace angaria::detail::recorded
