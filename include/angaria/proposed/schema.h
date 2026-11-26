#pragma once

// Review-only declarations for the proposed complete application API.
// These are process-local values, not persisted layouts or implemented methods.

#include "angaria/descriptors.h"
#include "angaria/identities.h"
#include "angaria/results.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angaria::proposed
{
  class Session;
  class Task;
  class LogArchive;
  class Registry;
  struct RegistrySnapshot;

  namespace detail
  {
    class PoolState;
    class PoolLeaseState;
    class EncodedPayloadState;
    class PayloadLeaseState;
    class SchemaCodecState;
  }

  enum class ScalarType
  {
    Boolean,
    Character,
    Byte,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64
  };

  enum class IntegerType
  {
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64
  };

  enum class DateShape
  {
    CalendarDate,
    YearMonth,
    Year,
    MonthDay
  };

  enum class ClockNotation
  {
    TwelveHour,
    TwentyFourHour
  };

  enum class ZoneRepresentation
  {
    Utc,
    RecordedOffset
  };

  enum class DurationUnit
  {
    Years,
    Months,
    Days,
    Hours,
    Minutes,
    Seconds,
    Milliseconds,
    Microseconds,
    Nanoseconds
  };

  struct ScalarField
  {
    ScalarType _type{ScalarType::Int64};
  };

  struct BitsField
  {
    std::uint16_t _bits{64}; // Exactly 8, 16, 32, 64, 128, or 256.
  };

  using EnumerationNumber = std::variant<std::int64_t, std::uint64_t>;

  struct Enumerator
  {
    std::string _name;
    EnumerationNumber _value;
  };

  struct EnumerationField
  {
    IntegerType _base{IntegerType::UInt32};
    std::vector<Enumerator> _values; // Closed: names and numbers are unique.
  };

  struct FixedAsciiField
  {
    std::uint32_t _bytes{};
  };

  struct FixedBytesField
  {
    std::uint32_t _bytes{};
  };

  struct DateField
  {
    DateShape _shape{DateShape::CalendarDate};
  };

  struct TimeField
  {
    ClockNotation _notation{ClockNotation::TwentyFourHour};
    ZoneRepresentation _zone{ZoneRepresentation::Utc};
  };

  struct DateTimeField
  {
    ZoneRepresentation _zone{ZoneRepresentation::Utc};
  };

  struct TimestampField
  {
    ZoneRepresentation _zone{ZoneRepresentation::Utc};
  };

  struct DurationField
  {
    DurationUnit _unit{DurationUnit::Nanoseconds};
  };

  struct SequenceField
  {
    // Application-supplied uint64 value, separate from transport identities.
  };

  struct GuidField
  {
  };

  struct SymbolField
  {
    std::uint32_t _maxCharacters{64}; // UTF-8 scalar count, never above 64.
  };

  struct StringField
  {
    std::uint64_t _maxUtf8Bytes{1048576};
  };

  struct ArrayField
  {
    FieldId _element{};
    std::uint32_t _count{};
  };

  struct TupleField
  {
    std::vector<FieldId> _fields;
  };

  struct GroupField
  {
    std::vector<FieldId> _fields;
    std::uint32_t _maxRows{};
  };

  // Referenced Field definitions form an acyclic graph. Compilation rejects
  // missing references, cycles, zero/unbounded compound limits, or excess depth.
  using FieldType = std::variant<ScalarField, BitsField, EnumerationField,
                                  FixedAsciiField, FixedBytesField, DateField,
                                  TimeField, DateTimeField, TimestampField,
                                  DurationField, SequenceField, GuidField,
                                  SymbolField, StringField, ArrayField,
                                  TupleField, GroupField>;

  struct BitsValue
  {
    std::uint16_t _bits{64};
    // Process-local unsigned magnitude, least significant byte first. Unused
    // high bytes are zero. This is not a promise about a future wire layout.
    std::array<std::byte, 32> _bytes{};

    static Result<BitsValue> from_hex(std::string_view text, std::uint16_t bits);
    Result<std::string> to_hex() const;
    Result<bool> test(std::uint16_t index) const;
    OperationResult set(std::uint16_t index, bool value = true);
    // Masks require equal widths. Shifts discard overflowing high/low bits;
    // a shift of at least the width gives zero, as with std::bitset.
    Result<BitsValue> bit_and(const BitsValue& other) const;
    Result<BitsValue> bit_or(const BitsValue& other) const;
    Result<BitsValue> bit_xor(const BitsValue& other) const;
    Result<BitsValue> bit_not() const;
    Result<BitsValue> shift_left(std::uint32_t count) const;
    Result<BitsValue> shift_right(std::uint32_t count) const;
  };

  struct EnumerationValue
  {
    EnumerationNumber _value;
  };

  struct FixedAsciiValue
  {
    std::string _text;
  };

  struct BytesValue
  {
    std::vector<std::byte> _bytes;
  };

  struct DateValue
  {
    std::variant<std::chrono::year_month_day, std::chrono::year_month,
                   std::chrono::year, std::chrono::month_day> _value;
  };

  struct RecordedZone
  {
    std::chrono::seconds _utcOffset{};
    // Optional annotation. The recorded offset governs interpretation;
    // decoding never consults the machine's local time zone or current tzdb.
    std::string _label;
  };

  struct TimeValue
  {
    std::chrono::nanoseconds _sinceMidnight{};
    RecordedZone _zone;
  };

  struct DateTimeValue
  {
    std::chrono::year_month_day _date;
    TimeValue _time;
  };

  struct TimestampValue
  {
    UtcTimestamp _utc;
    RecordedZone _presentationZone;
  };

  struct DurationValue
  {
    std::int64_t _count{};
    DurationUnit _unit{DurationUnit::Nanoseconds};
    // Calendar years/months are not silently converted to fixed elapsed time.
  };

  struct SequenceValue
  {
    std::uint64_t _value{};
  };

  struct GuidValue
  {
    std::array<std::byte, 16> _bytes{};
  };

  struct SymbolValue
  {
    std::string _text;
  };

  struct StringValue
  {
    std::string _text;
  };

  struct FieldValue;

  struct ArrayValue
  {
    std::vector<FieldValue> _elements;
  };

  struct TupleValue
  {
    std::vector<FieldValue> _fields;
  };

  struct GroupValue
  {
    std::vector<TupleValue> _rows;
  };

  // Every alternative owns its data. Decoding pool-backed fields produces text,
  // never a borrowed native offset. Scalar alternatives use ordinary C++ math.
  struct FieldValue
  {
    using Value = std::variant<bool, char, std::byte, std::int8_t, std::uint8_t,
                                 std::int16_t, std::uint16_t, std::int32_t,
                                 std::uint32_t, std::int64_t, std::uint64_t,
                                 float, double, BitsValue, EnumerationValue,
                                 FixedAsciiValue, BytesValue, DateValue, TimeValue,
                                 DateTimeValue, TimestampValue, DurationValue,
                                 SequenceValue, GuidValue, SymbolValue, StringValue,
                                 ArrayValue, TupleValue, GroupValue>;
    Value _value;
  };

  struct FieldConstraints
  {
    // Bounds apply only to orderable compatible types. Pattern is a whole-text
    // ECMAScript match for textual fields. Units document meaning; they do not
    // request implicit conversion of application values.
    std::optional<FieldValue> _minimum;
    std::optional<FieldValue> _maximum;
    std::optional<std::string> _pattern;
    std::optional<std::string> _units;
  };

  struct FieldDescriptor
  {
    FieldId _id{};
    std::string _name;
    FieldType _type;
    FieldConstraints _constraints;
    std::string _doc;
  };

  struct MessageDescriptor
  {
    SchemaId _schema{};
    std::string _name;
    std::vector<FieldId> _fields; // Ordered occurrences; repetition is valid.
    std::string _doc;
  };

  struct CodecLimits
  {
    std::uint64_t _maxPayloadBytes{1048576};
    std::uint64_t _maxMaterializedBytes{16777216};
    std::uint64_t _maxCatalogBytes{16777216};
    std::uint64_t _maxTextBytes{4194304};
    std::uint32_t _maxNesting{16};
    std::uint32_t _maxFieldValues{65536};
  };

  struct SymbolReference
  {
    // Values are never reused within a Session. Expired retained identities
    // return HistoryUnavailable; unknown identities return NotFound.
    SessionId _session{};
    std::uint32_t _value{};
  };

  struct StringReference
  {
    // Same no-reuse/expired-identity contract as SymbolReference.
    SessionId _session{};
    std::uint64_t _value{};
  };

  struct PoolUsage
  {
    std::uint64_t _entries{};
    std::uint64_t _entryLimit{};
    std::uint64_t _bytes{};
    std::uint64_t _byteLimit{};
    std::uint64_t _leasedEntries{};
  };

  class SymbolLease
  {
  public:
    SymbolLease(SymbolLease&&) noexcept;
    SymbolLease& operator=(SymbolLease&&) noexcept;
    SymbolLease(const SymbolLease&) = delete;
    SymbolLease& operator=(const SymbolLease&) = delete;
    ~SymbolLease();

    SymbolReference reference() const;
    Result<std::string_view> text() const &; // Borrows this lease.
    Result<std::string_view> text() const && = delete;

  private:
    friend class SymbolTable;
    explicit SymbolLease(std::shared_ptr<detail::PoolLeaseState> state);
    std::shared_ptr<detail::PoolLeaseState> _state;
  };

  class StringLease
  {
  public:
    StringLease(StringLease&&) noexcept;
    StringLease& operator=(StringLease&&) noexcept;
    StringLease(const StringLease&) = delete;
    StringLease& operator=(const StringLease&) = delete;
    ~StringLease();

    StringReference reference() const;
    Result<std::string_view> text() const &; // Borrows this lease.
    Result<std::string_view> text() const && = delete;

  private:
    friend class StringPool;
    explicit StringLease(std::shared_ptr<detail::PoolLeaseState> state);
    std::shared_ptr<detail::PoolLeaseState> _state;
  };

  // These copyable process-local handles may resolve a live Session's pools or
  // an archive's retained pool namespace. Read-only handles reject interning.
  // Local leases and persisted message retention prevent referenced values
  // from being reclaimed; no caller manually frees a stored reference.
  class SymbolTable
  {
  public:
    SessionId session() const;
    bool writable() const;
    Result<SymbolLease> intern(std::string_view text);
    Result<SymbolLease> acquire(SymbolReference reference) const;
    Result<std::string> resolve(SymbolReference reference) const;
    Result<PoolUsage> usage() const;

  private:
    friend class Session;
    friend class Task;
    friend class LogArchive;
    explicit SymbolTable(std::shared_ptr<detail::PoolState> state);
    std::shared_ptr<detail::PoolState> _state;
  };

  class StringPool
  {
  public:
    SessionId session() const;
    bool writable() const;
    Result<StringLease> intern(std::string_view text);
    Result<StringLease> acquire(StringReference reference) const;
    Result<std::string> resolve(StringReference reference) const;
    Result<PoolUsage> usage() const;

  private:
    friend class Session;
    friend class Task;
    friend class LogArchive;
    explicit StringPool(std::shared_ptr<detail::PoolState> state);
    std::shared_ptr<detail::PoolState> _state;
  };

  struct PoolContext
  {
    std::optional<SymbolTable> _symbols;
    std::optional<StringPool> _strings;
  };

  class EncodedPayload
  {
  public:
    EncodedPayload(EncodedPayload&&) noexcept;
    EncodedPayload& operator=(EncodedPayload&&) noexcept;
    EncodedPayload(const EncodedPayload&) = delete;
    EncodedPayload& operator=(const EncodedPayload&) = delete;
    ~EncodedPayload();

    SessionId schema_session() const;
    SchemaId schema() const;
    PayloadFormat format() const;
    std::span<const std::byte> bytes() const &;
    std::span<const std::byte> bytes() const && = delete;
    // Native pool leases remain owned until publication copies their retention
    // obligations or this payload is destroyed. No native offsets are exposed.

  private:
    friend class SchemaCodec;
    explicit EncodedPayload(std::shared_ptr<detail::EncodedPayloadState> state);
    std::shared_ptr<detail::EncodedPayloadState> _state;
  };

  // Owns pool pins, but borrows the caller's written output prefix. Both the
  // output storage and this lease must remain alive through send/publish. A
  // committed publication then supplies persisted retention independently.
  class PayloadLease
  {
  public:
    PayloadLease(PayloadLease&&) noexcept;
    PayloadLease& operator=(PayloadLease&&) noexcept;
    PayloadLease(const PayloadLease&) = delete;
    PayloadLease& operator=(const PayloadLease&) = delete;
    ~PayloadLease();

    SessionId schema_session() const;
    SchemaId schema() const;
    PayloadFormat format() const;
    std::span<const std::byte> bytes() const &;
    std::span<const std::byte> bytes() const && = delete;

  private:
    friend class SchemaCodec;
    explicit PayloadLease(std::shared_ptr<detail::PayloadLeaseState> state);
    std::shared_ptr<detail::PayloadLeaseState> _state;
  };

  struct PayloadSizeBounds
  {
    std::uint64_t _minimumBytes{};
    std::uint64_t _maximumBytes{};
    std::optional<std::uint64_t> _fixedBytes;
  };

  class SchemaCodec
  {
  public:
    static Result<SchemaCodec> from_registry(const Registry& registry,
        SchemaId schema, CodecLimits limits = {});
    static Result<SchemaCodec> from_snapshot(SessionId schemaSession,
        const RegistrySnapshot& snapshot, SchemaId schema, CodecLimits limits = {});
    static Result<SchemaCodec> from_catalog(SessionId schemaSession,
        std::span<const FieldDescriptor> fields,
        std::span<const MessageDescriptor> messages, SchemaId schema,
        CodecLimits limits = {});

    SessionId schema_session() const;
    SchemaId schema() const;
    PayloadSizeBounds binary_size() const;
    std::span<const FieldDescriptor> fields() const &;
    std::span<const FieldDescriptor> fields() const && = delete;

    // Native Binary pool fields require matching writable pools on encode,
    // and matching retained pools on decode. Decoded values always own text.
    // A cross-Session replay decodes in source context, then re-encodes against
    // the target catalog and pools. Raw pool-reference bytes cannot be copied.
    Result<EncodedPayload> encode(std::span<const FieldValue> values,
        PayloadFormat format = PayloadFormat::Binary, const PoolContext& pools = {}) const;
    Result<EncodedPayload> encode(std::initializer_list<FieldValue> values,
        PayloadFormat format = PayloadFormat::Binary, const PoolContext& pools = {}) const;
    // Sizing validates logical values and complete byte cost without interning
    // pool entries. Encoding can still fail on target pool capacity/ownership.
    Result<std::uint64_t> encoded_size(std::span<const FieldValue> values,
        PayloadFormat format = PayloadFormat::Binary) const;
    // Output capacity must cover encoded_size(). Validation/preparation ends
    // before replacing its prefix; any error leaves caller bytes unchanged.
    // The returned owner retains pool pins, not the output storage itself.
    Result<PayloadLease> encode_into(std::span<const FieldValue> values,
        std::span<std::byte> output, PayloadFormat format = PayloadFormat::Binary,
        const PoolContext& pools = {}) const;
    Result<std::vector<FieldValue>> decode(std::span<const std::byte> payload,
        PayloadFormat format = PayloadFormat::Binary, const PoolContext& pools = {}) const;

    // Json/Xml/Csv/Text serializers are schema-driven, including bounded nested
    // values. Binary is rejected here. They resolve logical text, not pool IDs;
    // exact escaping/compound representations require their own format contract.
    Result<std::string> serialize(std::span<const FieldValue> values,
        PayloadFormat format) const;
    Result<std::vector<FieldValue>> parse(std::string_view text,
        PayloadFormat format) const;

  private:
    explicit SchemaCodec(std::shared_ptr<detail::SchemaCodecState> state);
    std::shared_ptr<detail::SchemaCodecState> _state;
  };
} // namespace angaria::proposed
