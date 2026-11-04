#include "observation_feeds.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "record_codec.hpp"

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace t = core::target;
using Json = nlohmann::json;
constexpr std::int64_t kRetentionNs = 600000000000;

t::Failure invalid() {
  return t::rejected(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
}
t::Failure gap() {
  return t::rejected(t::ErrorCode::Conflict, t::Recovery::ReadCurrent);
}
bool bounded_text(std::string_view value, std::size_t minimum, std::size_t maximum) {
  return value.size() >= minimum && value.size() <= maximum && valid_sql_text(value);
}
int family_number(t::ObservationFamily family) {
  switch (family) {
  case t::ObservationFamily::Logs:
    return 1;
  case t::ObservationFamily::Messages:
    return 2;
  case t::ObservationFamily::Tasks:
    return 3;
  }
  return 0;
}
bool append_resource_valid(t::ObservationFamily family, const t::ResourceRef& resource) {
  if (!bounded_text(resource.id.value, 1, 256)) {
    return false;
  }
  if (family == t::ObservationFamily::Logs) {
    // A log row records the exact audit-entry resource (storage/namespace/instance/catalog/...);
    // the request scope for this family is still exactly RESOURCE_CATALOG/control. Only the
    // control catalog id is pinned here because it is the shared authorization catalog.
    return resource.kind != t::ResourceKind::Catalog || resource.id.value == "control";
  }
  switch (resource.kind) {
  case t::ResourceKind::Account:
  case t::ResourceKind::Adapter:
  case t::ResourceKind::Installation:
  case t::ResourceKind::Instance:
  case t::ResourceKind::Host:
  case t::ResourceKind::Task:
  case t::ResourceKind::Model:
    return true;
  default:
    return false;
  }
}
// Frozen request scope per family: logs exactly RESOURCE_CATALOG/control; tasks exact
// RESOURCE_TASK|RESOURCE_INSTANCE; messages exact RESOURCE_ACCOUNT.
bool scope_resource_valid(t::ObservationFamily family, const t::ResourceRef& resource) {
  if (family == t::ObservationFamily::Logs) {
    return resource.kind == t::ResourceKind::Catalog && resource.id.value == "control";
  }
  if (family == t::ObservationFamily::Tasks) {
    return (resource.kind == t::ResourceKind::Task || resource.kind == t::ResourceKind::Instance) &&
           bounded_text(resource.id.value, 1, 256);
  }
  return resource.kind == t::ResourceKind::Account && bounded_text(resource.id.value, 1, 256);
}
bool canonical_scope(t::ObservationFamily family, std::vector<t::ResourceRef>& scope) {
  if (scope.empty() || scope.size() > 127) {
    return false;
  }
  for (const auto& resource : scope) {
    if (!scope_resource_valid(family, resource)) {
      return false;
    }
  }
  std::sort(scope.begin(), scope.end(), [](const auto& left, const auto& right) {
    return std::pair{left.kind, left.id.value} < std::pair{right.kind, right.id.value};
  });
  return std::adjacent_find(scope.begin(), scope.end()) == scope.end();
}
std::int64_t database_now(pqxx::transaction_base& tx) {
  return tx.exec("SELECT (extract(epoch FROM clock_timestamp())*1000000000)::bigint")
      .one_row()[0]
      .as<std::int64_t>();
}
struct Head {
  int family = 0;
  std::string epoch;
  std::uint64_t sequence = 0;
  std::uint64_t low_water = 0;
  std::int64_t observed_at = 0;
  std::size_t max_events = 0;
  std::size_t max_bytes = 0;
};
Head head(pqxx::transaction_base& tx, int family, bool exclusive) {
  const auto row = tx.exec("SELECT epoch,sequence,low_water,observed_at,max_events,max_bytes "
                           "FROM kiarabot.observation_feed_head WHERE family=$1 " +
                               std::string(exclusive ? "FOR UPDATE" : "FOR SHARE"),
                           pqxx::params{family})
                       .one_row();
  return {family,
          row["epoch"].as<std::string>(),
          row["sequence"].as<std::uint64_t>(),
          row["low_water"].as<std::uint64_t>(),
          row["observed_at"].as<std::int64_t>(),
          row["max_events"].as<std::size_t>(),
          row["max_bytes"].as<std::size_t>()};
}
std::size_t prune_locked(pqxx::transaction_base& tx, const Head& current, std::size_t limit,
                         std::int64_t now) {
  const auto rows =
      tx.exec("WITH ranked AS (SELECT sequence,observed_at,"
              "row_number() OVER(ORDER BY sequence DESC) AS ordinal,"
              "sum(encoded_bytes) OVER(ORDER BY sequence DESC) AS bytes FROM "
              "kiarabot.observation_feed_changes WHERE family=$5), victims AS (SELECT sequence "
              "FROM ranked WHERE observed_at<=$1 OR ordinal>$2 OR bytes>$3 ORDER BY sequence "
              "LIMIT $4) DELETE FROM kiarabot.observation_feed_changes c USING victims v "
              "WHERE c.family=$5 AND c.sequence=v.sequence RETURNING c.sequence",
              pqxx::params{now - kRetentionNs, current.max_events, current.max_bytes, limit,
                           current.family});
  auto low = current.low_water;
  for (const auto& row : rows) {
    low = std::max(low, row[0].as<std::uint64_t>());
  }
  if (low != current.low_water) {
    tx.exec("UPDATE kiarabot.observation_feed_head SET low_water=$1 WHERE family=$2",
            pqxx::params{low, current.family});
  }
  return rows.size();
}
bool remaining_budget(pqxx::transaction_base& tx, std::chrono::steady_clock::time_point deadline) {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining.count() <= 0) {
    return false;
  }
  const auto budget = std::min<std::int64_t>(500, remaining.count());
  tx.exec("SELECT set_config('statement_timeout',$1,true),set_config('lock_timeout',$2,true)",
          pqxx::params{std::to_string(budget) + "ms",
                       std::to_string(std::min<std::int64_t>(250, budget)) + "ms"});
  return true;
}
std::int64_t nanos(t::TimePoint value) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}
Json resource_json(const t::ResourceRef& resource) {
  return {{"kind", static_cast<int>(resource.kind)}, {"id", resource.id.value}};
}
bool read_resource(const Json& value, t::ResourceRef& resource) {
  if (!value.is_object() || value.size() != 2 || !value.contains("kind") || !value.contains("id") ||
      !value["kind"].is_number_integer() || !value["id"].is_string()) {
    return false;
  }
  const auto kind = value["kind"].get<int>();
  if (kind < 0 || kind > 15) {
    return false;
  }
  resource.kind = static_cast<t::ResourceKind>(kind);
  resource.id.value = value["id"].get<std::string>();
  return bounded_text(resource.id.value, 1, 256);
}
std::string encode_payload(t::ObservationFamily family, const t::ObservationPayload& payload) {
  if (family == t::ObservationFamily::Logs) {
    const auto& log = std::get<t::LogObservation>(payload);
    return Json{{"owner", static_cast<int>(log.owner)},
                {"entry_id", log.entry_id},
                {"severity", log.severity},
                {"component", log.component},
                {"method", log.method},
                {"event", log.event},
                {"resource", resource_json(log.resource)},
                {"request_id", log.request_id},
                {"trace_id", log.trace_id},
                {"occurred_at", nanos(log.occurred_at)}}
        .dump();
  }
  if (family == t::ObservationFamily::Tasks) {
    const auto& task = std::get<t::TaskObservation>(payload);
    Json value{
        {"owner_instance_id", task.owner_instance_id}, {"task_id", task.task_id},
        {"state", static_cast<int>(task.state)},       {"version", task.version},
        {"updated_at", nanos(task.updated_at)},        {"cancel_requested", task.cancel_requested}};
    if (task.occurrence_id) {
      value["occurrence_id"] = *task.occurrence_id;
    }
    if (task.reason_code) {
      value["reason_code"] = static_cast<int>(*task.reason_code);
    }
    return value.dump();
  }
  const auto& message = std::get<t::MessageObservation>(payload);
  return Json{{"message_id", message.message_id},
              {"conversation_id", message.conversation_id},
              {"account_id", message.account_id},
              {"received_at", nanos(message.received_at)}}
      .dump();
}
bool valid_payload(t::ObservationFamily family, const t::ObservationPayload& payload,
                   t::ResourceRef& resource) {
  if (const auto* log = std::get_if<t::LogObservation>(&payload)) {
    if (family != t::ObservationFamily::Logs) {
      return false;
    }
    resource = log->resource;
    return (log->owner == t::Service::Identity || log->owner == t::Service::Runtime) &&
           bounded_text(log->entry_id, 1, 256) &&
           (log->severity == "DEBUG" || log->severity == "INFO" || log->severity == "WARN" ||
            log->severity == "ERROR") &&
           bounded_text(log->component, 1, 128) && bounded_text(log->method, 1, 128) &&
           (log->event == "request.recorded" || log->event == "request.rejected") &&
           bounded_text(log->request_id, 0, 128) && bounded_text(log->trace_id, 0, 128) &&
           log->occurred_at > t::TimePoint{};
  }
  if (const auto* task = std::get_if<t::TaskObservation>(&payload)) {
    if (family != t::ObservationFamily::Tasks) {
      return false;
    }
    resource = {t::ResourceKind::Task, t::ResourceId{task->task_id}};
    return bounded_text(task->task_id, 1, 256) && bounded_text(task->owner_instance_id, 1, 256) &&
           task->version > 0 && task->updated_at > t::TimePoint{} &&
           (!task->occurrence_id || bounded_text(*task->occurrence_id, 1, 256));
  }
  const auto* message = std::get_if<t::MessageObservation>(&payload);
  if (message == nullptr || family != t::ObservationFamily::Messages) {
    return false;
  }
  resource = {t::ResourceKind::Account, t::ResourceId{message->account_id}};
  return bounded_text(message->message_id, 1, 256) &&
         bounded_text(message->conversation_id, 1, 256) &&
         bounded_text(message->account_id, 1, 256) && message->received_at > t::TimePoint{};
}
bool decode_payload(t::ObservationFamily family, std::string_view text,
                    t::ObservationPayload& payload) {
  const auto value = Json::parse(text, nullptr, false);
  if (value.is_discarded() || !value.is_object()) {
    return false;
  }
  if (family == t::ObservationFamily::Logs) {
    if (!value.contains("owner") || !value.contains("entry_id") || !value.contains("severity") ||
        !value.contains("component") || !value.contains("method") || !value.contains("event") ||
        !value.contains("resource") || !value.contains("request_id") ||
        !value.contains("trace_id") || !value.contains("occurred_at") ||
        !value["occurred_at"].is_number_integer()) {
      return false;
    }
    t::LogObservation log;
    log.owner = static_cast<t::Service>(value["owner"].get<int>());
    log.entry_id = value["entry_id"].get<std::string>();
    log.severity = value["severity"].get<std::string>();
    log.component = value["component"].get<std::string>();
    log.method = value["method"].get<std::string>();
    log.event = value["event"].get<std::string>();
    if (!read_resource(value["resource"], log.resource)) {
      return false;
    }
    log.request_id = value["request_id"].get<std::string>();
    log.trace_id = value["trace_id"].get<std::string>();
    log.occurred_at =
        t::TimePoint{std::chrono::nanoseconds{value["occurred_at"].get<std::int64_t>()}};
    payload = std::move(log);
    return true;
  }
  if (family == t::ObservationFamily::Tasks) {
    t::TaskObservation task;
    if (!value.contains("owner_instance_id") || !value.contains("task_id") ||
        !value.contains("state") || !value.contains("version") || !value.contains("updated_at") ||
        !value.contains("cancel_requested")) {
      return false;
    }
    task.owner_instance_id = value["owner_instance_id"].get<std::string>();
    task.task_id = value["task_id"].get<std::string>();
    const auto state = value["state"].get<int>();
    if (state < 0 || state > 5) {
      return false;
    }
    task.state = static_cast<t::TaskFeedState>(state);
    task.version = value["version"].get<std::uint64_t>();
    task.updated_at =
        t::TimePoint{std::chrono::nanoseconds{value["updated_at"].get<std::int64_t>()}};
    task.cancel_requested = value["cancel_requested"].get<bool>();
    if (value.contains("occurrence_id") && value["occurrence_id"].is_string()) {
      task.occurrence_id = value["occurrence_id"].get<std::string>();
    }
    if (value.contains("reason_code") && value["reason_code"].is_number_integer()) {
      task.reason_code = static_cast<t::ErrorCode>(value["reason_code"].get<int>());
    }
    payload = std::move(task);
    return true;
  }
  t::MessageObservation message;
  if (!value.contains("message_id") || !value.contains("conversation_id") ||
      !value.contains("account_id") || !value.contains("received_at")) {
    return false;
  }
  message.message_id = value["message_id"].get<std::string>();
  message.conversation_id = value["conversation_id"].get<std::string>();
  message.account_id = value["account_id"].get<std::string>();
  message.received_at =
      t::TimePoint{std::chrono::nanoseconds{value["received_at"].get<std::int64_t>()}};
  payload = std::move(message);
  return true;
}
} // namespace

t::Status append_observation_changes(SqlWork& work, t::Service owner, t::ObservationFamily family,
                                     const std::vector<t::ObservationFeedAppend>& changes) {
  if (changes.empty()) {
    return std::monostate{};
  }
  const auto family_value = family_number(family);
  const bool owner_family =
      (owner == t::Service::Identity && family == t::ObservationFamily::Logs) ||
      (owner == t::Service::Runtime &&
       (family == t::ObservationFamily::Logs || family == t::ObservationFamily::Messages)) ||
      (owner == t::Service::Scheduler && family == t::ObservationFamily::Tasks);
  if (family_value == 0 || !owner_family || changes.size() > 1000) {
    return t::rejected(t::ErrorCode::RateLimited, t::Recovery::Backoff);
  }
  return work.run([&](pqxx::transaction_base& tx) -> t::Status {
    std::vector<std::pair<t::ResourceRef, std::string>> encoded;
    encoded.reserve(changes.size());
    for (const auto& change : changes) {
      t::ResourceRef resource;
      if (!append_resource_valid(family, change.resource) || change.observed_at <= t::TimePoint{} ||
          !valid_payload(family, change.payload, resource) || !(resource == change.resource)) {
        return invalid();
      }
      const auto payload = encode_payload(family, change.payload);
      if (payload.size() > t::kObservationFeedMaximumPayloadBytes) {
        return invalid();
      }
      encoded.emplace_back(resource, payload);
    }
    auto current = head(tx, family_value, true);
    if (current.sequence > std::numeric_limits<std::uint64_t>::max() - encoded.size()) {
      return t::rejected(t::ErrorCode::RateLimited, t::Recovery::Backoff);
    }
    const auto now = std::max(database_now(tx), current.observed_at);
    for (auto& [resource, payload] : encoded) {
      const auto sequence = ++current.sequence;
      tx.exec("INSERT INTO kiarabot.observation_feed_changes(family,sequence,resource_kind,"
              "resource_id,observed_at,payload,encoded_bytes) VALUES($1,$2,$3,$4,$5,$6,$7)",
              pqxx::params{family_value, sequence, static_cast<int>(resource.kind),
                           resource.id.value, now, payload, static_cast<int>(payload.size())});
    }
    tx.exec("UPDATE kiarabot.observation_feed_head SET sequence=$1,observed_at=$2 WHERE family=$3",
            pqxx::params{current.sequence, now, family_value});
    prune_locked(tx, current, t::kObservationFeedCapacity + 1000, now);
    return std::monostate{};
  });
}

t::Outcome<t::ObservationFeedPage> query_observation_changes(SqlWork& work, t::Service owner,
                                                             const t::ObservationFeedQuery& query) {
  const auto family = query.family;
  auto scope = query.scope;
  const auto family_value = family_number(family);
  const bool owner_family =
      (owner == t::Service::Identity && family == t::ObservationFamily::Logs) ||
      (owner == t::Service::Runtime &&
       (family == t::ObservationFamily::Logs || family == t::ObservationFamily::Messages)) ||
      (owner == t::Service::Scheduler && family == t::ObservationFamily::Tasks);
  if (family_value == 0 || !owner_family || !canonical_scope(family, scope) ||
      query.page_size > 100) {
    return invalid();
  }
  if (query.cursor) {
    auto previous_scope = query.cursor->scope;
    if (query.cursor->owner != owner || query.cursor->family != family ||
        !bounded_text(query.cursor->epoch, 1, 128) || !canonical_scope(family, previous_scope) ||
        previous_scope != scope) {
      return invalid();
    }
  }
  t::ObservationFeedPage result;
  const auto status = work.run([&](pqxx::transaction_base& tx) -> t::Status {
    if (!remaining_budget(tx, query.deadline)) {
      return t::rejected(t::ErrorCode::Timeout, t::Recovery::Backoff);
    }
    const auto current = head(tx, family_value, false);
    const auto now = database_now(tx);
    result.checked_at = t::TimePoint{std::chrono::nanoseconds{now}};
    result.next_cursor = {owner, family, current.epoch, scope, current.sequence};
    if (!query.cursor) {
      return std::monostate{};
    }
    const auto previous = *query.cursor;
    if (previous.epoch != current.epoch || previous.sequence > current.sequence ||
        previous.sequence < current.low_water) {
      return gap();
    }
    const auto page_size = static_cast<std::size_t>(
        std::min<std::uint32_t>(query.page_size == 0 ? 32 : query.page_size, 100));
    std::vector<int> kinds;
    std::vector<std::string> ids;
    kinds.reserve(scope.size());
    ids.reserve(scope.size());
    for (const auto& resource : scope) {
      kinds.push_back(static_cast<int>(resource.kind));
      ids.push_back(resource.id.value);
    }
    std::string kind_values;
    std::string id_values;
    for (std::size_t index = 0; index < kinds.size(); ++index) {
      if (!kind_values.empty()) {
        kind_values += ',';
      }
      kind_values += std::to_string(kinds[index]);
    }
    for (std::size_t index = 0; index < ids.size(); ++index) {
      if (ids[index].find('\x1f') != std::string::npos) {
        return invalid();
      }
      if (!id_values.empty()) {
        id_values += '\x1f';
      }
      id_values += ids[index];
    }
    // Log feeds are authorized by the explicit catalog/control scope and every redacted entry
    // belongs to that authorized feed; task/message feeds additionally match the exact scope.
    const auto rows =
        tx.exec("SELECT sequence,resource_kind,resource_id,observed_at,payload FROM "
                "kiarabot.observation_feed_changes WHERE family=$1 AND sequence>$2 AND "
                "(family=1 OR (resource_kind=ANY(string_to_array($3,',')::int[]) AND "
                "resource_id=ANY(string_to_array($4,E'\x1f')))) ORDER BY sequence LIMIT $5",
                pqxx::params{family_value, previous.sequence, kind_values, id_values,
                             static_cast<int>(page_size + 1)});
    std::uint64_t last_sequence = previous.sequence;
    for (const auto& row : rows) {
      if (result.changes.size() == page_size) {
        result.has_more = true;
        break;
      }
      t::ObservationPayload payload;
      try {
        if (!decode_payload(family, row["payload"].as<std::string>(), payload)) {
          return t::rejected(t::ErrorCode::Internal);
        }
      } catch (const std::exception&) {
        return t::rejected(t::ErrorCode::Internal);
      }
      const auto sequence = row["sequence"].as<std::uint64_t>();
      if (sequence <= last_sequence) {
        return t::rejected(t::ErrorCode::Internal);
      }
      result.changes.push_back(
          {std::move(payload), {owner, family, current.epoch, scope, sequence}});
      last_sequence = sequence;
    }
    if (!result.has_more) {
      // Every matching row up to the committed head was scanned: advance the scan boundary.
      result.next_cursor.sequence = std::max(current.sequence, last_sequence);
    } else {
      result.next_cursor.sequence = last_sequence;
    }
    return std::monostate{};
  });
  if (const auto* failure = std::get_if<t::Failure>(&status)) {
    return *failure;
  }
  return result;
}

t::Outcome<std::size_t> prune_observation_feeds(SqlWork& work, t::ObservationFamily family,
                                                std::size_t limit) {
  const auto family_value = family_number(family);
  if (family_value == 0 || limit == 0 || limit > 1000) {
    return invalid();
  }
  std::size_t removed = 0;
  const auto status = work.run([&](pqxx::transaction_base& tx) -> t::Status {
    const auto current = head(tx, family_value, false);
    removed = prune_locked(tx, current, limit, database_now(tx));
    return std::monostate{};
  });
  if (const auto* failure = std::get_if<t::Failure>(&status)) {
    return *failure;
  }
  return removed;
}

t::Status rotate_observation_feed_epoch(SqlWork& work, t::ObservationFamily family) {
  const auto family_value = family_number(family);
  if (family_value == 0) {
    return invalid();
  }
  return work.run([&](pqxx::transaction_base& tx) -> t::Status {
    const auto row = tx.exec("UPDATE kiarabot.observation_feed_head SET epoch="
                             "pg_catalog.gen_random_uuid()::text,sequence=0,low_water=0 "
                             "WHERE family=$1 RETURNING family",
                             pqxx::params{family_value});
    if (row.size() != 1) {
      return t::rejected(t::ErrorCode::Internal);
    }
    tx.exec("DELETE FROM kiarabot.observation_feed_changes WHERE family=$1",
            pqxx::params{family_value});
    return std::monostate{};
  });
}

t::ObservationFeedAppend make_log_feed_append(const t::LogObservation& log) {
  t::ObservationFeedAppend append;
  append.resource = log.resource;
  append.payload = log;
  append.observed_at = log.occurred_at;
  return append;
}
t::ObservationFeedAppend make_task_feed_append(const t::TaskObservation& task,
                                               const t::ResourceRef& resource) {
  t::ObservationFeedAppend append;
  append.resource = resource;
  append.payload = task;
  append.observed_at = task.updated_at;
  return append;
}
t::ObservationFeedAppend make_message_feed_append(const t::MessageObservation& message,
                                                  const t::ResourceRef& resource) {
  t::ObservationFeedAppend append;
  append.resource = resource;
  append.payload = message;
  append.observed_at = message.received_at;
  return append;
}

} // namespace kiarabot::infrastructure::postgres::detail
