#pragma once

// Private current-log experiment. Publication acceptance is still provided by
// QueueWriter::send; its existing Pending receipts are not consumed or changed.
// This separate connection owns only volatile delivery cursors and credits.
#include "angaria/message.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace angaria::benchmarks::spsc
{
  enum class Role
  {
    Producer,
    Consumer
  };

  struct Options
  {
    std::filesystem::path _directory;
    SessionId _session{};
    QueueId _queue{};
    TaskId _producer{};
    SchemaId _schema{};
    std::uint64_t _payloadBytes{};
    std::uint64_t _capacity{32};
    std::uint64_t _maxLogBytes{64ULL * 1024 * 1024};
  };

  class RecordedSpsc
  {
  public:
    // Call after untimed preload. Captures the stream's retained-log boundary;
    // previous acknowledged preload messages do not join this new connection.
    static void create(const Options& options);
    RecordedSpsc(const Options& options, Role role, TaskClaim actor);
    ~RecordedSpsc();
    RecordedSpsc(RecordedSpsc&&) noexcept;
    RecordedSpsc& operator=(RecordedSpsc&&) noexcept;
    RecordedSpsc(const RecordedSpsc&) = delete;
    RecordedSpsc& operator=(const RecordedSpsc&) = delete;

    // One producer preflights credit BEFORE issuing an ID or accepting a send.
    // Capture the record reference AFTER ID issuance and immediately before
    // the public send. Fixed topology and the sole log appender are required.
    bool has_credit() const;
    std::uint64_t next_record_reference() const;
    void publish_committed(std::uint64_t reference, PublicationId publication);

    // A successful borrow keeps its logged bytes valid until this endpoint is
    // destroyed, including later ring wraps/receives. copy_message() retains
    // the exact header and decoding dependencies beyond endpoint destruction.
    // Ending a borrow or returning ring credit certifies no application work.
    std::optional<MessageView> try_receive();

    // Untimed, owning replay from the connection's start. It recovers the
    // existing journal under its normal gate, then reads accepted records.
    // A restarted consumer likewise starts at the connection's beginning:
    // duplicate application delivery is possible and deliberately unacked.
    static std::vector<Message> replay(const Options& options);

  private:
    struct State;
    std::unique_ptr<State> _state;
  };
} // namespace angaria::benchmarks::spsc
