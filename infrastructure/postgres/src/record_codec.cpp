#include "record_codec.hpp"

#include <chrono>
#include <stdexcept>
#include <type_traits>

namespace kiarabot::infrastructure::postgres::detail {
namespace target = core::target;
static_assert(std::is_same_v<target::TimePoint::duration, std::chrono::nanoseconds>);
namespace {

template <typename Enum> Enum enum_field(const pqxx::field& field, int maximum) {
  const auto value = field.as<int>();
  if (value < 0 || value > maximum) {
    throw std::runtime_error("invalid stored enumeration");
  }
  return static_cast<Enum>(value);
}
std::optional<int> optional_enum(const auto& value) {
  return value ? std::optional<int>(static_cast<int>(*value)) : std::nullopt;
}
std::optional<std::string> optional_timestamp(const std::optional<target::TimePoint>& value) {
  return value ? std::optional<std::string>(timestamp(*value)) : std::nullopt;
}
std::string marks(std::size_t count, std::size_t offset = 1) {
  std::string result;
  for (std::size_t index = 0; index < count; ++index) {
    result += (index ? ",$" : "$") + std::to_string(offset + index);
  }
  return result;
}
void put_snapshot(pqxx::transaction_base& transaction, std::string_view table,
                  std::string_view columns, std::int64_t id, pqxx::params values) {
  pqxx::params parameters{id};
  parameters.append(std::move(values));
  transaction.exec("INSERT INTO kiarabot.result_" + std::string(table) + "(result_id," +
                       std::string(columns) + ") VALUES(" + marks(parameters.size()) + ")",
                   parameters);
}
pqxx::row get_snapshot(pqxx::transaction_base& transaction, std::string_view table, std::int64_t id,
                       std::string_view projection = "*") {
  return transaction
      .exec("SELECT " + std::string(projection) + " FROM kiarabot.result_" + std::string(table) +
                " WHERE result_id=$1",
            pqxx::params{id})
      .one_row();
}
} // namespace

bool valid_sql_text(std::string_view value) noexcept {
  return value.find('\0') == std::string_view::npos;
}
bool valid_sql_text_fields(std::initializer_list<std::string_view> values) noexcept {
  for (const auto value : values) {
    if (!valid_sql_text(value)) {
      return false;
    }
  }
  return true;
}
bool valid_text(const target::Operation& value) noexcept {
  return valid_sql_text_fields({value.ref.id.value, value.ref.resource.id.value});
}
bool valid_text(const target::HandoffContext& value) noexcept {
  return valid_sql_text_fields(
      {value.message.value, value.operation.id.value, value.operation.resource.id.value,
       value.resource.id.value, value.actor.value,
       value.causation ? std::string_view(value.causation->value) : std::string_view{}});
}
bool valid_text(const target::ReplayResult& result) noexcept {
  if (result.valueless_by_exception()) {
    return false;
  }
  return std::visit(
      [](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, target::EmptySuccess>) {
          return true;
        } else if constexpr (std::is_same_v<Value, target::ResourceMutationResult>) {
          return valid_sql_text(value.resource.id.value);
        } else if constexpr (std::is_same_v<Value, target::Operation>) {
          return valid_text(value);
        } else if constexpr (std::is_same_v<Value, target::Principal>) {
          return valid_sql_text(value.id.value) &&
                 (!value.binding || valid_sql_text(value.binding->id.value));
        } else if constexpr ((std::is_same_v<Value, target::LoginSession> ||
                              std::is_same_v<Value, target::SessionMetadata>)) {
          return valid_sql_text_fields({value.id.value, value.subject.value});
        } else if constexpr (std::is_same_v<Value, target::ResourceGrant>) {
          if (!valid_sql_text_fields({value.id.value, value.subject.value})) {
            return false;
          }
          for (const auto& scope : value.scopes) {
            if (!valid_sql_text_fields({scope.resource.id.value, scope.permission})) {
              return false;
            }
          }
          return true;
        } else if constexpr (std::is_same_v<Value, target::BotAccount>) {
          return valid_sql_text_fields(
              {value.id.value, value.platform, value.platform_account_id, value.display_name});
        } else if constexpr (std::is_same_v<Value, target::PluginInstallation>) {
          return valid_sql_text_fields({value.id.value, value.plugin_type.value});
        } else if constexpr (std::is_same_v<Value, target::PluginInstance>) {
          if (!valid_sql_text_fields(
                  {value.id.value, value.installation.value, value.plugin_type.value}) ||
              (value.host && !valid_sql_text(value.host->value))) {
            return false;
          }
          if (!value.current) {
            return true;
          }
          const auto& fence = *value.current;
          return valid_sql_text_fields({fence.instance.value, fence.runtime_epoch.value,
                                        fence.execution.value, fence.connection.value});
        } else if constexpr (std::is_same_v<Value, target::ScheduledTask>) {
          return valid_sql_text_fields({value.key.owner.value, value.key.task_id,
                                        value.installation.value, value.target.adapter.value,
                                        value.target.account.value, value.target.conversation_id,
                                        value.occurrence.value});
        } else if constexpr (std::is_same_v<Value, target::Occurrence>) {
          return valid_sql_text_fields({value.owner.value, value.installation.value, value.task_id,
                                        value.occurrence_id.value}) &&
                 (!value.accepted_event_id || valid_sql_text(*value.accepted_event_id)) &&
                 valid_text(value.handoff);
        } else {
          static_assert(std::is_same_v<Value, target::ModelIntent>);
          return valid_sql_text_fields(
              {value.owner.value, value.logical_call_id, value.model.value});
        }
      },
      result);
}

std::string timestamp(target::TimePoint value) {
  return std::to_string(
      std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
}
std::string time_projection(std::string_view column) {
  return std::string(column);
}
target::TimePoint time_field(const pqxx::row& row, const char* column) {
  return target::TimePoint(std::chrono::duration_cast<target::TimePoint::duration>(
      std::chrono::nanoseconds(row[column].as<std::int64_t>())));
}
std::optional<target::TimePoint> optional_time(const pqxx::row& row, const char* column) {
  return row[column].is_null() ? std::nullopt
                               : std::optional<target::TimePoint>(time_field(row, column));
}
std::string digest(const target::Sha256Digest& value) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string result = "\\x";
  for (const auto byte : value.bytes) {
    const auto number = std::to_integer<unsigned>(byte);
    result += kDigits[number >> 4];
    result += kDigits[number & 15];
  }
  return result;
}
target::Sha256Digest read_digest(const pqxx::field& field) {
  const auto value = field.as<pqxx::bytes>();
  if (value.size() != 32) {
    throw std::runtime_error("invalid stored digest");
  }
  target::Sha256Digest result;
  for (std::size_t index = 0; index < result.bytes.size(); ++index) {
    result.bytes[index] = static_cast<std::byte>(value[index]);
  }
  return result;
}
target::Operation read_operation(const pqxx::row& row) {
  target::Operation value;
  value.ref = {enum_field<target::Service>(row["owner"], 5),
               {row["id"].as<std::string>()},
               {enum_field<target::ResourceKind>(row["resource_kind"], 15),
                {row["resource_id"].as<std::string>()}}};
  value.version = {row["version"].as<std::uint64_t>()};
  value.state = enum_field<target::OperationState>(row["state"], 6);
  value.execution_outcome = enum_field<target::ExecutionOutcome>(row["execution_outcome"], 3);
  if (!row["blocked_reason"].is_null()) {
    value.blocked_reason = enum_field<target::ErrorCode>(row["blocked_reason"], 10);
  }
  value.next_attempt = optional_time(row, "next_attempt");
  if (!valid_text(value)) {
    throw std::runtime_error("invalid stored operation text");
  }
  return value;
}
pqxx::params handoff_params(const target::HandoffContext& value) {
  return {static_cast<int>(value.source),
          value.message.value,
          value.operation.id.value,
          static_cast<int>(value.operation.owner),
          static_cast<int>(value.operation.resource.kind),
          value.operation.resource.id.value,
          value.causation ? std::optional<std::string>(value.causation->value) : std::nullopt,
          static_cast<int>(value.resource.kind),
          value.resource.id.value,
          value.version.value,
          value.actor.value,
          value.authorization_version.value,
          digest(value.payload_hash),
          timestamp(value.created_at),
          timestamp(value.valid_until)};
}
std::string handoff_projection() {
  return "source,message,operation_id,operation_owner,operation_resource_kind,operation_resource_"
         "id,causation,resource_kind,resource_id,resource_version,actor,"
         "authorization_version,payload_hash," +
         time_projection("created_at") + "," + time_projection("valid_until");
}
target::HandoffContext read_handoff(const pqxx::row& row) {
  target::HandoffContext value;
  value.source = enum_field<target::Service>(row["source"], 5);
  value.message = {row["message"].as<std::string>()};
  value.resource = {enum_field<target::ResourceKind>(row["resource_kind"], 15),
                    {row["resource_id"].as<std::string>()}};
  value.operation = {enum_field<target::Service>(row["operation_owner"], 5),
                     {row["operation_id"].as<std::string>()},
                     {enum_field<target::ResourceKind>(row["operation_resource_kind"], 15),
                      {row["operation_resource_id"].as<std::string>()}}};
  if (!row["causation"].is_null()) {
    value.causation = target::MessageId{row["causation"].as<std::string>()};
  }
  value.version = {row["resource_version"].as<std::uint64_t>()};
  value.actor = {row["actor"].as<std::string>()};
  value.authorization_version = {row["authorization_version"].as<std::uint64_t>()};
  value.payload_hash = read_digest(row["payload_hash"]);
  value.created_at = time_field(row, "created_at");
  value.valid_until = time_field(row, "valid_until");
  if (!valid_text(value)) {
    throw std::runtime_error("invalid stored handoff text");
  }
  return value;
}

RecordSpec record_spec(RecordKind kind) {
  switch (kind) {
  case RecordKind::Principal:
    return {"principals", {"id"}, target::Service::Identity};
  case RecordKind::Session:
    return {"sessions", {"id"}, target::Service::Identity};
  case RecordKind::Grant:
    return {"grants", {"id"}, target::Service::Identity};
  case RecordKind::Account:
    return {"accounts", {"id"}, target::Service::Runtime};
  case RecordKind::Installation:
    return {"installations", {"id"}, target::Service::Runtime};
  case RecordKind::Instance:
    return {"instances", {"id"}, target::Service::Runtime};
  case RecordKind::Task:
    return {"tasks", {"owner", "task_id"}, target::Service::Scheduler};
  case RecordKind::Occurrence:
    return {"occurrences", {"occurrence_id"}, target::Service::Scheduler};
  case RecordKind::ModelIntent:
    return {"model_intents", {"owner", "logical_call_id"}, target::Service::Model};
  }
  throw std::runtime_error("invalid relation dispatch");
}
std::vector<std::string> record_key(const target::ReplayResult& value) {
  return std::visit(
      [](const auto& record) -> std::vector<std::string> {
        using Value = std::decay_t<decltype(record)>;
        if constexpr (std::is_same_v<Value, target::ScheduledTask>) {
          return {record.key.owner.value, record.key.task_id};
        } else if constexpr (std::is_same_v<Value, target::ModelIntent>) {
          return {record.owner.value, record.logical_call_id};
        } else if constexpr (std::is_same_v<Value, target::Occurrence>) {
          return {record.occurrence_id.value};
        } else if constexpr (requires { record.id.value; }) {
          return {record.id.value};
        } else {
          return {};
        }
      },
      value);
}
target::ResourceVersion record_version(const target::ReplayResult& value) {
  return std::visit(
      [](const auto& record) -> target::ResourceVersion {
        if constexpr (requires { record.version; }) {
          return record.version;
        } else {
          return {};
        }
      },
      value);
}
namespace {
std::string key_predicate(const RecordSpec& spec, std::size_t offset = 1) {
  std::string predicate;
  for (std::size_t index = 0; index < spec.keys.size(); ++index) {
    predicate += (index ? " AND " : "") + std::string(spec.keys[index]) + "=$" +
                 std::to_string(offset + index);
  }
  return predicate;
}
pqxx::params key_parameters(std::span<const std::string> key) {
  pqxx::params parameters;
  for (const auto& component : key) {
    parameters.append(component);
  }
  return parameters;
}
bool put_live(pqxx::transaction_base& transaction, const RecordSpec& spec, std::string_view columns,
              pqxx::params parameters, std::span<const std::string> key,
              std::optional<target::ResourceVersion> expected) {
  if (!expected) {
    return !transaction
                .exec("INSERT INTO kiarabot." + std::string(spec.table) + "(" +
                          std::string(columns) + ") VALUES(" + marks(parameters.size()) +
                          ") ON CONFLICT DO NOTHING RETURNING " + std::string(spec.keys[0]),
                      parameters)
                .empty();
  }
  std::string assignments;
  std::size_t begin = 0, index = 1;
  while (begin < columns.size()) {
    const auto end = columns.find(',', begin);
    const auto column =
        columns.substr(begin, end == std::string_view::npos ? columns.size() - begin : end - begin);
    assignments +=
        (assignments.empty() ? "" : ",") + std::string(column) + "=$" + std::to_string(index++);
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  const auto predicate = key_predicate(spec, parameters.size() + 1);
  parameters.append(key_parameters(key));
  const auto version_parameter = parameters.size() + 1;
  parameters.append(expected->value);
  return !transaction
              .exec("UPDATE kiarabot." + std::string(spec.table) + " SET " + assignments +
                        " WHERE " + predicate + " AND version=$" +
                        std::to_string(version_parameter) + " RETURNING " +
                        std::string(spec.keys[0]),
                    parameters)
              .empty();
}
std::int64_t write_record(pqxx::transaction_base& transaction, const target::ReplayResult& result,
                          bool live, std::optional<target::ResourceVersion> expected) {
  const auto id =
      live ? std::int64_t{0}
           : transaction
                 .exec("INSERT INTO kiarabot.replay_results(kind) VALUES($1) RETURNING id",
                       pqxx::params{static_cast<int>(result.index())})
                 .one_row()[0]
                 .as<std::int64_t>();
  bool matched = true;
  const auto put = [&](pqxx::transaction_base& connection, std::string_view table,
                       std::string_view columns, std::int64_t result_id, pqxx::params parameters) {
    if (!live) {
      put_snapshot(connection, table, columns, result_id, std::move(parameters));
      return;
    }
    const auto spec = record_spec(static_cast<RecordKind>(result.index()));
    if (spec.table != table) {
      throw std::runtime_error("invalid relation codec");
    }
    matched =
        put_live(connection, spec, columns, std::move(parameters), record_key(result), expected);
  };
  std::visit(
      [&](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, target::EmptySuccess>) {
          return;
        } else if constexpr (std::is_same_v<Value, target::ResourceMutationResult>) {
          put(transaction, "mutations", "resource_kind,resource_id,version", id,
              {static_cast<int>(value.resource.kind), value.resource.id.value,
               value.version.value});
        } else if constexpr (std::is_same_v<Value, target::Operation>) {
          put(transaction, "operations",
              "id,owner,resource_kind,resource_id,version,state,execution_outcome,blocked_reason,"
              "next_attempt",
              id,
              {value.ref.id.value, static_cast<int>(value.ref.owner),
               static_cast<int>(value.ref.resource.kind), value.ref.resource.id.value,
               value.version.value, static_cast<int>(value.state),
               static_cast<int>(value.execution_outcome), optional_enum(value.blocked_reason),
               optional_timestamp(value.next_attempt)});
        } else if constexpr (std::is_same_v<Value, target::Principal>) {
          put(transaction, "principals", "id,kind,binding_kind,binding_id,version,state", id,
              {value.id.value, static_cast<int>(value.kind),
               value.binding ? std::optional<int>(static_cast<int>(value.binding->kind))
                             : std::nullopt,
               value.binding ? std::optional<std::string>(value.binding->id.value) : std::nullopt,
               value.version.value, static_cast<int>(value.state)});
        } else if constexpr (std::is_same_v<Value, target::LoginSession>) {
          put(transaction, "sessions", "id,subject,version,rotation,expires_at,revoked", id,
              {value.id.value, value.subject.value, value.version.value, value.rotation,
               timestamp(value.expires_at), value.revoked});
        } else if constexpr (std::is_same_v<Value, target::ResourceGrant>) {
          put(transaction, "grants", "id,subject,role,version", id,
              {value.id.value, value.subject.value, optional_enum(value.role),
               value.version.value});
          if (!matched) {
            return;
          }
          if (live) {
            transaction.exec("DELETE FROM kiarabot.grant_scopes WHERE grant_id=$1",
                             pqxx::params{value.id.value});
          }
          for (std::size_t index = 0; index < value.scopes.size(); ++index) {
            const auto& scope = value.scopes[index];
            pqxx::params parameters;
            if (live) {
              parameters.append(value.id.value);
            } else {
              parameters.append(id);
            }
            parameters.append(pqxx::params{index, static_cast<int>(scope.resource.kind),
                                           scope.resource.id.value, scope.permission});
            transaction.exec("INSERT INTO kiarabot." +
                                 std::string(live ? "grant_scopes" : "result_grant_scopes") +
                                 " VALUES($1,$2,$3,$4,$5)",
                             parameters);
          }
        } else if constexpr (std::is_same_v<Value, target::BotAccount>) {
          put(transaction, "accounts", "id,platform,platform_account_id,display_name,version,state",
              id,
              {value.id.value, value.platform, value.platform_account_id, value.display_name,
               value.version.value, static_cast<int>(value.state)});
        } else if constexpr (std::is_same_v<Value, target::PluginInstallation>) {
          put(transaction, "installations", "id,plugin_type,version,state", id,
              {value.id.value, value.plugin_type.value, value.version.value,
               static_cast<int>(value.state)});
        } else if constexpr (std::is_same_v<Value, target::PluginInstance>) {
          pqxx::params parameters{value.id.value, value.installation.value, value.plugin_type.value,
                                  value.desired_version.value, value.applied_config_version.value};
          if (value.current) {
            const auto& fence = *value.current;
            parameters.append(pqxx::params{fence.instance.value, fence.runtime_epoch.value,
                                           fence.execution.value, fence.execution_generation.value,
                                           fence.connection.value,
                                           fence.connection_generation.value});
          } else {
            parameters.append(pqxx::params{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr});
          }
          parameters.append(pqxx::params{value.version.value, static_cast<int>(value.state),
                                         value.host ? std::optional<std::string>(value.host->value)
                                                    : std::nullopt});
          put(transaction, "instances",
              "id,installation,plugin_type,desired_version,applied_config_version,fence_instance,"
              "runtime_epoch,execution_id,execution_generation,connection_id,connection_generation,"
              "version,state,host",
              id, std::move(parameters));
        } else if constexpr (std::is_same_v<Value, target::ScheduledTask>) {
          put(transaction, "tasks",
              "owner,task_id,installation,adapter,account,conversation_id,conversation_kind,"
              "payload,run_at,state,version,occurrence_id",
              id,
              {value.key.owner.value, value.key.task_id, value.installation.value,
               value.target.adapter.value, value.target.account.value, value.target.conversation_id,
               static_cast<int>(value.target.kind),
               pqxx::bytes_view(value.payload.data(), value.payload.size()),
               timestamp(value.run_at), static_cast<int>(value.state), value.version.value,
               value.occurrence.value});
        } else if constexpr (std::is_same_v<Value, target::Occurrence>) {
          pqxx::params parameters{value.owner.value,
                                  value.installation.value,
                                  value.task_id,
                                  value.task_version.value,
                                  value.occurrence_id.value,
                                  timestamp(value.due_at),
                                  value.version.value,
                                  static_cast<int>(value.state),
                                  value.accepted_event_id,
                                  optional_timestamp(value.acknowledged_at)};
          parameters.append(handoff_params(value.handoff));
          put(transaction, "occurrences",
              "owner,installation,task_id,task_version,occurrence_id,due_at,version,state,accepted_"
              "event_id,acknowledged_at," +
                  std::string(kHandoffFields),
              id, std::move(parameters));
        } else if constexpr (std::is_same_v<Value, target::SessionMetadata>) {
          put(transaction, "session_metadata",
              "id,subject,created_at,expires_at,rotation,version,revoked", id,
              {value.id.value, value.subject.value, timestamp(value.created_at),
               timestamp(value.expires_at), value.rotation, value.version.value, value.revoked});
        } else if constexpr (std::is_same_v<Value, target::ModelIntent>) {
          put(transaction, "model_intents",
              "owner,logical_call_id,model,config_version,request_hash,state,version,created_at,"
              "observed_at",
              id,
              {value.owner.value, value.logical_call_id, value.model.value,
               value.config_version.value, digest(value.request_hash),
               static_cast<int>(value.state), value.version.value, timestamp(value.created_at),
               timestamp(value.observed_at)});
        }
      },
      result);
  return live ? (matched ? 1 : 0) : id;
}
} // namespace
std::int64_t store_result(pqxx::transaction_base& transaction, const target::ReplayResult& result) {
  return write_record(transaction, result, false, {});
}
namespace {
bool revoked_grant(pqxx::transaction_base& transaction, const std::string& id) {
  const auto rows = transaction.exec("SELECT state FROM kiarabot.exact_assignments WHERE id=$1",
                                     pqxx::params{id});
  if (rows.empty()) {
    return false;
  }
  const auto state = rows.one_row()[0].as<int>();
  if (state != 0 && state != 1) {
    throw std::runtime_error("invalid stored assignment state");
  }
  return state == 1;
}
} // namespace
bool store_live(pqxx::transaction_base& transaction, RecordKind kind,
                const target::ReplayResult& value,
                std::optional<target::ResourceVersion> expected) {
  if (static_cast<int>(kind) != static_cast<int>(value.index())) {
    return false;
  }
  if (kind == RecordKind::Grant &&
      revoked_grant(transaction, std::get<target::ResourceGrant>(value).id.value)) {
    return false;
  }
  return write_record(transaction, value, true, expected) != 0;
}

namespace {
target::ReplayResult load_record(pqxx::transaction_base& transaction, std::int64_t id,
                                 std::optional<RecordKind> live_kind = {},
                                 std::span<const std::string> live_key = {}) {
  const auto kind = live_kind ? static_cast<int>(*live_kind)
                              : transaction
                                    .exec("SELECT kind FROM kiarabot.replay_results WHERE id=$1",
                                          pqxx::params{id})
                                    .one_row()[0]
                                    .as<int>();
  const auto get = [&](pqxx::transaction_base& connection, std::string_view table,
                       std::int64_t result_id, std::string_view projection = "*") -> pqxx::row {
    if (!live_kind) {
      return get_snapshot(connection, table, result_id, projection);
    }
    const auto spec = record_spec(*live_kind);
    if (spec.table != table) {
      throw std::runtime_error("invalid relation decoder");
    }
    return connection
        .exec("SELECT " + std::string(projection) + " FROM kiarabot." + std::string(table) +
                  " WHERE " + key_predicate(spec),
              key_parameters(live_key))
        .one_row();
  };
  switch (kind) {
  case 0:
    return target::EmptySuccess{};
  case 1: {
    const auto row = get(transaction, "mutations", id);
    return target::ResourceMutationResult{
        {enum_field<target::ResourceKind>(row["resource_kind"], 15),
         {row["resource_id"].as<std::string>()}},
        {row["version"].as<std::uint64_t>()}};
  }
  case 2:
    return read_operation(
        get(transaction, "operations", id,
            "id,owner,resource_kind,resource_id,version,state,execution_outcome,blocked_reason," +
                time_projection("next_attempt")));
  case 3: {
    const auto row = get(transaction, "principals", id);
    target::Principal value{{row["id"].as<std::string>()},
                            enum_field<target::PrincipalKind>(row["kind"], 3),
                            {},
                            {row["version"].as<std::uint64_t>()},
                            enum_field<target::LifecycleState>(row["state"], 5)};
    if (!row["binding_kind"].is_null()) {
      value.binding = target::ResourceRef{enum_field<target::ResourceKind>(row["binding_kind"], 15),
                                          {row["binding_id"].as<std::string>()}};
    }
    return value;
  }
  case 4: {
    const auto row = get(transaction, "sessions", id,
                         "id,subject,version,rotation,revoked," + time_projection("expires_at"));
    return target::LoginSession{
        {row["id"].as<std::string>()},        {row["subject"].as<std::string>()},
        {row["version"].as<std::uint64_t>()}, row["rotation"].as<std::uint64_t>(),
        time_field(row, "expires_at"),        row["revoked"].as<bool>()};
  }
  case 5: {
    const auto row = get(transaction, "grants", id);
    target::ResourceGrant value{{row["id"].as<std::string>()},
                                {row["subject"].as<std::string>()},
                                {},
                                {},
                                {row["version"].as<std::uint64_t>()}};
    if (!row["role"].is_null()) {
      value.role = enum_field<target::FixedRole>(row["role"], 2);
    }
    const auto scopes =
        live_kind
            ? transaction.exec(
                  "SELECT * FROM kiarabot.grant_scopes WHERE grant_id=$1 ORDER BY position",
                  pqxx::params{live_key[0]})
            : transaction.exec(
                  "SELECT * FROM kiarabot.result_grant_scopes WHERE result_id=$1 ORDER BY position",
                  pqxx::params{id});
    for (const auto& scope : scopes) {
      value.scopes.push_back({{enum_field<target::ResourceKind>(scope["resource_kind"], 15),
                               {scope["resource_id"].as<std::string>()}},
                              scope["permission"].as<std::string>()});
    }
    return value;
  }
  case 6: {
    const auto row = get(transaction, "accounts", id);
    return target::BotAccount{{row["id"].as<std::string>()},
                              row["platform"].as<std::string>(),
                              row["platform_account_id"].as<std::string>(),
                              row["display_name"].as<std::string>(),
                              {row["version"].as<std::uint64_t>()},
                              enum_field<target::LifecycleState>(row["state"], 5)};
  }
  case 7: {
    const auto row = get(transaction, "installations", id);
    return target::PluginInstallation{{row["id"].as<std::string>()},
                                      {row["plugin_type"].as<std::string>()},
                                      {row["version"].as<std::uint64_t>()},
                                      enum_field<target::LifecycleState>(row["state"], 5)};
  }
  case 8: {
    const auto row = get(transaction, "instances", id);
    target::PluginInstance value{{row["id"].as<std::string>()},
                                 {row["installation"].as<std::string>()},
                                 {row["plugin_type"].as<std::string>()},
                                 {row["desired_version"].as<std::uint64_t>()},
                                 {row["applied_config_version"].as<std::uint64_t>()},
                                 {},
                                 {row["version"].as<std::uint64_t>()},
                                 enum_field<target::LifecycleState>(row["state"], 5),
                                 {}};
    if (!row["fence_instance"].is_null()) {
      value.current = target::Fence{{row["fence_instance"].as<std::string>()},
                                    {row["runtime_epoch"].as<std::string>()},
                                    {row["execution_id"].as<std::string>()},
                                    {row["execution_generation"].as<std::uint64_t>()},
                                    {row["connection_id"].as<std::string>()},
                                    {row["connection_generation"].as<std::uint64_t>()}};
    }
    if (!row["host"].is_null()) {
      value.host = target::ResourceId{row["host"].as<std::string>()};
    }
    return value;
  }
  case 9: {
    const auto row = get(transaction, "tasks", id,
                         "owner,task_id,installation,adapter,account,conversation_id,conversation_"
                         "kind,payload,state,version,occurrence_id," +
                             time_projection("run_at"));
    const auto payload = row["payload"].as<pqxx::bytes>();
    target::ScheduledTask value{
        {{row["owner"].as<std::string>()}, row["task_id"].as<std::string>()},
        {row["installation"].as<std::string>()},
        {{row["adapter"].as<std::string>()},
         {row["account"].as<std::string>()},
         row["conversation_id"].as<std::string>(),
         enum_field<target::ConversationKey::Kind>(row["conversation_kind"], 2)},
        {},
        time_field(row, "run_at"),
        enum_field<target::TaskState>(row["state"], 5),
        {row["version"].as<std::uint64_t>()},
        {row["occurrence_id"].as<std::string>()}};
    for (std::size_t index = 0; index < payload.size(); ++index) {
      value.payload.push_back(static_cast<std::byte>(payload[index]));
    }
    return value;
  }
  case 10: {
    const auto row = get(
        transaction, "occurrences", id,
        "owner,installation,task_id,task_version,occurrence_id,version,state,accepted_event_id," +
            time_projection("due_at") + "," + time_projection("acknowledged_at") + "," +
            handoff_projection());
    return target::Occurrence{{row["owner"].as<std::string>()},
                              {row["installation"].as<std::string>()},
                              row["task_id"].as<std::string>(),
                              {row["task_version"].as<std::uint64_t>()},
                              {row["occurrence_id"].as<std::string>()},
                              time_field(row, "due_at"),
                              read_handoff(row),
                              {row["version"].as<std::uint64_t>()},
                              enum_field<target::OperationState>(row["state"], 6),
                              row["accepted_event_id"].as<std::optional<std::string>>(),
                              optional_time(row, "acknowledged_at")};
  }
  case 11: {
    const auto row = get(transaction, "model_intents", id,
                         "owner,logical_call_id,model,config_version,request_hash,state,version," +
                             time_projection("created_at") + "," + time_projection("observed_at"));
    return target::ModelIntent{
        {row["owner"].as<std::string>()},     row["logical_call_id"].as<std::string>(),
        {row["model"].as<std::string>()},     {row["config_version"].as<std::uint64_t>()},
        read_digest(row["request_hash"]),     enum_field<target::ModelCallState>(row["state"], 3),
        {row["version"].as<std::uint64_t>()}, time_field(row, "created_at"),
        time_field(row, "observed_at")};
  }
  case 12: {
    const auto row = get(transaction, "session_metadata", id);
    return target::SessionMetadata{{row["id"].as<std::string>()},
                                   {row["subject"].as<std::string>()},
                                   time_field(row, "created_at"),
                                   time_field(row, "expires_at"),
                                   row["rotation"].as<std::uint64_t>(),
                                   {row["version"].as<std::uint64_t>()},
                                   row["revoked"].as<bool>()};
  }
  default:
    throw std::runtime_error("invalid stored result kind");
  }
}
} // namespace

target::ReplayResult load_result(pqxx::transaction_base& transaction, std::int64_t id) {
  auto value = load_record(transaction, id);
  if (!valid_text(value)) {
    throw std::runtime_error("invalid stored text representation");
  }
  return value;
}
std::optional<target::ReplayResult> load_live(pqxx::transaction_base& transaction, RecordKind kind,
                                              std::span<const std::string> key) {
  if (kind == RecordKind::Grant && revoked_grant(transaction, key[0])) {
    return {};
  }
  const auto spec = record_spec(kind);
  const auto exists = transaction.exec("SELECT 1 FROM kiarabot." + std::string(spec.table) +
                                           " WHERE " + key_predicate(spec),
                                       key_parameters(key));
  if (exists.empty()) {
    return {};
  }
  auto value = load_record(transaction, 0, kind, key);
  if (!valid_text(value)) {
    throw std::runtime_error("invalid stored text representation");
  }
  return value;
}
} // namespace kiarabot::infrastructure::postgres::detail
