#include "configuration.h"

#include "angaria/detail/cpptoml.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/field_representation.h"
#include "angaria/detail/identity_helpers.h"
#include "runtime_state.h"
#include "schema_metadata.h"
#include "topology_append.h"
#include "topology_metadata.h"
#include "topology_policy.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace angaria::detail
{
  namespace
  {
    using Table = std::shared_ptr<cpptoml::table>;
    using Node = std::shared_ptr<cpptoml::base>;

    [[noreturn]] void invalid(const std::string& path, const std::string& message)
    {
      runtime_fail(ErrorCode::InvalidConfiguration, path, message);
    }

    std::string key_path(const std::string& path, std::string_view key)
    {
      return path.empty() ? std::string{key} : path + "." + std::string{key};
    }

    Node optional_node(const Table& table, const std::string& key)
    {
      return table->contains(key) ? table->get(key) : nullptr;
    }

    Node required_node(const Table& table, const std::string& key, const std::string& path)
    {
      const auto value = optional_node(table, key);
      if (!value)
      {
        invalid(key_path(path, key), "Required configuration key is missing");
      }
      return value;
    }

    void check_keys(const Table& table, const std::string& path,
                       std::initializer_list<std::string_view> allowed)
    {
      for (const auto& [key, value] : *table)
      {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
        {
          invalid(key_path(path, key), "Unknown configuration key '" + key + "'");
        }
      }
    }

    Table child_table(const Table& table, const std::string& key, const std::string& path,
                         bool required = false)
    {
      const auto value = required ? required_node(table, key, path) : optional_node(table, key);
      if (!value)
      {
        return cpptoml::make_table();
      }
      const auto result = value->as_table();
      if (!result)
      {
        invalid(key_path(path, key), "Expected a TOML table");
      }
      return result;
    }

    std::string string_value(const Table& table, const std::string& key, const std::string& path,
                                 bool required = true, const std::string& fallback = {})
    {
      const auto node = required ? required_node(table, key, path) : optional_node(table, key);
      if (!node)
      {
        return fallback;
      }
      const auto value = node->as<std::string>();
      if (!value)
      {
        invalid(key_path(path, key), "Expected a TOML string");
      }
      return value->get();
    }

    // The parsed document retains these strings while its configuration plan
    // is validated. Length checks can inspect them without an additional copy.
    std::string_view borrowed_string_value(const Table& table, const std::string& key,
                                             const std::string& path, bool required = true)
    {
      const auto node = required ? required_node(table, key, path) : optional_node(table, key);
      if (!node)
      {
        return {};
      }
      const auto value = node->as<std::string>();
      if (!value)
      {
        invalid(key_path(path, key), "Expected a TOML string");
      }
      return value->get();
    }

    std::uint64_t unsigned_value(const Table& table, const std::string& key, const std::string& path,
                                    std::uint64_t minimum, std::uint64_t maximum,
                                    std::optional<std::uint64_t> fallback = {})
    {
      const auto node = fallback ? optional_node(table, key) : required_node(table, key, path);
      if (!node)
      {
        return *fallback;
      }
      const auto value = node->as<std::int64_t>();
      if (!value)
      {
        invalid(key_path(path, key), "Expected a TOML integer");
      }
      const auto integer = value->get();
      if (integer < 0 || static_cast<std::uint64_t>(integer) < minimum ||
          static_cast<std::uint64_t>(integer) > maximum)
      {
        invalid(key_path(path, key), "Value " + std::to_string(integer) + " must be in [" +
                std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
      }
      return static_cast<std::uint64_t>(integer);
    }

    template <typename T>
    T enum_value(const Table& table, const std::string& key, const std::string& path,
                     std::initializer_list<std::pair<std::string_view, T>> choices, T fallback)
    {
      if (!table->contains(key))
      {
        return fallback;
      }
      const auto value = string_value(table, key, path);
      for (const auto& [name, choice] : choices)
      {
        if (value == name)
        {
          return choice;
        }
      }
      invalid(key_path(path, key), "Unknown value '" + value + "'");
    }

    std::vector<std::string> string_array(const Table& table, const std::string& key, const std::string& path)
    {
      const auto values = required_node(table, key, path)->as_array();
      if (!values)
      {
        invalid(key_path(path, key), "Expected an array of strings");
      }
      std::vector<std::string> result;
      for (std::size_t index = 0; index < values->get().size(); ++index)
      {
        const auto value = values->get()[index]->as<std::string>();
        if (!value)
        {
          invalid(key_path(path, key) + "[" + std::to_string(index) + "]", "Expected a string reference");
        }
        result.push_back(value->get());
      }
      return result;
    }

    std::vector<Table> declaration_tables(const Table& root, const std::string& key,
                                           const std::string& path = {})
    {
      const auto node = optional_node(root, key);
      if (!node)
      {
        return {};
      }
      if (const auto tables = node->as_table_array())
      {
        return tables->get();
      }
      // An explicitly empty array is a valid spelling of an empty category.
      if (const auto array = node->as_array(); array && array->get().empty())
      {
        return {};
      }
      invalid(key_path(path, key), "Expected an array of TOML tables, or an empty array");
    }

    std::string indexed_path(std::string_view category, std::size_t index)
    {
      return std::string{category} + "[" + std::to_string(index) + "]";
    }

    // Shared metadata/policy validators name their descriptor kind. Replace it
    // with the original TOML array position and retain their detailed suffix.
    template <typename Function>
    auto at_path(const std::string& path, Function function)
    {
      try
      {
        return function();
      }
      catch (const RuntimeFailure& failure)
      {
        auto error = failure._error;
        if (error._code == ErrorCode::InvalidArgument)
        {
          error._code = ErrorCode::InvalidConfiguration;
        }
        const auto dot = error._path.find('.');
        error._path = path + (dot == std::string::npos ? "" : error._path.substr(dot));
        if (error._path.ends_with(".base"))
        {
          error._path.replace(error._path.size() - 5, 5, ".type");
        }
        throw RuntimeFailure{std::move(error)};
      }
    }

    void check_text(std::string_view text, const std::string& path, bool required = true,
                        bool noNull = true)
    {
      runtime_value(mapped::validate_metadata_text(text, required, noNull, path.c_str(),
                                                   ErrorCode::InvalidConfiguration));
    }

    void check_count(std::uint64_t count, std::uint64_t limit, const std::string& path,
                         const std::string& category)
    {
      if (count > limit)
      {
        runtime_fail(ErrorCode::CapacityExceeded, path, category + " count " + std::to_string(count) +
                     " exceeds configured capacity " + std::to_string(limit));
      }
    }

    EFieldBase field_base(const Table& table, const std::string& key, const std::string& path)
    {
      (void)required_node(table, key, path);
      return enum_value<EFieldBase>(table, key, path,
          {{"none", EFieldBase::BASE_NONE}, {"char", EFieldBase::BASE_CHAR}, {"byte", EFieldBase::BASE_BYTE},
           {"int8", EFieldBase::BASE_INT8}, {"int16", EFieldBase::BASE_INT16},
           {"uint16", EFieldBase::BASE_UINT16}, {"int32", EFieldBase::BASE_INT32},
           {"uint32", EFieldBase::BASE_UINT32}, {"int64", EFieldBase::BASE_INT64},
           {"uint64", EFieldBase::BASE_UINT64}, {"float32", EFieldBase::BASE_FLOAT32},
           {"float64", EFieldBase::BASE_FLOAT64}, {"date", EFieldBase::BASE_DATE},
           {"timestamp", EFieldBase::BASE_TIMESTAMP}, {"years", EFieldBase::BASE_YEARS},
           {"months", EFieldBase::BASE_MONTHS}, {"days", EFieldBase::BASE_DAYS},
           {"hours", EFieldBase::BASE_HOURS}, {"minutes", EFieldBase::BASE_MINS},
           {"seconds", EFieldBase::BASE_SECS}, {"milliseconds", EFieldBase::BASE_MILLISECS},
           {"microseconds", EFieldBase::BASE_MICROSECS}, {"nanoseconds", EFieldBase::BASE_NANOSECS},
           {"chars", EFieldBase::BASE_CHARS}, {"bytes", EFieldBase::BASE_BYTES},
           {"symbol", EFieldBase::BASE_SYMBOL}, {"string", EFieldBase::BASE_STRING}}, EFieldBase::BASE_NONE);
    }

    EnumerationNumber enumeration_value(const Table& table, EFieldBase base, const std::string& path)
    {
      const auto node = required_node(table, "value", path);
      const auto valuePath = path + ".value";
      const auto bits = scalar_width(base).value_or(0) * 8;
      const auto integer = integer_shape(base);
      const auto isSigned = integer && integer->_signed;
      std::int64_t signedValue{};
      std::uint64_t unsignedValue{};
      if (const auto value = node->as<std::int64_t>())
      {
        signedValue = value->get();
        if (!isSigned && signedValue < 0)
        {
          invalid(valuePath, "An unsigned enumerator cannot be negative");
        }
        unsignedValue = static_cast<std::uint64_t>(signedValue);
      }
      else if (const auto value = node->as<std::string>())
      {
        const auto& text = value->get();
        const auto first = !text.empty() && text.front() == '-' ? std::size_t{1} : std::size_t{};
        if (first == text.size() || (text[first] == '0' && (first != 0 || text.size() != 1)) ||
            !std::all_of(text.begin() + static_cast<std::ptrdiff_t>(first), text.end(),
                [](char character)
                {
                  return character >= '0' && character <= '9';
                }))
        {
          invalid(valuePath, "Expected a canonical decimal integer string without sign '+', leading zeros, or whitespace");
        }
        if (!isSigned && first != 0)
        {
          invalid(valuePath, "An unsigned enumerator cannot be negative");
        }
        const auto parsed = isSigned ? std::from_chars(text.data(), text.data() + text.size(), signedValue) :
                                       std::from_chars(text.data(), text.data() + text.size(), unsignedValue);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        {
          invalid(valuePath, "Enumerator does not fit its underlying integer range");
        }
      }
      else
      {
        invalid(valuePath, "Expected a TOML integer or canonical decimal integer string");
      }
      if (isSigned)
      {
        const auto maximum = bits == 64 ? std::numeric_limits<std::int64_t>::max() :
                                         (std::int64_t{1} << (bits - 1)) - 1;
        if (signedValue < -maximum - 1 || signedValue > maximum)
        {
          invalid(valuePath, "Enumerator does not fit its signed underlying integer range");
        }
        return signedValue;
      }
      const auto maximum = bits == 64 ? std::numeric_limits<std::uint64_t>::max() :
                                       (std::uint64_t{1} << bits) - 1;
      if (unsignedValue > maximum)
      {
        invalid(valuePath, "Enumerator does not fit its unsigned underlying integer range");
      }
      return unsignedValue;
    }

    Table parse_file(const std::filesystem::path& file)
    {
      if (file.empty() || file.native().find('\0') != std::string::npos)
      {
        invalid("configuration", "Configuration file needs a nonempty path without NUL");
      }
      std::ifstream input(file);
      if (!input)
      {
        runtime_fail(ErrorCode::IoError, file.string(), "Cannot open configuration file");
      }
      try
      {
        cpptoml::parser parser(input);
        auto root = parser.parse();
        if (input.bad())
        {
          runtime_fail(ErrorCode::IoError, file.string(), "Cannot read configuration file");
        }
        check_keys(root, "", {"config_version", "session", "limits", "defaults", "tasks", "fields",
                               "messages", "source_sets", "queues", "publishing_sets"});
        (void)unsigned_value(root, "config_version", "", 1, 2);
        return root;
      }
      catch (const cpptoml::parse_exception& error)
      {
        runtime_fail(ErrorCode::InvalidConfiguration, file.string(),
                     file.string() + ": " + error.what());
      }
    }

    SessionOptions parse_options(const Table& root, const std::filesystem::path& file)
    {
      const auto session = child_table(root, "session", "", true);
      check_keys(session, "session", {"name", "application_version", "directory", "max_log_bytes", "storage",
                                        "segment_bytes"});
      SessionOptions options;
      options._name = string_value(session, "name", "session");
      options._applicationVersion = string_value(session, "application_version", "session", false);
      const auto directory = string_value(session, "directory", "session");
      check_text(options._name, "session.name");
      check_text(options._applicationVersion, "session.application_version", false, false);
      check_text(directory, "session.directory");
      options._directory = std::filesystem::path{directory};
      if (options._directory.is_relative())
      {
        options._directory = std::filesystem::absolute(file).parent_path() / options._directory;
      }
      options._maxLogBytes = unsigned_value(session, "max_log_bytes", "session", 1, mapped::max_log_bytes);
      options._storage = enum_value<StorageMapping>(session, "storage", "session",
          {{"two_plane", StorageMapping::TwoPlane}, {"segmented", StorageMapping::Segmented}}, StorageMapping::TwoPlane);
      if (session->contains("segment_bytes"))
      {
        options._segmentBytes = unsigned_value(session, "segment_bytes", "session", 1, mapped::max_log_bytes);
      }
      else if (options._storage == StorageMapping::Segmented)
      {
        options._segmentBytes = 1048576;
      }
      if (options._storage == StorageMapping::TwoPlane && options._segmentBytes)
      {
        invalid("session.segment_bytes", "segment_bytes is only valid for segmented storage");
      }
      if (options._segmentBytes && (!mapped::valid_segment_bytes(*options._segmentBytes) ||
                                     *options._segmentBytes > options._maxLogBytes))
      {
        invalid("session.segment_bytes", "Segment bytes must be a multiple of 64, at least 4160, within max_log_bytes");
      }

      const auto limits = child_table(root, "limits", "", true);
      check_keys(limits, "limits", {"tasks", "queues", "source_sets", "publishing_sets", "memberships_per_set",
                                      "fields", "message_schemas", "symbols", "string_pool_bytes"});
      options._limits = CapacityLimits::empty();
      auto& capacity = options._limits;
      capacity._tasks = static_cast<std::uint32_t>(unsigned_value(limits, "tasks", "limits", 0, mapped::max_entity_id));
      capacity._queues = static_cast<std::uint32_t>(unsigned_value(limits, "queues", "limits", 0, mapped::max_entity_id));
      capacity._sourceSets = static_cast<std::uint32_t>(unsigned_value(limits, "source_sets", "limits", 0, mapped::max_entity_id));
      capacity._publishingSets = static_cast<std::uint32_t>(unsigned_value(limits, "publishing_sets", "limits", 0, mapped::max_entity_id));
      capacity._membershipsPerSet = static_cast<std::uint32_t>(unsigned_value(limits, "memberships_per_set", "limits", 0, mapped::max_entity_id));
      capacity._fields = static_cast<std::uint32_t>(unsigned_value(limits, "fields", "limits", 0, mapped::max_fields));
      capacity._messageSchemas = static_cast<std::uint32_t>(unsigned_value(limits, "message_schemas", "limits", 0,
                                                                            std::numeric_limits<std::uint32_t>::max()));
      capacity._symbols = static_cast<std::uint32_t>(unsigned_value(limits, "symbols", "limits", 0,
                                                                     std::numeric_limits<std::uint32_t>::max()));
      capacity._stringPoolBytes = unsigned_value(limits, "string_pool_bytes", "limits", 0, mapped::max_log_bytes);
      if (std::uint64_t{capacity._sourceSets} + capacity._publishingSets > mapped::max_entity_id)
      {
        invalid("limits.publishing_sets", "Source and Publishing capacities together exceed 65535 Queue Set IDs");
      }
      if (capacity._tasks == 0)
      {
        invalid("limits.tasks", "A Session must reserve room for its creator Task");
      }
      if (!mapped::make_fixed_layout(capacity))
      {
        invalid("limits", "The combined capacity reservations exceed the format's byte-address range");
      }

      const auto defaults = child_table(root, "defaults", "");
      check_keys(defaults, "defaults", {"queue", "source_set", "publishing_set"});
      const auto queue = child_table(defaults, "queue", "defaults");
      check_keys(queue, "defaults.queue", {"ring_capacity", "delivery"});
      if (options._storage == StorageMapping::Segmented && queue->contains("ring_capacity"))
      {
        invalid("defaults.queue.ring_capacity", "An explicit ring_capacity requires two_plane storage");
      }
      options._defaults._ringCapacity = unsigned_value(queue, "ring_capacity", "defaults.queue", 1,
                                                          mapped::max_log_bytes, options._defaults._ringCapacity);
      if (!mapped::ring_storage_bytes(options._defaults._ringCapacity))
      {
        invalid("defaults.queue.ring_capacity", "Ring geometry exceeds format bounds");
      }
      options._defaults._delivery = enum_value<DeliveryPolicy>(queue, "delivery", "defaults.queue",
          {{"recover_from_log", DeliveryPolicy::RecoverFromLog}, {"allow_loss", DeliveryPolicy::AllowLoss}},
          options._defaults._delivery);
      const auto source = child_table(defaults, "source_set", "defaults");
      check_keys(source, "defaults.source_set", {"selection", "wait"});
      options._defaults._selection = enum_value<SelectionPolicy>(source, "selection", "defaults.source_set",
          {{"strict", SelectionPolicy::Strict}, {"fair", SelectionPolicy::Fair},
           {"weighted_random", SelectionPolicy::WeightedRandom}}, options._defaults._selection);
      options._defaults._wait = enum_value<WaitPolicy>(source, "wait", "defaults.source_set",
          {{"spin", WaitPolicy::Spin}, {"eventfd", WaitPolicy::EventFd}}, options._defaults._wait);
      const auto publishing = child_table(defaults, "publishing_set", "defaults");
      check_keys(publishing, "defaults.publishing_set", {"acceptance"});
      options._defaults._acceptance = enum_value<AcceptancePolicy>(publishing, "acceptance", "defaults.publishing_set",
          {{"all_destinations", AcceptancePolicy::AllDestinations}, {"partial", AcceptancePolicy::Partial}},
          options._defaults._acceptance);
      // Preserve the precise TOML key for the current support boundary while
      // sharing the authoritative programmatic option validator below.
      try
      {
        runtime_validate_options(options, ErrorCode::InvalidConfiguration);
      }
      catch (const RuntimeFailure& failure)
      {
        auto error = failure._error;
        if (error._code == ErrorCode::UnsupportedPolicy)
        {
          if (options._storage != StorageMapping::TwoPlane)
          {
            error._path = "session.storage";
          }
          else if (options._defaults._selection != SelectionPolicy::Strict)
          {
            error._path = "defaults.source_set.selection";
          }
          else if (options._defaults._wait != WaitPolicy::Spin)
          {
            error._path = "defaults.source_set.wait";
          }
          else
          {
            error._path = "defaults.publishing_set.acceptance";
          }
        }
        throw RuntimeFailure{std::move(error)};
      }
      return options;
    }

    struct TaskReference
    {
      TaskId _id{};
      TaskKind _kind{};
    };

    struct SourceReference
    {
      SourceQueueSetId _id{};
      SelectionPolicy _selection{};
    };

    struct QueueReference
    {
      QueueId _id{};
      std::string _producer;
    };

    class PlanParser
    {
    public:
      PlanParser(Table root, SessionOptions options, const Registry* existing, bool allowLoss)
          : _root(std::move(root)), _existing(existing), _allowLoss(allowLoss)
      {
        _plan._options = std::move(options);
        if (_plan._options._defaults._delivery == DeliveryPolicy::AllowLoss && !_allowLoss)
        {
          runtime_fail(ErrorCode::UnsupportedPolicy, "defaults.queue.delivery",
                       "Allowed-loss defaults require the Session's negotiated capability");
        }
      }

      ConfigurationPlan parse(std::string_view creator)
      {
        parse_tasks(creator);
        parse_fields();
        parse_messages();
        parse_sources();
        parse_queues();
        parse_publishing();
        return std::move(_plan);
      }

    private:
      template <typename T>
      std::optional<T> found(Result<T> result, const std::string& path)
      {
        if (const auto* value = std::get_if<T>(&result))
        {
          return *value;
        }
        const auto& error = std::get<Error>(result);
        if (error._code == ErrorCode::NotFound)
        {
          return {};
        }
        runtime_fail(error._code, path, error._message);
      }

      TaskReference task_reference(const std::string& name, const std::string& path)
      {
        check_text(name, path);
        if (const auto declared = _tasks.find(name); declared != _tasks.end())
        {
          return declared->second;
        }
        if (_existing)
        {
          if (const auto id = found(_existing->find_task(name), path))
          {
            const auto descriptor = runtime_value(_existing->describe_task(*id));
            check_count(_tasks.size() + 1, mapped::max_entity_id, path, "Referenced Task names");
            const TaskReference reference{static_cast<TaskId>(_tasks.size() + 1), descriptor._kind};
            _tasks.emplace(name, reference);
            return reference;
          }
        }
        invalid(path, "Task reference '" + name + "' does not resolve");
      }

      SourceReference source_reference(const std::string& name, const std::string& path)
      {
        check_text(name, path);
        if (const auto declared = _sources.find(name); declared != _sources.end())
        {
          return declared->second;
        }
        if (_existing)
        {
          if (const auto id = found(_existing->find_source_set(name), path))
          {
            const auto descriptor = runtime_value(_existing->describe_source_set(*id));
            check_count(_sources.size() + 1, mapped::max_entity_id, path, "Referenced Source Queue Set names");
            const SourceReference reference{static_cast<SourceQueueSetId>(_sources.size() + 1),
                descriptor._selection.value_or(_plan._options._defaults._selection)};
            _sources.emplace(name, reference);
            return reference;
          }
        }
        invalid(path, "Source Queue Set reference '" + name + "' does not resolve");
      }

      QueueReference queue_reference(const std::string& name, const std::string& path)
      {
        check_text(name, path);
        if (const auto declared = _queues.find(name); declared != _queues.end())
        {
          return declared->second;
        }
        if (_existing)
        {
          if (const auto id = found(_existing->find_queue(name), path))
          {
            const auto descriptor = runtime_value(_existing->describe_queue(*id));
            if (descriptor._producer == TaskId::Invalid)
            {
              invalid(path, "Destination '" + name + "' has no producer owner");
            }
            const auto producer = runtime_value(_existing->describe_task(descriptor._producer));
            check_count(_queues.size() + 1, mapped::max_entity_id, path, "Referenced Queue names");
            const QueueReference reference{static_cast<QueueId>(_queues.size() + 1), producer._name};
            _queues.emplace(name, reference);
            return reference;
          }
        }
        invalid(path, "Queue reference '" + name + "' does not resolve");
      }

      FieldId field_reference(const std::string& name, const std::string& path)
      {
        check_text(name, path);
        if (const auto declared = _fields.find(name); declared != _fields.end())
        {
          return declared->second;
        }
        if (_existing)
        {
          if (const auto id = found(_existing->find_field(name), path))
          {
            return *id;
          }
        }
        invalid(path, "Field reference '" + name + "' does not resolve");
      }

      void parse_tasks(std::string_view creator)
      {
        const auto tables = declaration_tables(_root, "tasks");
        check_count(tables.size(), _plan._options._limits._tasks, "limits.tasks", "Task declarations");
        std::vector<ConfigurationEntry<TaskDescriptor>> entries;
        std::vector<std::optional<std::string>> parents;
        std::set<std::string> names;
        std::optional<std::size_t> creatorIndex;
        for (std::size_t index = 0; index < tables.size(); ++index)
        {
          const auto& table = tables[index];
          const auto path = indexed_path("tasks", index);
          check_keys(table, path, {"name", "kind", "process", "doc"});
          TaskDescriptor task;
          task._name = string_value(table, "name", path);
          check_text(task._name, path + ".name");
          if (!names.insert(task._name).second)
          {
            invalid(path + ".name", "Duplicate Task name '" + task._name + "'");
          }
          task._kind = enum_value<TaskKind>(table, "kind", path,
              {{"process", TaskKind::Process}, {"thread", TaskKind::Thread}}, TaskKind::Process);
          task._doc = string_value(table, "doc", path, false);
          check_text(task._doc, path + ".doc", false, false);
          std::optional<std::string> parent;
          if (task._kind == TaskKind::Thread)
          {
            parent = string_value(table, "process", path);
            check_text(*parent, path + ".process");
          }
          else if (table->contains("process"))
          {
            invalid(path + ".process", "A Process Task must omit process");
          }
          if ((!creator.empty() && task._name == creator) ||
              (creator.empty() && !creatorIndex && task._kind == TaskKind::Process))
          {
            creatorIndex = index;
          }
          entries.push_back({std::move(task), path});
          parents.push_back(std::move(parent));
        }
        if (!creator.empty() && !creatorIndex)
        {
          invalid("creator", "Creator '" + std::string{creator} + "' must name a declared Process Task");
        }
        if (creatorIndex && entries[*creatorIndex]._descriptor._kind != TaskKind::Process)
        {
          invalid("creator", "Creator '" + std::string{creator} + "' at " +
                  entries[*creatorIndex]._path + " must be a Process Task");
        }
        std::vector<std::size_t> order;
        if (creatorIndex)
        {
          order.push_back(*creatorIndex);
        }
        for (const auto kind : {TaskKind::Process, TaskKind::Thread})
        {
          for (std::size_t index = 0; index < entries.size(); ++index)
          {
            if ((!creatorIndex || index != *creatorIndex) && entries[index]._descriptor._kind == kind)
            {
              order.push_back(index);
            }
          }
        }
        for (std::size_t position = 0; position < order.size(); ++position)
        {
          const auto& task = entries[order[position]]._descriptor;
          const auto id = static_cast<TaskId>(position + 1);
          _tasks.emplace(task._name, TaskReference{id, task._kind});
        }
        for (const auto index : order)
        {
          auto& entry = entries[index];
          if (parents[index])
          {
            const auto parent = task_reference(*parents[index], entry._path + ".process");
            if (parent._kind != TaskKind::Process)
            {
              invalid(entry._path + ".process", "Parent '" + *parents[index] + "' is not a Process Task");
            }
            entry._descriptor._process = parent._id;
          }
          at_path(entry._path, [&]
          {
            runtime_value(mapped::validate_task_descriptor(entry._descriptor));
          });
          _plan._tasks.push_back(std::move(entry));
        }
      }

      void parse_enumerators(const Table& table, FieldDescriptor& descriptor, const std::string& path,
                               std::string_view fieldName, std::string_view fieldDoc)
      {
        const auto tables = declaration_tables(table, "enumerators", path);
        if (tables.empty())
        {
          invalid(path + ".enumerators", "An enumeration needs at least one enumerator");
        }
        // The parser already owns the input. Borrow its strings until the whole
        // Field and its minimum append allocation have passed byte preflight.
        const auto arrayBytes = mapped::checked_multiply(tables.size(), 24);
        if (!arrayBytes || *arrayBytes > _plan._options._maxLogBytes)
        {
          runtime_fail(ErrorCode::LogFull, "session.max_log_bytes", "Enumerator array cannot fit the log while planning " + path);
        }
        auto& entries = descriptor._logical._enumerators;
        if (tables.size() > entries.max_size())
        {
          runtime_fail(ErrorCode::CapacityExceeded, path + ".enumerators", "Enumerator array exceeds the native allocation range");
        }
        const auto add_metadata_bytes = [&path](std::uint64_t left, std::uint64_t right)
        {
          const auto sum = mapped::checked_add(left, right);
          if (!sum || *sum > mapped::max_log_bytes)
          {
            runtime_fail(ErrorCode::LogFull, "session.max_log_bytes",
                         "Enumeration metadata length overflows while planning " + path);
          }
          return *sum;
        };
        auto bodyBytes = add_metadata_bytes(88 + 16, *arrayBytes);
        bodyBytes = add_metadata_bytes(bodyBytes, fieldName.size());
        bodyBytes = add_metadata_bytes(bodyBytes, fieldDoc.size());
        for (std::size_t index = 0; index < tables.size(); ++index)
        {
          const auto entryPath = path + ".enumerators[" + std::to_string(index) + "]";
          const auto& entryTable = tables[index];
          check_keys(entryTable, entryPath, {"name", "value"});
          const auto label = borrowed_string_value(entryTable, "name", entryPath);
          check_text(label, entryPath + ".name");
          (void)enumeration_value(entryTable, descriptor._base, entryPath);
          bodyBytes = add_metadata_bytes(bodyBytes, label.size());
        }
        if (bodyBytes > std::vector<std::byte>{}.max_size() || !fits_host_size(bodyBytes))
        {
          runtime_fail(ErrorCode::CapacityExceeded, path + ".enumerators",
                       "Enumeration metadata exceeds the native allocation range");
        }
        const auto layout = mapped::make_fixed_layout(_plan._options._limits);
        if (!layout)
        {
          invalid("limits", "The combined capacity reservations exceed the format's byte-address range");
        }
        const std::array bodies{bodyBytes};
        try
        {
          // Starting at the empty arena is a lower bound. The complete initial
          // configuration, including bootstrap and earlier definitions, still
          // receives its ordinary cumulative preflight before publication.
          (void)append_tail(layout->_arenaBegin, bodies, 48, _plan._options._maxLogBytes);
        }
        catch (const RuntimeFailure& failure)
        {
          auto error = failure._error;
          error._message += " while planning " + path + ".enumerators";
          throw RuntimeFailure{std::move(error)};
        }
        entries.reserve(tables.size());
        // Reservation keeps borrowed name views valid while entries are built.
        std::set<std::string_view> names;
        std::set<EnumerationNumber> numbers;
        for (std::size_t index = 0; index < tables.size(); ++index)
        {
          const auto entryPath = path + ".enumerators[" + std::to_string(index) + "]";
          const auto& entryTable = tables[index];
          check_keys(entryTable, entryPath, {"name", "value"});
          EnumeratorDescriptor entry;
          entry._name = string_value(entryTable, "name", entryPath);
          check_text(entry._name, entryPath + ".name");
          entry._value = enumeration_value(entryTable, descriptor._base, entryPath);
          entries.push_back(std::move(entry));
          if (!names.insert(entries.back()._name).second)
          {
            invalid(entryPath + ".name", "Duplicate enumerator name '" + entries.back()._name + "'");
          }
          if (!numbers.insert(entries.back()._value).second)
          {
            invalid(entryPath + ".value", "Duplicate enumerator numeric value");
          }
        }
      }

      void parse_fields()
      {
        const auto tables = declaration_tables(_root, "fields");
        check_count(tables.size(), _plan._options._limits._fields, "limits.fields", "Field declarations");
        const auto version = unsigned_value(_root, "config_version", "", 1, 2);
        std::set<FieldId> identities;
        for (std::size_t index = 0; index < tables.size(); ++index)
        {
          const auto& table = tables[index];
          const auto path = indexed_path("fields", index);
          const auto type = string_value(table, "type", path);
          if (version == 1)
          {
            for (const auto* key : {"bits", "size", "underlying", "enumerators"})
            {
              if (table->contains(key))
              {
                invalid(path + "." + key, "This Field key requires config_version = 2");
              }
            }
            if (type == "bool" || type == "bits" || type == "enum" || type == "int8")
            {
              invalid(path + ".type", "Field type '" + type + "' requires config_version = 2");
            }
          }
          if (version == 2 && type == "bits")
          {
            check_keys(table, path, {"name", "type", "id", "doc", "bits"});
          }
          else if (version == 2 && type == "enum")
          {
            check_keys(table, path, {"name", "type", "id", "doc", "underlying", "enumerators"});
          }
          else if (version == 2 && type == "chars")
          {
            check_keys(table, path, {"name", "type", "id", "doc", "size"});
          }
          else
          {
            check_keys(table, path, {"name", "type", "id", "doc"});
          }
          FieldDescriptor descriptor;
          const auto fieldName = borrowed_string_value(table, "name", path);
          const auto fieldDoc = borrowed_string_value(table, "doc", path, false);
          check_text(fieldName, path + ".name");
          check_text(fieldDoc, path + ".doc", false, false);
          descriptor._id = static_cast<FieldId>(unsigned_value(table, "id", path, 0,
                                                                 std::numeric_limits<std::uint32_t>::max()));
          if (!valid_field_id(descriptor._id))
          {
            invalid(path + ".id", "Field identity " + std::to_string(static_cast<std::uint32_t>(descriptor._id)) +
                    " must retain the 0xEE high byte");
          }
          if (type == "bool")
          {
            descriptor._base = EFieldBase::BASE_BYTE;
            descriptor._size = 1;
            descriptor._logical._kind = FieldLogicalKind::Boolean;
          }
          else if (type == "bits")
          {
            const auto bits = unsigned_value(table, "bits", path, 8, 64);
            switch (bits)
            {
            case 8: descriptor._base = EFieldBase::BASE_BYTE; break;
            case 16: descriptor._base = EFieldBase::BASE_UINT16; break;
            case 32: descriptor._base = EFieldBase::BASE_UINT32; break;
            case 64: descriptor._base = EFieldBase::BASE_UINT64; break;
            default: invalid(path + ".bits", "Bit-field width must be exactly 8, 16, 32, or 64");
            }
            descriptor._size = static_cast<std::uint32_t>(bits / 8);
            descriptor._logical._kind = FieldLogicalKind::Bits;
          }
          else if (type == "enum")
          {
            descriptor._base = field_base(table, "underlying", path);
            if (!integer_shape(descriptor._base))
            {
              invalid(path + ".underlying", "Enum storage must be byte, int8, int16, uint16, int32, uint32, int64, or uint64");
            }
            descriptor._size = scalar_width(descriptor._base).value_or(0);
            descriptor._logical._kind = FieldLogicalKind::Enumeration;
            parse_enumerators(table, descriptor, path, fieldName, fieldDoc);
          }
          else if (version == 2 && type == "chars")
          {
            descriptor._base = EFieldBase::BASE_CHARS;
            descriptor._size = static_cast<std::uint32_t>(unsigned_value(table, "size", path, 1,
                std::numeric_limits<std::uint32_t>::max()));
            descriptor._logical._kind = FieldLogicalKind::FixedAscii;
          }
          else
          {
            descriptor._base = field_base(table, "type", path);
            descriptor._size = scalar_width(descriptor._base).value_or(0);
          }
          descriptor._name = fieldName;
          descriptor._doc = fieldDoc;
          at_path(path, [&]
          {
            descriptor = runtime_value(mapped::canonicalize_field_descriptor(descriptor));
            runtime_validate_field_policy(descriptor, ErrorCode::InvalidConfiguration);
          });
          if (!_fields.emplace(descriptor._name, descriptor._id).second)
          {
            invalid(path + ".name", "Duplicate Field name '" + descriptor._name + "'");
          }
          if (!identities.insert(descriptor._id).second)
          {
            invalid(path + ".id", "Duplicate Field identity " +
                    std::to_string(static_cast<std::uint32_t>(descriptor._id)));
          }
          _plan._fields.push_back({std::move(descriptor), path});
        }
      }

      void parse_messages()
      {
        const auto tables = declaration_tables(_root, "messages");
        check_count(tables.size(), _plan._options._limits._messageSchemas, "limits.message_schemas", "Message schema declarations");
        std::map<MessageType, std::string> names;
        std::map<std::string, MessageType> types;
        std::set<std::pair<MessageType, std::uint32_t>> identities;
        for (std::size_t index = 0; index < tables.size(); ++index)
        {
          const auto& table = tables[index];
          const auto path = indexed_path("messages", index);
          check_keys(table, path, {"name", "type", "version", "fields", "doc"});
          MessageDescriptor descriptor;
          descriptor._name = string_value(table, "name", path);
          descriptor._doc = string_value(table, "doc", path, false);
          descriptor._schema._type = static_cast<MessageType>(unsigned_value(table, "type", path, 1, mapped::max_entity_id));
          descriptor._schema._version = static_cast<std::uint32_t>(unsigned_value(table, "version", path, 1,
                                                                                    std::numeric_limits<std::uint32_t>::max()));
          const auto fields = string_array(table, "fields", path);
          for (std::size_t field = 0; field < fields.size(); ++field)
          {
            descriptor._fields.push_back(field_reference(fields[field], path + ".fields[" + std::to_string(field) + "]"));
          }
          at_path(path, [&]
          {
            runtime_value(mapped::validate_message_descriptor(descriptor));
          });
          if (const auto name = names.find(descriptor._schema._type); name != names.end() && name->second != descriptor._name)
          {
            invalid(path + ".name", "Message type already belongs to name '" + name->second + "'");
          }
          if (const auto type = types.find(descriptor._name); type != types.end() && type->second != descriptor._schema._type)
          {
            invalid(path + ".type", "Message name '" + descriptor._name + "' already has a different type");
          }
          if (!identities.emplace(descriptor._schema._type, descriptor._schema._version).second)
          {
            invalid(path + ".version", "Duplicate message type/version declaration");
          }
          names.emplace(descriptor._schema._type, descriptor._name);
          types.emplace(descriptor._name, descriptor._schema._type);
          _plan._messages.push_back({std::move(descriptor), path});
        }
      }

      void parse_sources()
      {
        const auto tables = declaration_tables(_root, "source_sets");
        check_count(tables.size(), _plan._options._limits._sourceSets, "limits.source_sets", "Source Queue Set declarations");
        for (std::size_t index = 0; index < tables.size(); ++index)
        {
          const auto& table = tables[index];
          const auto path = indexed_path("source_sets", index);
          check_keys(table, path, {"name", "owner", "selection", "wait", "random_seed", "doc"});
          SourceQueueSetDescriptor descriptor;
          descriptor._name = string_value(table, "name", path);
          descriptor._doc = string_value(table, "doc", path, false);
          descriptor._owner = task_reference(string_value(table, "owner", path), path + ".owner")._id;
          if (!_setNames.insert(descriptor._name).second)
          {
            invalid(path + ".name", "Duplicate Queue Set name '" + descriptor._name + "'");
          }
          if (table->contains("selection"))
          {
            descriptor._selection = enum_value<SelectionPolicy>(table, "selection", path,
                {{"strict", SelectionPolicy::Strict}, {"fair", SelectionPolicy::Fair},
                 {"weighted_random", SelectionPolicy::WeightedRandom}}, SelectionPolicy::Strict);
          }
          if (table->contains("wait"))
          {
            descriptor._wait = enum_value<WaitPolicy>(table, "wait", path,
                {{"spin", WaitPolicy::Spin}, {"eventfd", WaitPolicy::EventFd}}, WaitPolicy::Spin);
          }
          if (table->contains("random_seed"))
          {
            descriptor._randomSeed = unsigned_value(table, "random_seed", path, 0, mapped::max_log_bytes);
          }
          SourcePolicy policy;
          try
          {
            at_path(path, [&]
            {
              runtime_value(mapped::validate_source_set_descriptor(descriptor));
              policy = resolve_source(descriptor, _plan._options);
            });
          }
          catch (const RuntimeFailure& failure)
          {
            auto error = failure._error;
            if (error._code == ErrorCode::UnsupportedPolicy && error._path == path + ".policy")
            {
              error._path = path +
                  (descriptor._selection.value_or(_plan._options._defaults._selection) != SelectionPolicy::Strict ?
                   ".selection" : ".wait");
            }
            throw RuntimeFailure{std::move(error)};
          }
          // Give unsupported Source choices their actual TOML key, rather than
          // the shared validator's combined programmatic policy path.
          descriptor._selection = policy._selection;
          descriptor._wait = policy._wait;
          const auto id = static_cast<SourceQueueSetId>(index + 1);
          _sources.emplace(descriptor._name, SourceReference{id, policy._selection});
          _plan._sources.push_back({std::move(descriptor), path});
        }
      }

      void parse_queues()
      {
        const auto tables = declaration_tables(_root, "queues");
        check_count(tables.size(), _plan._options._limits._queues, "limits.queues", "Queue declarations");
        std::map<std::string, std::uint64_t> members;
        for (std::size_t index = 0; index < tables.size(); ++index)
        {
          const auto& table = tables[index];
          const auto path = indexed_path("queues", index);
          check_keys(table, path, {"name", "producer", "source_set", "priority", "ring_capacity", "delivery", "doc"});
          QueueDescriptor descriptor;
          descriptor._name = string_value(table, "name", path);
          descriptor._doc = string_value(table, "doc", path, false);
          const auto producer = string_value(table, "producer", path);
          descriptor._producer = task_reference(producer, path + ".producer")._id;
          const auto sourceName = string_value(table, "source_set", path);
          const auto source = source_reference(sourceName, path + ".source_set");
          descriptor._sourceSet = source._id;
          descriptor._priority = static_cast<std::uint8_t>(unsigned_value(table, "priority", path,
              source._selection == SelectionPolicy::Strict ? 0 : 1, 255, 1));
          if (table->contains("ring_capacity"))
          {
            if (_plan._options._storage != StorageMapping::TwoPlane)
            {
              invalid(path + ".ring_capacity", "An explicit ring_capacity requires two_plane storage");
            }
            descriptor._ringCapacity = unsigned_value(table, "ring_capacity", path, 1, mapped::max_log_bytes);
          }
          if (table->contains("delivery"))
          {
            descriptor._delivery = enum_value<DeliveryPolicy>(table, "delivery", path,
                {{"recover_from_log", DeliveryPolicy::RecoverFromLog}, {"allow_loss", DeliveryPolicy::AllowLoss}},
                DeliveryPolicy::RecoverFromLog);
          }
          at_path(path, [&]
          {
            runtime_value(mapped::validate_queue_descriptor(descriptor));
            const auto policy = resolve_queue(descriptor, _plan._options, _allowLoss);
            descriptor._ringCapacity = policy._capacity;
            descriptor._delivery = policy._delivery;
          });
          check_count(++members[sourceName], _plan._options._limits._membershipsPerSet,
                          "limits.memberships_per_set", "Source '" + sourceName + "' membership");
          const auto id = static_cast<QueueId>(index + 1);
          if (!_queues.emplace(descriptor._name, QueueReference{id, producer}).second)
          {
            invalid(path + ".name", "Duplicate Queue name '" + descriptor._name + "'");
          }
          _plan._queues.push_back({std::move(descriptor), path});
        }
      }

      void parse_publishing()
      {
        const auto tables = declaration_tables(_root, "publishing_sets");
        check_count(tables.size(), _plan._options._limits._publishingSets, "limits.publishing_sets", "Publishing Queue Set declarations");
        for (std::size_t index = 0; index < tables.size(); ++index)
        {
          const auto& table = tables[index];
          const auto path = indexed_path("publishing_sets", index);
          check_keys(table, path, {"name", "owner", "destinations", "acceptance", "doc"});
          PublishingQueueSetDescriptor descriptor;
          descriptor._name = string_value(table, "name", path);
          descriptor._doc = string_value(table, "doc", path, false);
          const auto owner = string_value(table, "owner", path);
          descriptor._owner = task_reference(owner, path + ".owner")._id;
          if (!_setNames.insert(descriptor._name).second)
          {
            invalid(path + ".name", "Duplicate Queue Set name '" + descriptor._name + "'");
          }
          const auto destinations = string_array(table, "destinations", path);
          check_count(destinations.size(), _plan._options._limits._membershipsPerSet,
                          "limits.memberships_per_set", "Publishing set '" + descriptor._name + "' membership");
          std::set<std::string> unique;
          for (std::size_t destination = 0; destination < destinations.size(); ++destination)
          {
            const auto destinationPath = path + ".destinations[" + std::to_string(destination) + "]";
            if (!unique.insert(destinations[destination]).second)
            {
              invalid(destinationPath, "Duplicate destination '" + destinations[destination] + "'");
            }
            const auto queue = queue_reference(destinations[destination], destinationPath);
            if (queue._producer != owner)
            {
              invalid(destinationPath, "Destination '" + destinations[destination] + "' belongs to producer '" +
                                      queue._producer + "', not '" + owner + "'");
            }
            descriptor._destinations.push_back(queue._id);
          }
          if (table->contains("acceptance"))
          {
            descriptor._acceptance = enum_value<AcceptancePolicy>(table, "acceptance", path,
                {{"all_destinations", AcceptancePolicy::AllDestinations}, {"partial", AcceptancePolicy::Partial}},
                AcceptancePolicy::AllDestinations);
          }
          at_path(path, [&]
          {
            runtime_value(mapped::validate_publishing_set_descriptor(descriptor));
            descriptor._acceptance = resolve_publishing(descriptor, _plan._options);
          });
          _plan._publishing.push_back({std::move(descriptor), path});
        }
      }

      Table _root;
      const Registry* _existing{};
      bool _allowLoss{};
      ConfigurationPlan _plan;
      std::map<std::string, TaskReference> _tasks;
      std::map<std::string, FieldId> _fields;
      std::map<std::string, SourceReference> _sources;
      std::map<std::string, QueueReference> _queues;
      std::set<std::string> _setNames;
    };
  } // namespace

  Result<SessionOptions> load_configuration_options(const std::filesystem::path& file)
  {
    try
    {
      return parse_options(parse_file(file), file);
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<ConfigurationPlan> load_configuration(const std::filesystem::path& file,
                                                std::string_view creator, const Registry* existing,
                                                bool allowLoss)
  {
    try
    {
      auto root = parse_file(file);
      auto options = parse_options(root, file);
      return PlanParser{std::move(root), std::move(options), existing, allowLoss}.parse(creator);
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }
} // namespace angaria::detail
