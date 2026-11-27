#pragma once

#include "angaria/detail/mapped_format.h"
#include "angaria/results.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace angaria::detail::mapped
{
  inline constexpr std::size_t journal_offset = 672;
  inline constexpr std::size_t journal_bytes = 2048;
  inline constexpr std::size_t journal_header_bytes = 256;
  inline constexpr std::size_t inline_command_bytes = 1792;

  enum class JournalControl : std::uint64_t
  {
    Idle = 0,
    Prepared = 1,
    Committed = 2,
    Applied = 3
  };

  enum class JournalKind : std::uint16_t
  {
    Append = 1,
    FixedState = 2
  };

  enum class TransactionSubtype : std::uint16_t
  {
    Registry = 1,
    Publication = 2,
    Retry = 3
  };

  enum class RedoOpcode : std::uint16_t
  {
    WriteRange = 1,
    ResetBudgets = 2,
    LoseRange = 3,
    ReleaseClaim = 4
  };

  struct RedoCommand
  {
    RedoOpcode _opcode{RedoOpcode::WriteRange};
    std::uint64_t _target{};
    std::vector<std::byte> _payload;

    bool operator==(const RedoCommand&) const = default;
  };

  struct JournalHeader
  {
    JournalControl _control{JournalControl::Idle};
    std::uint64_t _generation{};
    JournalKind _kind{JournalKind::FixedState};
    std::uint64_t _baseTail{};
    std::uint64_t _newTail{};
    std::uint64_t _baseAccounted{};
    std::uint64_t _newAccounted{};
    std::uint64_t _manifest{};
    std::uint32_t _inlineBytes{};
    TaskId _actor{};
    std::uint64_t _actorIncarnation{};
    std::uint64_t _fileLogicalStart{};
    std::uint64_t _originalFileBytes{};
    std::uint64_t _newFileBytes{};
    std::uint64_t _newExtentBytes{};
    std::uint32_t _recordCount{};
    std::uint64_t _baseRevision{};
    std::uint64_t _newRevision{};
    std::uint64_t _firstRecord{};

    bool operator==(const JournalHeader&) const = default;
  };

  struct RecordEnvelope
  {
    std::uint16_t _kind{};
    std::uint16_t _encoding{1};
    std::uint64_t _totalBytes{};
    std::uint64_t _immutableBytes{};
    std::uint64_t _generation{};
    std::uint64_t _previous{};
    std::uint64_t _registryRevision{};
    std::uint64_t _readiness{};

    bool operator==(const RecordEnvelope&) const = default;
  };

  struct Manifest
  {
    TransactionSubtype _subtype{TransactionSubtype::Registry};
    TaskId _actor{};
    std::uint64_t _actorIncarnation{};
    std::uint64_t _baseRevision{};
    std::uint64_t _newRevision{};
    std::vector<std::uint64_t> _records;
    std::vector<RedoCommand> _commands;

    bool operator==(const Manifest&) const = default;
  };

  // Validated metadata over local immutable bytes. References are little-endian
  // eight-byte entries, not host-aligned integers. The view borrows its input:
  // consume it before that input is destroyed or reused for another read.
  struct ManifestView
  {
    TransactionSubtype _subtype{TransactionSubtype::Registry};
    TaskId _actor{};
    std::uint64_t _actorIncarnation{};
    std::uint64_t _baseRevision{};
    std::uint64_t _newRevision{};
    std::span<const std::byte> _recordReferences;
  };

  using JournalHeaderBytes = std::array<std::byte, journal_header_bytes>;
  using RecordEnvelopeBytes = std::array<std::byte, record_envelope_bytes>;

  // These codecs operate on local byte copies, never directly on live atomic
  // words. An Idle journal body is opaque and must not be decoded for recovery.
  Result<JournalHeaderBytes> encode_journal_header(const JournalHeader& header);
  Result<JournalHeader> decode_journal_header(std::span<const std::byte> bytes);
  Result<std::vector<std::byte>> encode_commands(std::span<const RedoCommand> commands);
  Result<std::vector<RedoCommand>> decode_commands(std::span<const std::byte> bytes);
  OperationResult validate_commands(std::span<const std::byte> bytes);
  Result<RecordEnvelopeBytes> encode_record_envelope(const RecordEnvelope& envelope);
  Result<RecordEnvelope> decode_record_envelope(std::span<const std::byte> bytes);

  // Bodies exclude the record envelope; slices still contain absolute logical
  // offsets. References precede the command stream in the canonical encoding.
  Result<std::vector<std::byte>> encode_manifest_body(const Manifest& manifest,
                                                      std::uint64_t recordOffset);
  Result<Manifest> decode_manifest_body(std::span<const std::byte> bytes,
                                         std::uint64_t recordOffset);
  // Checks the same complete body/command stream without materializing owning
  // vectors. No command payload or reference view escapes its input lifetime.
  Result<ManifestView> validate_manifest_body(std::span<const std::byte> bytes,
                                             std::uint64_t recordOffset);
} // namespace angaria::detail::mapped
