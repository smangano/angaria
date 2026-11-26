#pragma once

#include "journal_fixture.h"
#include "native_fixture.h"
#include "publication_metadata.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace angaria::test_support
{
  struct StoredPayloadPublication
  {
    std::uint64_t _reference{};
    std::uint64_t _payloadOffset{};
    std::uint64_t _payloadBytes{};
    std::uint64_t _receiptOffset{};
    std::uint64_t _totalBytes{};
  };

  // Private storage qualification, deliberately not a bootstrapped Session.
  // The older Task record supplies a correctly shaped historical reference;
  // it is not a registered schema. These fixtures isolate committed record
  // geometry, mapping lifetime, and rollback from later delivery semantics.
  class PublicationStorageFixture
  {
  public:
    explicit PublicationStorageFixture(std::size_t payloadBytes = 8193)
    {
      auto pattern = (std::filesystem::temp_directory_path() / "angaria-payload-storage-XXXXXX").string();
      if (::mkdtemp(pattern.data()) == nullptr)
      {
        throw std::runtime_error("Cannot create payload storage fixture directory");
      }
      _root = pattern;
      try
      {
        _directory = _root / "session";
        std::filesystem::create_directories(_directory / "claims");
        static std::atomic<std::uint64_t> nextSession{1};
        _configuration._session = {static_cast<std::uint64_t>(::getpid()), nextSession.fetch_add(1)};
        _configuration._liveAbiProfile = require_value(mapped::qualify_native_profile())._liveAbiProfile;
        _configuration._formatMinor = mapped::delivery_format_minor;
        _configuration._requiredFeatures = mapped::journal_feature | mapped::basic_fields_feature |
                                             mapped::native_delivery_feature;
        _configuration._maxLogBytes = 4 * 1024 * 1024;
        _configuration._limits = CapacityLimits::empty().tasks(2).queues(2).source_sets(1)
                                    .publishing_sets(1).memberships_per_set(2).string_pool_bytes(64);
        const auto layout = mapped::make_fixed_layout(_configuration._limits);
        if (!layout)
        {
          throw std::runtime_error("Payload storage fixture has no fixed layout");
        }
        auto file = require_value(mapped::NativeFile::create_exclusive(_directory / "session.ang",
                                                                       _configuration._maxLogBytes));
        require_value(file.resize(layout->_arenaBegin));
        require_value(file.write_exact(0, require_value(mapped::encode_initial_superblock(_configuration))));
        require_value(mapped::NativeFile::create_exclusive(_directory / "mutation.lock"));
        _state = open_runtime_state(_directory);
        {
          detail::RuntimeAccess access(_state, false);
          const auto result = require_value(access._journal.execute(access._guard,
              make_plan(access._snapshot, _configuration, true)));
          _olderReference = result._records.front();
        }
        _publication = append_publication(payloadBytes);
      }
      catch (...)
      {
        cleanup();
        throw;
      }
    }

    ~PublicationStorageFixture()
    {
      cleanup();
    }

    PublicationStorageFixture(const PublicationStorageFixture&) = delete;
    PublicationStorageFixture& operator=(const PublicationStorageFixture&) = delete;

    StoredPayloadPublication append_publication(std::size_t payloadBytes)
    {
      detail::RuntimeAccess access(_state, false);
      const auto reference = access._snapshot._tail;
      const auto layout = require_value(mapped::plan_publication_layout(reference, 1, payloadBytes,
                                                                         _configuration._maxLogBytes));
      mapped::PublicationDefinition definition;
      definition._producer = TaskId{1};
      definition._schema = {MessageType{1}, 1};
      definition._format = PayloadFormat::Text;
      definition._producerIncarnation = 7;
      definition._publicationSequence = ++_publicationSequence;
      definition._topologyRevision = access._snapshot._revision;
      definition._schemaReference = _olderReference;
      definition._destinations = {mapped::DestinationDefinition{
          QueueId{1}, DestinationStatus::Accepted, ErrorCode::None,
          _publicationSequence, _previousDelivery, {}, layout._receiptsOffset}};
      definition._payload.assign(payloadBytes, std::byte{0xa5});
      if (!definition._payload.empty())
      {
        definition._payload.front() = std::byte{0x41};
        definition._payload.back() = std::byte{0x5a};
      }
      mapped::JournalRecord record;
      record._kind = 9;
      record._immutable = require_value(mapped::encode_publication_body(definition, reference,
                                                                         _configuration._maxLogBytes));
      record._mutable.resize(mapped::publication_receipt_bytes);
      put_integer(record._mutable, 0, 1); // Pending; reserved receipt bytes remain zero.
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::Append;
      plan._subtype = mapped::TransactionSubtype::Publication;
      plan._actor = TaskId{1};
      plan._actorIncarnation = 7;
      plan._newRevision = access._snapshot._revision;
      plan._records.push_back(std::move(record));
      plan._commands.push_back(write_word(layout._receiptsOffset, 1));
      require_value(access._journal.execute(access._guard, plan));
      _committedSnapshot = require_value(access._journal.snapshot(access._guard));
      _previousDelivery = layout._destinationsOffset;
      return {reference, layout._payloadOffset, payloadBytes, layout._receiptsOffset, layout._totalBytes};
    }

    std::filesystem::path _directory;
    std::shared_ptr<detail::SessionState> _state;
    mapped::SuperblockConfiguration _configuration;
    StoredPayloadPublication _publication;
    mapped::JournalSnapshot _committedSnapshot;

  private:
    void cleanup() noexcept
    {
      _state.reset();
      std::error_code ignored;
      std::filesystem::remove_all(_root, ignored);
    }

    std::filesystem::path _root;
    std::uint64_t _olderReference{};
    std::uint64_t _previousDelivery{};
    std::uint64_t _publicationSequence{};
  };
} // namespace angaria::test_support
