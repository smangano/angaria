#include "public_runtime_storage.h"

#include "angaria/payload.h"
#include "topology_state.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace angaria::detail
{
  namespace
  {
    std::uint64_t seal_revision(RuntimeAccess& access)
    {
      const auto seal = access.read_words(mapped::catalog_seal_offset, 1).front();
      if (seal != 0 && seal != access._snapshot._revision)
      {
        runtime_fail(ErrorCode::IoError, "registry.seal", "Sealed catalog revision differs from the administrative Registry");
      }
      return seal;
    }

    std::filesystem::path resource_path(const std::filesystem::path& directory,
                                         const char* kind, std::uint16_t id, const char* suffix)
    {
      return directory / (std::string{kind} + std::to_string(id) + suffix);
    }

    void remove_unpublished_resource(const std::filesystem::path& path)
    {
      std::error_code error;
      const auto status = std::filesystem::symlink_status(path, error);
      if (error && error != std::errc::no_such_file_or_directory)
      {
        runtime_fail(ErrorCode::IoError, "registry.bootstrap", error.message());
      }
      if (status.type() == std::filesystem::file_type::not_found)
      {
        return;
      }
      if (status.type() != std::filesystem::file_type::regular)
      {
        runtime_fail(ErrorCode::IoError, "registry.bootstrap", "Unpublished messaging resource is not a regular file");
      }
      if (!std::filesystem::remove(path))
      {
        runtime_fail(ErrorCode::IoError, "registry.bootstrap", "Unpublished messaging resource could not be removed");
      }
    }

    void bootstrap_resources(const RecordedSessionResources& resources)
    {
      // The caller holds the administrative gate and the seal is still zero.
      // No endpoint can have escaped. A crash before sealing therefore permits
      // rebuilding exactly these unpublished resources, without a migration or
      // partial-file reader. Never remove a resource from a sealed Session.
      for (const auto& [task, producer] : resources._producers)
      {
        (void)task;
        remove_unpublished_resource(producer._paths._log);
        remove_unpublished_resource(producer._paths._owner);
      }
      for (const auto& [queue, options] : resources._queues)
      {
        (void)queue;
        remove_unpublished_resource(options._connectionPath);
        if (options._acknowledged)
        {
          remove_unpublished_resource(options._progressPath);
        }
      }
      for (const auto& [task, producer] : resources._producers)
      {
        (void)task;
        runtime_value(recorded::ProducerLog::create(producer._paths, producer._options));
      }
      for (const auto& [queue, options] : resources._queues)
      {
        (void)queue;
        runtime_value(recorded::create_recorded_connection(options));
      }
    }
  } // namespace

  void runtime_require_unsealed(RuntimeAccess& access)
  {
    if (seal_revision(access) != 0)
    {
      runtime_fail(ErrorCode::InvalidState, "registry.seal", "Complete registration and activation before opening messaging endpoints or issuing publication IDs");
    }
  }

  std::shared_ptr<const RecordedSessionResources> runtime_recorded_resources(RuntimeAccess& access, TaskClaim actor)
  {
    const auto sealed = seal_revision(access);
    if (sealed == 0 && (actor._task == TaskId::Invalid || actor._session == SessionId{} || actor._incarnation == 0))
    {
      runtime_fail(ErrorCode::InvalidState, "registry.seal", "Read-only capture requires an already sealed messaging catalog");
    }
    if (const auto& cached = access._session->_recordedResources; cached && sealed != 0)
    {
      if (cached->_catalog._revision != sealed)
      {
        runtime_fail(ErrorCode::IoError, "registry.seal", "Cached catalog differs from its committed seal");
      }
      return cached;
    }
    const auto session = runtime_session_definition(access);
    const auto tasks = runtime_tasks(access);
    const auto fields = runtime_fields(access, tasks);
    const auto schemas = runtime_schemas(access, fields, tasks);
    const auto topology = runtime_topology(access);
    auto resources = std::make_shared<RecordedSessionResources>();
    auto& catalog = resources->_catalog;
    catalog._revision = access._snapshot._revision;
    catalog._currentRevision = access._snapshot._revision;
    for (const auto& task : tasks)
    {
      const auto& definition = task._definition;
      catalog._tasks.push_back({definition._id, definition._descriptor, definition._registration});
    }
    for (const auto& field : fields)
    {
      catalog._fields.push_back({field._definition._descriptor, field._definition._registration});
    }
    std::vector<recorded::RecordedSchemaBinding> bindings;
    for (const auto& schema : schemas)
    {
      catalog._messages.push_back({schema._definition._descriptor, schema._definition._registration});
      bindings.push_back({schema._definition._descriptor._schema, schema._reference});
    }
    for (const auto& queue : topology._queues)
    {
      const auto& definition = queue._definition;
      if (definition._state != EntityState::Active)
      {
        runtime_fail(ErrorCode::NotActive, "queue.state", "Every registered Queue must be activated before sealing the catalog");
      }
      if (definition._descriptor._delivery != DeliveryPolicy::RecoverFromLog)
      {
        runtime_fail(ErrorCode::UnsupportedPolicy, "queue.delivery", "Public recorded delivery currently requires recovery from the message log");
      }
      catalog._queues.push_back({definition._id, definition._descriptor, definition._registration, definition._state});
    }
    for (const auto& source : topology._sources)
    {
      const auto& definition = source._definition;
      if (definition._descriptor._selection != SelectionPolicy::Strict || definition._descriptor._wait != WaitPolicy::Spin ||
          definition._members.size() > 1)
      {
        runtime_fail(ErrorCode::UnsupportedPolicy, "source_set.policy", "Public point-to-point admission requires one input and Strict/Spin policies");
      }
      catalog._sources.push_back({definition._id, definition._descriptor, definition._registration,
                                 definition._state, definition._members});
    }
    for (const auto& publishing : topology._publishing)
    {
      const auto& definition = publishing._definition;
      catalog._publishing.push_back({definition._id, definition._descriptor, definition._registration,
                                    definition._state, definition._topologyRevision});
    }
    if (!catalog._queues.empty() && bindings.empty())
    {
      runtime_fail(ErrorCode::InvalidState, "message.schema", "Declare an exact Binary schema before opening messaging endpoints");
    }
    for (const auto& binding : bindings)
    {
      (void)runtime_value(BinaryPayloadCodec::from_snapshot(catalog, binding._schema, session._options._producerLogBytes));
    }

    const auto& directory = access._session->_directory;
    const auto& configuration = access._session->_controller->configuration();
    for (const auto& task : catalog._tasks)
    {
      RecordedProducerResources producer;
      const auto id = static_cast<std::uint16_t>(task._id);
      producer._paths = {resource_path(directory, "producer-", id, ".messages.ang"),
                         resource_path(directory, "producer-", id, ".owner.ang")};
      producer._options._header = {configuration._session, task._id, catalog._revision,
          session._options._producerLogBytes, session._options._producerLogPublications, configuration._liveAbiProfile};
      for (const auto& queue : catalog._queues)
      {
        if (queue._descriptor._producer == task._id)
        {
          producer._options._queues.push_back(queue._id);
        }
      }
      if (producer._options._queues.empty())
      {
        // No Queue can be added after sealing. ID-only Tasks need the same
        // ownership/issuance protocol, but reserve no unused message capacity.
        producer._options._header._fileBytes = recorded::producer_header_bytes + recorded::minimum_publication_bytes;
        producer._options._header._publicationCapacity = 1;
      }
      (void)runtime_value(recorded::encode_producer_header(producer._options._header));
      resources->_producers.emplace(task._id, std::move(producer));
    }
    for (const auto& queue : catalog._queues)
    {
      const auto& producer = resources->_producers.at(queue._descriptor._producer);
      const auto source = std::find_if(catalog._sources.begin(), catalog._sources.end(), [&](const auto& entry)
      {
        return entry._id == queue._descriptor._sourceSet;
      });
      if (source == catalog._sources.end() || source->_state != EntityState::Active ||
          source->_members != std::vector{queue._id})
      {
        runtime_fail(ErrorCode::InvalidState, "queue.source_set", "Active Queue requires its exact active single-input Source");
      }
      const auto capacity = *queue._descriptor._ringCapacity;
      if (capacity == 0 || capacity > 65536 || capacity > session._options._producerLogPublications)
      {
        runtime_fail(ErrorCode::InvalidConfiguration, "queue.ring_capacity", "Queue credit exceeds the recorded connection reservation");
      }
      recorded::RecordedQueueOptions options;
      options._producerPaths = producer._paths;
      options._log = producer._options;
      options._queue = queue._id;
      options._source = source->_id;
      options._consumer = source->_descriptor._owner;
      options._catalog = catalog;
      options._schemaBindings = bindings;
      options._capacity = capacity;
      options._topologyRevision = queue._registration._revision;
      options._acknowledged = source->_descriptor._acknowledgment.value_or(AcknowledgmentMode::Required) == AcknowledgmentMode::Required;
      const auto id = static_cast<std::uint16_t>(queue._id);
      options._connectionPath = resource_path(directory, "queue-", id, ".transport.ang");
      if (options._acknowledged)
      {
        options._progressPath = resource_path(directory, "queue-", id, ".progress.ang");
      }
      resources->_queues.emplace(queue._id, std::move(options));
    }
    if (sealed == 0)
    {
      bootstrap_resources(*resources);
      mapped::TransactionPlan seal;
      seal._base = access._snapshot;
      seal._kind = mapped::JournalKind::FixedState;
      seal._actor = actor._task;
      seal._actorIncarnation = actor._incarnation;
      seal._newRevision = access._snapshot._revision;
      const std::array words{access._snapshot._revision};
      seal._commands.push_back(runtime_write_words(mapped::catalog_seal_offset, words));
      runtime_value(access.execute(seal));
    }
    // Access to this shared Session cache remains under the administrative gate;
    // established Task/endpoint handles retain their own immutable shared owner.
    access._session->_recordedResources = resources;
    return resources;
  }
} // namespace angaria::detail
