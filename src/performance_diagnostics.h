#pragma once

// Private, opt-in measurement plumbing. Normal builds remove every scope and
// counter macro, including evaluation of counter arguments. A benchmark owns
// each fixed-size Sample; the collector never allocates or writes output.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace angaria::detail::diagnostics
{
  enum class Collection
  {
    None,
    Counts,
    Timings
  };

  enum class Phase : std::size_t
  {
    JournalOpen,
    Recovery,
    Snapshot,
    PrefixScan,
    RecordIndex,
    TaskCheck,
    TaskCatalog,
    Topology,
    DeliveryValidation,
    PublicationLookup,
    SchemaLookup,
    CodecBuild,
    PayloadValidation,
    PublicationPlan,
    JournalPreflight,
    JournalBody,
    FileGrowth,
    Materialize,
    CommitApply,
    SourceReconcile,
    ReceiveSelect,
    NativeRead,
    NativeMap,
    GateAcquire,
    NativeStat,
    NativeUnmap,
    // Acquisition subphases are siblings under GateAcquire. A nonblocking
    // attempt measures syscall/probe cost rather than blocked wait time.
    LocalGateWait,
    NativeGateWait,
    // Reserved for root wall time outside all observed regions.
    Other,
    Count
  };

  enum class Counter : std::size_t
  {
    PrefixScans,
    PrefixRecords,
    IndexScans,
    IndexedRecords,
    TopologyBuilds,
    DeliveryValidations,
    ValidatedPublications,
    PublicationLookups,
    LookupPublications,
    TaskSlots,
    Allocations,
    AllocatedBytes,
    EmptyPolls,
    NativeReadCalls,
    NativeReadBytes,
    NativeMapCalls,
    NativeMappedBytes,
    NativeUnmapCalls,
    FileStatCalls,
    FileGrowCalls,
    GateAttempts,
    GateBusy,
    GateAcquisitions,
    RecoveryChecks,
    RecoveryReplays,
    NativeGateHolds,
    NativeGateHoldNs,
    NativeGateBlockingAttempts,
    NativeGateNonblockingAttempts,
    Count
  };

  inline constexpr std::size_t phase_count = static_cast<std::size_t>(Phase::Count);
  inline constexpr std::size_t counter_count = static_cast<std::size_t>(Counter::Count);
  inline constexpr std::array<std::string_view, phase_count> phase_names{
      "journal_open", "recovery", "snapshot", "prefix_scan", "record_index", "task_check",
      "task_catalog", "topology", "delivery_validation", "publication_lookup", "schema_lookup",
      "codec_build", "payload_validation", "publication_plan", "journal_preflight", "journal_body",
      "file_growth", "materialize", "commit_apply", "source_reconcile", "receive_select",
      "native_read", "native_map", "gate_acquire", "native_stat", "native_unmap",
      "local_gate_wait", "native_gate_wait", "other"};
  inline constexpr std::array<std::string_view, counter_count> counter_names{
      "prefix_scans", "prefix_records", "index_scans", "indexed_records", "topology_builds",
      "delivery_validations", "validated_publications", "publication_lookups", "lookup_publications",
      "task_slots", "allocations", "allocated_bytes", "empty_polls", "native_read_calls",
      "native_read_bytes", "native_map_calls", "native_mapped_bytes", "native_unmap_calls",
      "file_stat_calls", "file_grow_calls", "gate_attempts", "gate_busy", "gate_acquisitions",
      "recovery_checks", "recovery_replays", "native_gate_holds", "native_gate_hold_ns",
      "native_gate_blocking_attempts", "native_gate_nonblocking_attempts"};

#if defined(ANGARIA_ENABLE_DIAGNOSTICS) && ANGARIA_ENABLE_DIAGNOSTICS
  inline constexpr bool instrumentation_enabled = true;
#else
  inline constexpr bool instrumentation_enabled = false;
#endif

  struct PhaseStats
  {
    std::uint64_t _calls{};
    std::uint64_t _inclusiveNs{};
    std::uint64_t _exclusiveNs{};
  };

  struct Sample
  {
    std::array<PhaseStats, phase_count> _phases{};
    std::array<std::uint64_t, counter_count> _counters{};
    std::uint64_t _wallNs{};
    std::uint64_t _cpuNs{};
    bool _cpuAvailable{};
    bool _overflow{};
    bool _valid{true};
  };

  class Scope;
  class GateHold;

  // One capture surrounds one actual public operation. Its wall and CPU clocks
  // remain available with collection disabled, for matching baseline workloads.
  // Nested captures use distinct Samples and restore the outer thread context.
  class Capture
  {
  public:
    Capture(Sample& sample, Collection collection) noexcept;
    ~Capture() noexcept;
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

  private:
    friend class Scope;
    friend class GateHold;
    friend void count(Counter counter, std::uint64_t amount) noexcept;
    Sample* _sample{};
    Capture* _previous{};
    Scope* _scope{};
    Collection _collection{Collection::None};
    std::uint64_t _startedNs{};
    std::uint64_t _startedCpuNs{};
#if defined(ANGARIA_ENABLE_DIAGNOSTICS) && ANGARIA_ENABLE_DIAGNOSTICS
    std::uint64_t _epoch{};
#endif
    bool _cpuAvailable{};
  };

  // Stack-linked frames avoid allocation and make exclusive times meaningful
  // even when one measured phase calls another phase with the same name.
  class Scope
  {
  public:
    explicit Scope(Phase phase) noexcept;
    ~Scope() noexcept;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    friend class Capture;
    Capture* _capture{};
    Scope* _parent{};
    Phase _phase{Phase::Other};
    std::uint64_t _startedNs{};
    std::uint64_t _childNs{};
  };

  // A completed native-lock ownership interval overlaps ordinary phases; its
  // nanoseconds must not be added to exclusive phase accounting. The token
  // stores a unique Capture epoch, never a Capture or Sample pointer.
  // finish() contributes only while the exact originating Capture is current.
  class GateHold
  {
  public:
    GateHold() noexcept = default;
    GateHold(const GateHold&) = delete;
    GateHold& operator=(const GateHold&) = delete;
    void start() noexcept;
    void finish() noexcept;

  private:
#if defined(ANGARIA_ENABLE_DIAGNOSTICS) && ANGARIA_ENABLE_DIAGNOSTICS
    std::uint64_t _epoch{};
    std::uint64_t _startedNs{};
#endif
  };

  void count(Counter counter, std::uint64_t amount = 1) noexcept;

#if defined(ANGARIA_ENABLE_DIAGNOSTICS) && ANGARIA_ENABLE_DIAGNOSTICS
  // Called only by the diagnostic executable's replacement new operators.
  // Counts requested C++ allocation bytes, not malloc traffic or live memory.
  void record_allocation(std::size_t bytes) noexcept;
#else
  inline void record_allocation(std::size_t) noexcept
  {
  }
#endif
} // namespace angaria::detail::diagnostics

#define ANGARIA_DIAGNOSTIC_JOIN_INNER(left, right) left##right
#define ANGARIA_DIAGNOSTIC_JOIN(left, right) ANGARIA_DIAGNOSTIC_JOIN_INNER(left, right)

#if defined(ANGARIA_ENABLE_DIAGNOSTICS) && ANGARIA_ENABLE_DIAGNOSTICS
#define ANGARIA_DIAGNOSTIC_SCOPE(phase) \
  ::angaria::detail::diagnostics::Scope ANGARIA_DIAGNOSTIC_JOIN(angariaDiagnosticScope, __LINE__){ \
      ::angaria::detail::diagnostics::Phase::phase}
#define ANGARIA_DIAGNOSTIC_COUNT(counter, amount) \
  ::angaria::detail::diagnostics::count(::angaria::detail::diagnostics::Counter::counter, (amount))
#else
#define ANGARIA_DIAGNOSTIC_SCOPE(phase) ((void)0)
#define ANGARIA_DIAGNOSTIC_COUNT(counter, amount) ((void)0)
#endif
