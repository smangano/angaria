#include "journal_format.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace angaria::detail::mapped
{
  namespace
  {
    using angaria::detail::all_zero;
    using angaria::detail::read_unsigned_le;
    using angaria::detail::write_unsigned_le;

    Error failure(ErrorCode code, const char* path, std::string message)
    {
      return Error{code, std::move(message), path};
    }

    bool actor_valid(TaskId actor, std::uint64_t incarnation)
    {
      return (actor == TaskId::Invalid) == (incarnation == 0);
    }

    bool reference_valid(std::uint64_t offset)
    {
      return offset >= superblock_bytes && offset <= max_log_bytes && offset % record_alignment == 0;
    }

    std::optional<Error> validate_header(const JournalHeader& header, ErrorCode code)
    {
      const auto invalid = [code](const char* message)
      {
        return failure(code, "journal.header", message);
      };
      if (static_cast<std::uint64_t>(header._control) > 3 || header._generation == 0 ||
          !actor_valid(header._actor, header._actorIncarnation))
      {
        return invalid("Invalid journal control, generation, or actor incarnation");
      }
      if (!reference_valid(header._baseTail) || !reference_valid(header._newTail) ||
          header._newTail < header._baseTail || header._baseAccounted < superblock_bytes ||
          header._newAccounted < header._baseAccounted || header._newAccounted > max_log_bytes ||
          header._newFileBytes < superblock_bytes || header._newFileBytes < header._originalFileBytes ||
          header._newFileBytes > max_log_bytes || header._newRevision < header._baseRevision ||
          header._inlineBytes > inline_command_bytes || header._inlineBytes % 8 != 0)
      {
        return invalid("Journal allocation, accounting, revision, or inline length is invalid");
      }
      if (header._fileLogicalStart == 0)
      {
        if (header._newExtentBytes != 0 || header._originalFileBytes < superblock_bytes)
        {
          return invalid("The session file cannot describe a new extent allocation");
        }
      }
      else if (!reference_valid(header._fileLogicalStart) ||
               !contains_range(max_log_bytes, header._fileLogicalStart, header._newFileBytes) ||
               (header._newExtentBytes == 0 ? header._originalFileBytes < superblock_bytes :
                 header._originalFileBytes != 0 || header._newExtentBytes != header._newFileBytes))
      {
        return invalid("Invalid affected extent range");
      }
      if (header._kind == JournalKind::FixedState)
      {
        if (header._baseTail != header._newTail || header._baseAccounted != header._newAccounted ||
            header._manifest != 0 || header._recordCount != 0 || header._firstRecord != 0 ||
            header._fileLogicalStart != 0 || header._newExtentBytes != 0 ||
            header._originalFileBytes != header._newFileBytes || header._baseRevision != header._newRevision)
        {
          return invalid("FixedState cannot allocate records, change roots, or resize files");
        }
      }
      else if (header._kind == JournalKind::Append)
      {
        if (header._newTail == header._baseTail || header._recordCount == 0 || header._inlineBytes != 0 ||
            !reference_valid(header._firstRecord) || header._firstRecord < header._baseTail ||
            !reference_valid(header._manifest) || header._manifest < header._firstRecord ||
            !contains_range(header._newTail, header._manifest, record_envelope_bytes))
        {
          return invalid("Append requires a bounded new batch and its final manifest");
        }
      }
      else
      {
        return invalid("Unknown journal operation kind");
      }
      return std::nullopt;
    }

    std::optional<Error> validate_command(RedoOpcode opcode, std::uint64_t target,
                                          std::span<const std::byte> payload, ErrorCode code)
    {
      const auto invalid = [code](const char* message)
      {
        return failure(code, "journal.commands", message);
      };
      if (target == 0 || target % 8 != 0 || !contains_range(max_log_bytes, target, 8) ||
          payload.size() > std::numeric_limits<std::uint32_t>::max())
      {
        return invalid("Redo target or payload length exceeds the format bounds");
      }
      switch (opcode)
      {
      case RedoOpcode::WriteRange:
        if (payload.empty() || payload.size() % 8 != 0 ||
            !contains_range(max_log_bytes, target, payload.size()))
        {
          return invalid("WriteRange requires a positive aligned byte range");
        }
        break;
      case RedoOpcode::ResetBudgets:
        if (payload.size() != 24 || read_unsigned_le<std::uint16_t>(payload, 0) == 0 ||
            !all_zero(payload.subspan(2, 6)) || !reference_valid(read_unsigned_le<std::uint64_t>(payload, 8)) ||
            read_unsigned_le<std::uint64_t>(payload, 16) == 0)
        {
          return invalid("Invalid ResetBudgets payload");
        }
        break;
      case RedoOpcode::LoseRange:
        if (payload.size() != 32 || read_unsigned_le<std::uint64_t>(payload, 0) == 0 ||
            read_unsigned_le<std::uint64_t>(payload, 8) < read_unsigned_le<std::uint64_t>(payload, 0) ||
            read_unsigned_le<std::uint16_t>(payload, 24) == 0 || !all_zero(payload.subspan(26, 6)))
        {
          return invalid("Invalid LoseRange payload");
        }
        break;
      case RedoOpcode::ReleaseClaim:
        if (payload.size() != 16 || read_unsigned_le<std::uint64_t>(payload, 0) == 0)
        {
          return invalid("Invalid ReleaseClaim payload");
        }
        break;
      default:
        return invalid("Unknown redo opcode");
      }
      return std::nullopt;
    }

    template <typename Consume>
    OperationResult walk_commands(std::span<const std::byte> bytes, Consume&& consume)
    {
      std::size_t offset{};
      while (offset < bytes.size())
      {
        if (bytes.size() - offset < 16)
        {
          return failure(ErrorCode::IoError, "journal.commands", "Truncated redo command header");
        }
        const auto opcode = static_cast<RedoOpcode>(read_unsigned_le<std::uint16_t>(bytes, offset));
        const auto length = read_unsigned_le<std::uint32_t>(bytes, offset + 4);
        const auto target = read_unsigned_le<std::uint64_t>(bytes, offset + 8);
        if (read_unsigned_le<std::uint16_t>(bytes, offset + 2) != 0 || length > bytes.size() - offset - 16)
        {
          return failure(ErrorCode::IoError, "journal.commands", "Invalid command flags or truncated payload");
        }
        const auto payload = bytes.subspan(offset + 16, length);
        if (const auto error = validate_command(opcode, target, payload, ErrorCode::IoError))
        {
          return *error;
        }
        consume(opcode, target, payload);
        offset += std::size_t{16} + length;
      }
      return std::monostate{};
    }

    std::optional<Error> validate_envelope(const RecordEnvelope& envelope, ErrorCode code)
    {
      if (envelope._kind == 0 || envelope._kind > 12 || envelope._encoding != 1 ||
          envelope._totalBytes < record_envelope_bytes || envelope._totalBytes > max_log_bytes ||
          envelope._totalBytes % 8 != 0 || envelope._immutableBytes % 8 != 0 ||
          envelope._immutableBytes > envelope._totalBytes - record_envelope_bytes ||
          envelope._generation == 0 || envelope._readiness > 2 ||
          (envelope._previous != 0 && !reference_valid(envelope._previous)))
      {
        return failure(code, "journal.record", "Invalid record kind, encoding, geometry, generation, or readiness");
      }
      return std::nullopt;
    }

    template <typename Header>
    std::optional<Error> validate_manifest_header(const Header& manifest, std::uint64_t recordOffset,
                                                  ErrorCode code)
    {
      if (!reference_valid(recordOffset) ||
          !contains_range(max_log_bytes, recordOffset, record_envelope_bytes + 64) ||
          !actor_valid(manifest._actor, manifest._actorIncarnation))
      {
        return failure(code, "journal.manifest", "Invalid manifest position or actor");
      }
      switch (manifest._subtype)
      {
      case TransactionSubtype::Registry:
        if (manifest._baseRevision == std::numeric_limits<std::uint64_t>::max() ||
            manifest._newRevision != manifest._baseRevision + 1)
        {
          return failure(code, "journal.manifest", "Registry manifest must advance its revision once");
        }
        break;
      case TransactionSubtype::Publication:
      case TransactionSubtype::Retry:
        if (manifest._newRevision != manifest._baseRevision)
        {
          return failure(code, "journal.manifest", "Delivery manifests preserve the Registry revision");
        }
        break;
      default:
        return failure(code, "journal.manifest", "Unknown manifest subtype");
      }
      return std::nullopt;
    }

    std::optional<Error> validate_manifest_reference(std::uint64_t reference, std::uint64_t recordOffset,
                                                     std::uint64_t previous, ErrorCode code)
    {
      if (!reference_valid(reference) || reference >= recordOffset || reference <= previous)
      {
        return failure(code, "journal.manifest", "Manifest record references must name an ordered earlier batch");
      }
      return std::nullopt;
    }

    std::optional<Error> validate_manifest(const Manifest& manifest, std::uint64_t recordOffset,
                                           ErrorCode code)
    {
      if (const auto error = validate_manifest_header(manifest, recordOffset, code))
      {
        return *error;
      }
      std::uint64_t previous{};
      for (const auto reference : manifest._records)
      {
        if (const auto error = validate_manifest_reference(reference, recordOffset, previous, code))
        {
          return *error;
        }
        previous = reference;
      }
      return std::nullopt;
    }

    struct ManifestFrame
    {
      ManifestView _manifest;
      std::span<const std::byte> _commands;
    };

    // Framing precedes semantic checks in both paths, preserving the owning
    // decoder's ordering when a body contains more than one corruption.
    Result<ManifestFrame> parse_manifest_frame(std::span<const std::byte> bytes,
                                               std::uint64_t recordOffset)
    {
      if (bytes.size() < 64 || bytes.size() % 8 != 0 || !reference_valid(recordOffset) ||
          !contains_range(max_log_bytes, recordOffset, record_envelope_bytes) ||
          !contains_range(max_log_bytes, recordOffset + record_envelope_bytes, bytes.size()))
      {
        return failure(ErrorCode::IoError, "journal.manifest", "Invalid manifest body range");
      }
      if (!all_zero(bytes.subspan(4, 4)))
      {
        return failure(ErrorCode::IoError, "journal.manifest", "Manifest flags are reserved");
      }
      const auto referencesOffset = read_unsigned_le<std::uint64_t>(bytes, 32);
      const auto count = read_unsigned_le<std::uint64_t>(bytes, 40);
      const auto commandsOffset = read_unsigned_le<std::uint64_t>(bytes, 48);
      const auto commandBytes = read_unsigned_le<std::uint64_t>(bytes, 56);
      const auto referenceBytes = checked_multiply(count, 8);
      const auto bodyStart = recordOffset + record_envelope_bytes;
      if (!referenceBytes || *referenceBytes > bytes.size() - 64 ||
          (count == 0 ? referencesOffset != 0 : referencesOffset != bodyStart + 64))
      {
        return failure(ErrorCode::IoError, "journal.manifest", "Invalid manifest reference slice");
      }
      const auto commandBegin = 64 + static_cast<std::size_t>(*referenceBytes);
      if (commandBytes > bytes.size() - commandBegin || commandBytes % 8 != 0 ||
          (commandBytes == 0 ? commandsOffset != 0 : commandsOffset != bodyStart + commandBegin))
      {
        return failure(ErrorCode::IoError, "journal.manifest", "Invalid manifest command slice");
      }
      const auto used = commandBegin + static_cast<std::size_t>(commandBytes);
      if (bytes.size() - used >= section_alignment || !all_zero(bytes.subspan(used)))
      {
        return failure(ErrorCode::IoError, "journal.manifest", "Invalid manifest allocation padding");
      }
      ManifestFrame frame;
      auto& manifest = frame._manifest;
      manifest._subtype = static_cast<TransactionSubtype>(read_unsigned_le<std::uint16_t>(bytes, 0));
      manifest._actor = static_cast<TaskId>(read_unsigned_le<std::uint16_t>(bytes, 2));
      manifest._actorIncarnation = read_unsigned_le<std::uint64_t>(bytes, 8);
      manifest._baseRevision = read_unsigned_le<std::uint64_t>(bytes, 16);
      manifest._newRevision = read_unsigned_le<std::uint64_t>(bytes, 24);
      manifest._recordReferences = bytes.subspan(64, static_cast<std::size_t>(*referenceBytes));
      frame._commands = bytes.subspan(commandBegin, static_cast<std::size_t>(commandBytes));
      return frame;
    }
  } // namespace

  Result<JournalHeaderBytes> encode_journal_header(const JournalHeader& header)
  {
    if (const auto error = validate_header(header, ErrorCode::InvalidArgument))
    {
      return *error;
    }
    JournalHeaderBytes bytes{};
    write_unsigned_le<std::uint64_t>(bytes, 0, static_cast<std::uint64_t>(header._control));
    write_unsigned_le<std::uint64_t>(bytes, 8, header._generation);
    write_unsigned_le<std::uint16_t>(bytes, 16, static_cast<std::uint16_t>(header._kind));
    write_unsigned_le<std::uint16_t>(bytes, 18, 1);
    write_unsigned_le<std::uint64_t>(bytes, 24, header._baseTail);
    write_unsigned_le<std::uint64_t>(bytes, 32, header._newTail);
    write_unsigned_le<std::uint64_t>(bytes, 40, header._baseAccounted);
    write_unsigned_le<std::uint64_t>(bytes, 48, header._newAccounted);
    write_unsigned_le<std::uint64_t>(bytes, 56, header._manifest);
    write_unsigned_le<std::uint32_t>(bytes, 64, header._inlineBytes);
    write_unsigned_le<std::uint16_t>(bytes, 72, static_cast<std::uint16_t>(header._actor));
    write_unsigned_le<std::uint64_t>(bytes, 80, header._actorIncarnation);
    write_unsigned_le<std::uint64_t>(bytes, 96, header._fileLogicalStart);
    write_unsigned_le<std::uint64_t>(bytes, 104, header._originalFileBytes);
    write_unsigned_le<std::uint64_t>(bytes, 112, header._newFileBytes);
    write_unsigned_le<std::uint64_t>(bytes, 120, header._newExtentBytes);
    write_unsigned_le<std::uint32_t>(bytes, 128, header._recordCount);
    write_unsigned_le<std::uint64_t>(bytes, 136, header._baseRevision);
    write_unsigned_le<std::uint64_t>(bytes, 144, header._newRevision);
    write_unsigned_le<std::uint64_t>(bytes, 152, header._firstRecord);
    return bytes;
  }

  Result<JournalHeader> decode_journal_header(std::span<const std::byte> bytes)
  {
    if (bytes.size() != journal_header_bytes)
    {
      return failure(ErrorCode::IoError, "journal.header", "Journal header must contain exactly 256 bytes");
    }
    if (read_unsigned_le<std::uint16_t>(bytes, 18) != 1 || !all_zero(bytes.subspan(20, 4)) ||
        !all_zero(bytes.subspan(68, 4)) || !all_zero(bytes.subspan(74, 6)) || !all_zero(bytes.subspan(88, 8)) ||
        !all_zero(bytes.subspan(132, 4)) || !all_zero(bytes.subspan(160, 96)))
    {
      return failure(ErrorCode::IoError, "journal.header", "Unsupported command encoding or nonzero reserved header bytes");
    }
    JournalHeader header;
    header._control = static_cast<JournalControl>(read_unsigned_le<std::uint64_t>(bytes, 0));
    header._generation = read_unsigned_le<std::uint64_t>(bytes, 8);
    header._kind = static_cast<JournalKind>(read_unsigned_le<std::uint16_t>(bytes, 16));
    header._baseTail = read_unsigned_le<std::uint64_t>(bytes, 24);
    header._newTail = read_unsigned_le<std::uint64_t>(bytes, 32);
    header._baseAccounted = read_unsigned_le<std::uint64_t>(bytes, 40);
    header._newAccounted = read_unsigned_le<std::uint64_t>(bytes, 48);
    header._manifest = read_unsigned_le<std::uint64_t>(bytes, 56);
    header._inlineBytes = read_unsigned_le<std::uint32_t>(bytes, 64);
    header._actor = static_cast<TaskId>(read_unsigned_le<std::uint16_t>(bytes, 72));
    header._actorIncarnation = read_unsigned_le<std::uint64_t>(bytes, 80);
    header._fileLogicalStart = read_unsigned_le<std::uint64_t>(bytes, 96);
    header._originalFileBytes = read_unsigned_le<std::uint64_t>(bytes, 104);
    header._newFileBytes = read_unsigned_le<std::uint64_t>(bytes, 112);
    header._newExtentBytes = read_unsigned_le<std::uint64_t>(bytes, 120);
    header._recordCount = read_unsigned_le<std::uint32_t>(bytes, 128);
    header._baseRevision = read_unsigned_le<std::uint64_t>(bytes, 136);
    header._newRevision = read_unsigned_le<std::uint64_t>(bytes, 144);
    header._firstRecord = read_unsigned_le<std::uint64_t>(bytes, 152);
    if (const auto error = validate_header(header, ErrorCode::IoError))
    {
      return *error;
    }
    return header;
  }

  Result<std::vector<std::byte>> encode_commands(std::span<const RedoCommand> commands)
  {
    std::uint64_t total{};
    for (const auto& command : commands)
    {
      if (const auto error = validate_command(command._opcode, command._target, command._payload,
                                              ErrorCode::InvalidArgument))
      {
        return *error;
      }
      const auto next = checked_add(total, std::uint64_t{16} + command._payload.size());
      if (!next || *next > max_log_bytes || !fits_size_t(*next))
      {
        return failure(ErrorCode::InvalidArgument, "journal.commands", "Command stream exceeds addressable bounds");
      }
      total = *next;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(total));
    std::size_t offset{};
    for (const auto& command : commands)
    {
      write_unsigned_le<std::uint16_t>(bytes, offset, static_cast<std::uint16_t>(command._opcode));
      write_unsigned_le<std::uint32_t>(bytes, offset + 4, static_cast<std::uint32_t>(command._payload.size()));
      write_unsigned_le<std::uint64_t>(bytes, offset + 8, command._target);
      std::copy(command._payload.begin(), command._payload.end(), bytes.begin() + offset + 16);
      offset += 16 + command._payload.size();
    }
    return bytes;
  }

  Result<std::vector<RedoCommand>> decode_commands(std::span<const std::byte> bytes)
  {
    std::vector<RedoCommand> commands;
    auto result = walk_commands(bytes, [&](RedoOpcode opcode, std::uint64_t target,
                                           std::span<const std::byte> payload)
    {
      commands.push_back(RedoCommand{opcode, target, {payload.begin(), payload.end()}});
    });
    if (const auto error = std::get_if<Error>(&result))
    {
      return *error;
    }
    return commands;
  }

  OperationResult validate_commands(std::span<const std::byte> bytes)
  {
    return walk_commands(bytes, [](RedoOpcode, std::uint64_t, std::span<const std::byte>)
    {
    });
  }

  Result<RecordEnvelopeBytes> encode_record_envelope(const RecordEnvelope& envelope)
  {
    if (const auto error = validate_envelope(envelope, ErrorCode::InvalidArgument))
    {
      return *error;
    }
    RecordEnvelopeBytes bytes{};
    write_unsigned_le<std::uint16_t>(bytes, 0, envelope._kind);
    write_unsigned_le<std::uint16_t>(bytes, 2, envelope._encoding);
    write_unsigned_le<std::uint32_t>(bytes, 4, record_envelope_bytes);
    write_unsigned_le<std::uint64_t>(bytes, 8, envelope._totalBytes);
    write_unsigned_le<std::uint64_t>(bytes, 16, envelope._immutableBytes);
    write_unsigned_le<std::uint64_t>(bytes, 24, envelope._generation);
    write_unsigned_le<std::uint64_t>(bytes, 32, envelope._previous);
    write_unsigned_le<std::uint64_t>(bytes, 40, envelope._registryRevision);
    write_unsigned_le<std::uint64_t>(bytes, 48, envelope._readiness);
    return bytes;
  }

  Result<RecordEnvelope> decode_record_envelope(std::span<const std::byte> bytes)
  {
    if (bytes.size() != record_envelope_bytes)
    {
      return failure(ErrorCode::IoError, "journal.record", "Record envelope must contain exactly 64 bytes");
    }
    if (read_unsigned_le<std::uint32_t>(bytes, 4) != record_envelope_bytes || !all_zero(bytes.subspan(56, 8)))
    {
      return failure(ErrorCode::IoError, "journal.record", "Invalid envelope size or reserved recovery word");
    }
    RecordEnvelope envelope;
    envelope._kind = read_unsigned_le<std::uint16_t>(bytes, 0);
    envelope._encoding = read_unsigned_le<std::uint16_t>(bytes, 2);
    envelope._totalBytes = read_unsigned_le<std::uint64_t>(bytes, 8);
    envelope._immutableBytes = read_unsigned_le<std::uint64_t>(bytes, 16);
    envelope._generation = read_unsigned_le<std::uint64_t>(bytes, 24);
    envelope._previous = read_unsigned_le<std::uint64_t>(bytes, 32);
    envelope._registryRevision = read_unsigned_le<std::uint64_t>(bytes, 40);
    envelope._readiness = read_unsigned_le<std::uint64_t>(bytes, 48);
    if (const auto error = validate_envelope(envelope, ErrorCode::IoError))
    {
      return *error;
    }
    return envelope;
  }

  Result<std::vector<std::byte>> encode_manifest_body(const Manifest& manifest, std::uint64_t recordOffset)
  {
    if (const auto error = validate_manifest(manifest, recordOffset, ErrorCode::InvalidArgument))
    {
      return *error;
    }
    auto encoded = encode_commands(manifest._commands);
    if (const auto error = std::get_if<Error>(&encoded))
    {
      return *error;
    }
    auto commands = std::move(std::get<std::vector<std::byte>>(encoded));
    const auto referenceBytes = checked_multiply(manifest._records.size(), sizeof(std::uint64_t));
    const auto bodyBytes = referenceBytes ? checked_add(std::uint64_t{64} + commands.size(), *referenceBytes) : std::nullopt;
    if (!bodyBytes || !fits_size_t(*bodyBytes) ||
        !contains_range(max_log_bytes, recordOffset + record_envelope_bytes, *bodyBytes))
    {
      return failure(ErrorCode::InvalidArgument, "journal.manifest", "Manifest body exceeds addressable bounds");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(*bodyBytes));
    write_unsigned_le<std::uint16_t>(bytes, 0, static_cast<std::uint16_t>(manifest._subtype));
    write_unsigned_le<std::uint16_t>(bytes, 2, static_cast<std::uint16_t>(manifest._actor));
    write_unsigned_le<std::uint64_t>(bytes, 8, manifest._actorIncarnation);
    write_unsigned_le<std::uint64_t>(bytes, 16, manifest._baseRevision);
    write_unsigned_le<std::uint64_t>(bytes, 24, manifest._newRevision);
    const auto referencesOffset = recordOffset + record_envelope_bytes + 64;
    if (!manifest._records.empty())
    {
      write_unsigned_le<std::uint64_t>(bytes, 32, referencesOffset);
      write_unsigned_le<std::uint64_t>(bytes, 40, manifest._records.size());
    }
    for (std::size_t index = 0; index < manifest._records.size(); ++index)
    {
      write_unsigned_le<std::uint64_t>(bytes, 64 + index * 8, manifest._records[index]);
    }
    if (!commands.empty())
    {
      write_unsigned_le<std::uint64_t>(bytes, 48, referencesOffset + *referenceBytes);
      write_unsigned_le<std::uint64_t>(bytes, 56, commands.size());
      std::copy(commands.begin(), commands.end(), bytes.begin() + 64 + *referenceBytes);
    }
    return bytes;
  }

  Result<Manifest> decode_manifest_body(std::span<const std::byte> bytes, std::uint64_t recordOffset)
  {
    auto parsed = parse_manifest_frame(bytes, recordOffset);
    if (const auto error = std::get_if<Error>(&parsed))
    {
      return *error;
    }
    const auto& frame = std::get<ManifestFrame>(parsed);
    const auto& view = frame._manifest;
    Manifest manifest;
    manifest._subtype = view._subtype;
    manifest._actor = view._actor;
    manifest._actorIncarnation = view._actorIncarnation;
    manifest._baseRevision = view._baseRevision;
    manifest._newRevision = view._newRevision;
    // Every count is bounded by its owning immutable span before allocation.
    const auto count = view._recordReferences.size() / sizeof(std::uint64_t);
    manifest._records.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
      manifest._records.push_back(read_unsigned_le<std::uint64_t>(view._recordReferences, index * 8));
    }
    if (const auto error = validate_manifest(manifest, recordOffset, ErrorCode::IoError))
    {
      return *error;
    }
    auto decoded = decode_commands(frame._commands);
    if (const auto error = std::get_if<Error>(&decoded))
    {
      return *error;
    }
    manifest._commands = std::move(std::get<std::vector<RedoCommand>>(decoded));
    return manifest;
  }

  Result<ManifestView> validate_manifest_body(std::span<const std::byte> bytes, std::uint64_t recordOffset)
  {
    auto parsed = parse_manifest_frame(bytes, recordOffset);
    if (const auto error = std::get_if<Error>(&parsed))
    {
      return *error;
    }
    const auto& frame = std::get<ManifestFrame>(parsed);
    const auto& manifest = frame._manifest;
    if (const auto error = validate_manifest_header(manifest, recordOffset, ErrorCode::IoError))
    {
      return *error;
    }
    std::uint64_t previous{};
    for (std::size_t offset = 0; offset < manifest._recordReferences.size(); offset += 8)
    {
      const auto reference = read_unsigned_le<std::uint64_t>(manifest._recordReferences, offset);
      if (const auto error = validate_manifest_reference(reference, recordOffset, previous, ErrorCode::IoError))
      {
        return *error;
      }
      previous = reference;
    }
    if (auto result = validate_commands(frame._commands); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    return manifest;
  }
} // namespace angaria::detail::mapped
