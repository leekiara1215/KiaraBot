#include "kiarabot/core/application/control_service.hpp"

#include <cctype>
#include <regex>
#include <set>
#include <stdexcept>

#include <google/protobuf/util/json_util.h>

#include "kiarabot/core/runtime/request_digest.hpp"

namespace kiarabot::core {
namespace {
namespace wire = kiarabot::v1;
using Json = ControlJson;
std::int64_t seconds(WallTime time) {
  return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}
std::string lower(std::string value) {
  for (auto& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}
wire::Error error(wire::ErrorCode code, const ControlMetadata& meta) {
  wire::Error result;
  result.set_code(code);
  result.set_message(wire::ErrorCode_Name(code));
  result.set_request_id(meta.request_id);
  result.set_trace_id(meta.trace_id);
  result.set_retryable(code == wire::UPSTREAM_UNAVAILABLE || code == wire::TIMEOUT ||
                       code == wire::RATE_LIMITED);
  return result;
}
wire::Error mapped(StorageError code, const ControlMetadata& meta) {
  return error(code == StorageError::Corrupt    ? wire::INTERNAL
               : code == StorageError::Conflict ? wire::CONFLICT
                                                : wire::UPSTREAM_UNAVAILABLE,
               meta);
}
Json load(const RuntimeSnapshot& snapshot, std::string_view key) {
  const auto found = snapshot.namespaces.find("$control");
  if (found == snapshot.namespaces.end() || !found->second.contains("state")) {
    return {
        {"sessions", Json::object()}, {"receipts", Json::object()}, {"configs", Json::object()}};
  }
  return Json::parse(control_decrypt(found->second.at("state").value, key));
}
Json payload(const ControlOperation& operation) {
  return std::visit(
      [](const auto& value) -> Json {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ControlLogin>) {
          return {{"username", lower(value.username)}, {"password", value.password}};
        } else if constexpr (std::is_same_v<T, ControlRefresh>) {
          return {{"refresh_token", value.refresh_token}};
        } else if constexpr (std::is_same_v<T, ControlRevokeSession>) {
          return {{"resource", value.session_id}};
        } else if constexpr (std::is_same_v<T, ControlSetPluginEnabled>) {
          return {{"resource", value.plugin_id}, {"enabled", value.enabled}};
        } else if constexpr (std::is_same_v<T, ControlWritePluginConfig>) {
          return {{"resource", value.plugin_id},
                  {"expected_version", value.expected_version},
                  {"values", control_json(value.values)}};
        } else if constexpr (std::is_same_v<T, ControlValidatePlugin>) {
          return control_json(value.manifest);
        } else {
          return Json::object();
        }
      },
      operation);
}
Json cached_reply(const ControlReply& reply) {
  return std::visit(
      [](const auto& value) -> Json {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ControlTokenPair>) {
          return {{"access_token", value.access_token},
                  {"refresh_token", value.refresh_token},
                  {"expires_in", value.expires_in}};
        } else if constexpr (std::is_same_v<T, ControlPluginConfig>) {
          return {{"version", value.version}, {"values", control_json(value.values)}};
        } else if constexpr (std::is_same_v<T, ControlValidation>) {
          return {{"valid", value.valid}, {"errors", value.errors}};
        } else {
          return Json::object();
        }
      },
      reply);
}
ControlReply replay(const ControlOperation& operation, const Json& body) {
  if (std::holds_alternative<ControlLogin>(operation) ||
      std::holds_alternative<ControlRefresh>(operation)) {
    return ControlTokenPair{body.at("access_token"), body.at("refresh_token"), 900};
  }
  if (std::holds_alternative<ControlWritePluginConfig>(operation)) {
    return ControlPluginConfig{body.at("version"), control_struct(body.at("values"))};
  }
  if (std::holds_alternative<ControlValidatePlugin>(operation)) {
    return ControlValidation{body.at("valid"), body.at("errors").get<std::vector<std::string>>()};
  }
  if (std::holds_alternative<ControlRevokeSession>(operation)) {
    return ControlNoContent{};
  }
  return ControlAccepted{};
}
bool sensitive(const Json& value) {
  if (value.is_object()) {
    for (const auto& [key, child] : value.items()) {
      const auto name = lower(key);
      if (name.find("token") != std::string::npos || name.find("secret") != std::string::npos ||
          name.find("password") != std::string::npos || name.find("api_key") != std::string::npos ||
          name.find("credential") != std::string::npos || sensitive(child)) {
        return true;
      }
    }
  } else if (value.is_array()) {
    return std::any_of(value.begin(), value.end(), sensitive);
  }
  return false;
}
std::string plugin_state(const RuntimeSnapshot& snapshot, const std::string& id) {
  if (!control_plugin_enabled(snapshot, id)) {
    return "PLUGIN_STATE_DISABLED";
  }
  const auto found = snapshot.plugins.find(id);
  if (found == snapshot.plugins.end()) {
    return "PLUGIN_STATE_STOPPED";
  }
  switch (found->second.state) {
  case PluginState::ready:
    return "PLUGIN_STATE_READY";
  case PluginState::running:
    return "PLUGIN_STATE_RUNNING";
  case PluginState::unavailable:
    return "PLUGIN_STATE_UNAVAILABLE";
  case PluginState::failed:
    return "PLUGIN_STATE_FAILED";
  }
  return "PLUGIN_STATE_UNSPECIFIED";
}
} // namespace
ControlService::ControlService(RuntimeStore& store, ControlOptions options, RuntimeClock clock)
    : store_(store), options_(std::move(options)), clock_(std::move(clock)) {
  if (!clock_) {
    clock_ = [] { return std::chrono::system_clock::now(); };
  }
  (void)control_digest("configuration", options_.encryption_key);
  std::set<std::string> ids;
  std::set<std::string> names;
  if (options_.users.empty()) {
    throw std::invalid_argument("control users required");
  }
  for (auto& user : options_.users) {
    user.username = lower(user.username);
    if (user.id.empty() || user.id.size() > 128 || user.username.size() > 128 ||
        !std::regex_match(user.username, std::regex("[a-z0-9_.-]+")) ||
        !user.password_hash.starts_with("$argon2id$") ||
        !control_role_allows(user.role, "session.read") || !ids.insert(user.id).second ||
        !names.insert(user.username).second) {
      throw std::invalid_argument("invalid control user");
    }
  }
  ids.clear();
  for (const auto& plugin : options_.plugins) {
    for (const auto* field : {"id", "name", "version", "api_version", "runtime"}) {
      if (!plugin.contains(field) || !plugin.at(field).is_string() ||
          plugin.at(field).get<std::string>().empty()) {
        throw std::invalid_argument("incomplete public plugin manifest");
      }
    }
    const auto id = plugin.at("id").get<std::string>();
    if (!std::regex_match(id, std::regex("[a-z0-9][a-z0-9.-]{1,63}")) || !ids.insert(id).second ||
        sensitive(plugin) || plugin.at("runtime") != "python-rpc") {
      throw std::invalid_argument("invalid public plugin manifest");
    }
  }
  const auto state = store_.read();
  if (std::holds_alternative<StorageError>(state)) {
    throw std::runtime_error("control store unavailable");
  }
  (void)load(std::get<RuntimeSnapshot>(state), options_.encryption_key);
}
PortStatus ControlService::readiness() {
  std::lock_guard lock(mutex_);
  try {
    auto begun = store_.begin();
    if (std::holds_alternative<StorageError>(begun)) {
      return error(wire::UPSTREAM_UNAVAILABLE, {});
    }
    auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(begun));
    if (std::holds_alternative<StorageError>(transaction->commit())) {
      return error(wire::UPSTREAM_UNAVAILABLE, {});
    }
    return std::monostate{};
  } catch (...) {
    return error(wire::INTERNAL, {});
  }
}
PortResult<ControlReply> ControlService::invoke(const ControlRequest& request) {
  std::lock_guard lock(mutex_);
  try {
    return invoke_locked(request);
  } catch (...) {
    return error(wire::INTERNAL, request.metadata);
  }
}
PortResult<ControlReply> ControlService::invoke_locked(const ControlRequest& request) {
  const auto now = clock_();
  const auto epoch = seconds(now);
  const auto& meta = request.metadata;
  const auto policy = control_policy(request.operation);
  auto begun = store_.begin();
  if (const auto* failure = std::get_if<StorageError>(&begun)) {
    return std::holds_alternative<ControlHealth>(request.operation)
               ? error(wire::UPSTREAM_UNAVAILABLE, meta)
               : mapped(*failure, meta);
  }
  auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(begun));
  auto& snapshot = transaction->snapshot();
  auto state = load(snapshot, options_.encryption_key);
  auto& sessions = state.at("sessions");
  auto& receipts = state.at("receipts");
  for (auto entry = sessions.begin(); entry != sessions.end();) {
    if (entry.value().at("expires").get<std::int64_t>() <= epoch) {
      entry = sessions.erase(entry);
    } else {
      ++entry;
    }
  }
  std::string actor = "anonymous";
  std::string resource;
  auto finish_error = [&](wire::ErrorCode code) -> PortResult<ControlReply> {
    if (snapshot.audit.size() >= options_.audit_limit) {
      return error(wire::UPSTREAM_UNAVAILABLE, meta);
    }
    snapshot.audit.push_back({actor, std::string(policy.operation_id), resource, meta.request_id,
                              meta.trace_id, now, false, wire::ErrorCode_Name(code)});
    const auto committed = transaction->commit();
    if (const auto* failure = std::get_if<StorageError>(&committed)) {
      return mapped(*failure, meta);
    }
    return error(code, meta);
  };
  if (const auto invalid = validate_control_request(request, now)) {
    return finish_error(invalid->code());
  }
  const ControlUser* user = nullptr;
  std::string session_id;
  std::string credential_digest;
  if (const auto* login = std::get_if<ControlLogin>(&request.operation)) {
    const auto name = lower(login->username);
    // Bound attacker-controlled rate-limit keys; one overflow bucket is fail closed.
    std::erase_if(login_attempts_, [&](const auto& entry) {
      return entry.second.first + std::chrono::minutes(1) <= now;
    });
    auto& attempt = login_attempts_[login_attempts_.size() < 1024 || login_attempts_.contains(name)
                                        ? name
                                        : "$overflow"];
    if (attempt.first == WallTime{}) {
      attempt.first = now;
    }
    if (++attempt.second > 30) {
      return finish_error(wire::RATE_LIMITED);
    }
    for (const auto& candidate : options_.users) {
      if (candidate.username == name) {
        user = &candidate;
        break;
      }
    }
    // Verify an Argon2 hash even for an unknown username.
    const auto& hash = user ? user->password_hash : options_.users.front().password_hash;
    const bool verified = control_password_verify(hash, login->password);
    if (!user || !verified) {
      return finish_error(wire::UNAUTHENTICATED);
    }
    actor = user->id;
  } else if (const auto* refresh = std::get_if<ControlRefresh>(&request.operation)) {
    credential_digest = control_digest(refresh->refresh_token, options_.encryption_key);
    for (const auto& [id, session] : sessions.items()) {
      if (session.at("refresh") == credential_digest) {
        session_id = id;
        break;
      }
    }
    if (session_id.empty()) {
      for (const auto& [id, receipt] : receipts.items()) {
        (void)id;
        if (receipt.value("credential", "") == credential_digest &&
            receipt.at("expires").get<std::int64_t>() > epoch) {
          session_id = receipt.at("session");
          break;
        }
      }
    }
  } else {
    credential_digest = control_digest(meta.access_token, options_.encryption_key);
    for (const auto& [id, session] : sessions.items()) {
      if (session.at("access") == credential_digest &&
          session.at("access_expires").get<std::int64_t>() > epoch) {
        session_id = id;
        break;
      }
    }
  }
  if (!std::holds_alternative<ControlLogin>(request.operation)) {
    if (session_id.empty() || !sessions.contains(session_id)) {
      return finish_error(wire::UNAUTHENTICATED);
    }
    const auto& session = sessions.at(session_id);
    if (session.at("revoked").get<bool>() || session.at("expires").get<std::int64_t>() <= epoch) {
      return finish_error(wire::UNAUTHENTICATED);
    }
    for (const auto& candidate : options_.users) {
      if (session.at("user") == candidate.id) {
        user = &candidate;
        break;
      }
    }
    if (!user) {
      return finish_error(wire::UNAUTHENTICATED);
    }
    actor = user->id;
    if (!std::holds_alternative<ControlRefresh>(request.operation) &&
        !control_role_allows(user->role, policy.permission)) {
      return finish_error(wire::PERMISSION_DENIED);
    }
  }
  const auto input = payload(request.operation);
  resource = input.value("resource", "");
  const auto fingerprint = control_digest(input.dump(), options_.encryption_key);
  const auto scope = control_digest(
      Json::array({std::holds_alternative<ControlRefresh>(request.operation) ? session_id
                   : std::holds_alternative<ControlLogin>(request.operation)
                       ? lower(std::get<ControlLogin>(request.operation).username)
                       : actor,
                   policy.operation_id, resource, meta.idempotency_key})
          .dump(),
      options_.encryption_key);
  for (auto it = receipts.begin(); it != receipts.end();) {
    if (it.value().at("expires").get<std::int64_t>() <= epoch) {
      it = receipts.erase(it);
    } else {
      ++it;
    }
  }
  if (policy.mutating && receipts.contains(scope)) {
    const auto& receipt = receipts.at(scope);
    if (receipt.at("fingerprint") != fingerprint) {
      return finish_error(wire::CONFLICT);
    }
    if (receipt.contains("session")) {
      const auto id = receipt.at("session").get<std::string>();
      if (!sessions.contains(id) || sessions.at(id).at("revoked").get<bool>() ||
          sessions.at(id).at("expires").get<std::int64_t>() <= epoch ||
          sessions.at(id).at("generation") != receipt.at("generation") ||
          sessions.at(id).at("access_expires").get<std::int64_t>() <= epoch) {
        return finish_error(wire::UNAUTHENTICATED);
      }
    }
    return replay(request.operation, receipt.at("reply"));
  }
  if (policy.mutating && snapshot.audit.size() >= options_.audit_limit) {
    return error(wire::UPSTREAM_UNAVAILABLE, meta);
  }
  ControlReply reply = ControlNoContent{};
  bool token_reply = false;
  if (std::holds_alternative<ControlLogin>(request.operation) ||
      std::holds_alternative<ControlRefresh>(request.operation)) {
    const bool login = std::holds_alternative<ControlLogin>(request.operation);
    if (!login && sessions.at(session_id).at("refresh") != credential_digest) {
      return finish_error(wire::UNAUTHENTICATED);
    }
    if (login) {
      if (sessions.size() >= 4096) {
        return finish_error(wire::RATE_LIMITED);
      }
      session_id = control_secret();
      sessions[session_id] = {{"user", user->id},
                              {"created", epoch},
                              {"expires", epoch + 2592000},
                              {"revoked", false},
                              {"generation", 0}};
    }
    auto& session = sessions.at(session_id);
    const auto access = control_secret();
    const auto refresh = control_secret();
    session["access"] = control_digest(access, options_.encryption_key);
    session["refresh"] = control_digest(refresh, options_.encryption_key);
    session["access_expires"] = epoch + 900;
    session["generation"] = session.at("generation").get<unsigned>() + 1;
    reply = ControlTokenPair{access, refresh, 900};
    token_reply = true;
  } else if (std::holds_alternative<ControlPrincipalQuery>(request.operation)) {
    reply = ControlPrincipal{user->id, session_id, {user->role}, control_permissions(user->role)};
  } else if (const auto* revoke = std::get_if<ControlRevokeSession>(&request.operation)) {
    if (!sessions.contains(revoke->session_id) ||
        (user->role != "admin" && sessions.at(revoke->session_id).at("user") != user->id)) {
      return finish_error(wire::NOT_FOUND);
    }
    sessions[revoke->session_id]["revoked"] = true;
  } else if (const auto* list = std::get_if<ControlList>(&request.operation)) {
    std::erase_if(pages_, [&](const auto& entry) { return entry.second.expires <= now; });
    const auto binding =
        Json::array({user->id, user->role, list->resource, list->page_size}).dump();
    PageSnapshot page{binding, now + std::chrono::minutes(10), {}, 0};
    if (list->page_token.empty()) {
      page.items = resources(list->resource, snapshot, state, *user);
    } else {
      const auto found = pages_.find(list->page_token);
      if (found == pages_.end() || found->second.binding != binding) {
        return finish_error(wire::INVALID_ARGUMENT);
      }
      page = found->second;
    }
    ControlPage result{list->resource, {}, list->page_size, std::nullopt};
    const auto end = std::min(page.items.size(), page.offset + list->page_size);
    for (auto index = page.offset; index < end; ++index) {
      result.items.push_back(control_struct(page.items[index]));
    }
    if (end < page.items.size()) {
      if (pages_.size() >= 1024) {
        return finish_error(wire::RATE_LIMITED);
      }
      const auto cursor = control_secret();
      page.offset = end;
      pages_[cursor] = std::move(page);
      result.next_page_token = cursor;
    }
    reply = std::move(result);
  } else if (const auto* validate = std::get_if<ControlValidatePlugin>(&request.operation)) {
    std::vector<std::string> errors;
    const auto& manifest = validate->manifest;
    if (manifest.name().empty() || manifest.version().empty()) {
      errors.emplace_back("INVALID_ARGUMENT");
    }
    if (manifest.api_version() != "kiarabot.plugin.v1" || manifest.runtime() != "python-rpc" ||
        (!manifest.isolation().empty() && manifest.isolation() != "process")) {
      errors.emplace_back("CAPABILITY_NOT_SUPPORTED");
    }
    reply = ControlValidation{errors.empty(), errors};
  } else if (const auto* health = std::get_if<ControlHealth>(&request.operation)) {
    ControlHealthResult result{wire::HEALTHY, {{"storage", wire::HEALTHY, "readable"}}};
    if (!health->plugin_id) {
      if (!options_.adapter_id.empty()) {
        const bool connected = options_.adapter_connected && options_.adapter_connected();
        result.components.push_back({"adapter", connected ? wire::HEALTHY : wire::UNHEALTHY,
                                     connected ? "connected" : "disconnected"});
        if (!connected) {
          result.state = wire::DEGRADED;
        }
      }
      for (const auto& plugin : resources(ControlResource::Plugins, snapshot, state, *user)) {
        const bool running = plugin.at("state") == "PLUGIN_STATE_RUNNING";
        const bool disabled = plugin.at("state") == "PLUGIN_STATE_DISABLED";
        if (!disabled) {
          result.components.push_back(
              {plugin.at("id"), running ? wire::HEALTHY : wire::DEGRADED, plugin.at("state")});
          if (!running) {
            result.state = wire::DEGRADED;
          }
        }
      }
    }
    if (health->plugin_id) {
      const auto plugins = resources(ControlResource::Plugins, snapshot, state, *user);
      const auto found = std::find_if(plugins.begin(), plugins.end(), [&](const auto& value) {
        return value.at("id") == *health->plugin_id;
      });
      if (found == plugins.end()) {
        return finish_error(wire::NOT_FOUND);
      }
      result.state = found->at("state") == "PLUGIN_STATE_RUNNING" ? wire::HEALTHY : wire::DEGRADED;
      result.components = {{*health->plugin_id, result.state, found->at("state")}};
    }
    if (clock_() >= meta.deadline) {
      return error(wire::TIMEOUT, meta);
    }
    if (std::holds_alternative<StorageError>(transaction->commit())) {
      return error(wire::UPSTREAM_UNAVAILABLE, meta);
    }
    reply = result;
  } else if (std::holds_alternative<ControlActionQuery>(request.operation)) {
    const auto& query = std::get<ControlActionQuery>(request.operation);
    const ActionEntry* entry = nullptr;
    for (const auto& [id, candidate] : snapshot.actions) {
      (void)id;
      if (candidate.record.owner == "control/" + user->id &&
          (query.by_idempotency_key ? candidate.record.idempotency_key
                                    : candidate.record.action_id) == query.selector) {
        entry = &candidate;
        break;
      }
    }
    if (!entry) {
      return finish_error(wire::NOT_FOUND);
    }
    wire::ActionResult result;
    result.set_action_id(entry->record.action_id);
    constexpr std::array statuses{wire::ACCEPTED, wire::SUCCEEDED, wire::FAILED, wire::EXPIRED,
                                  wire::CANCELLED};
    result.set_status(statuses.at(static_cast<std::size_t>(entry->state)));
    if (entry->state != ActionState::accepted) {
      result.mutable_completed_at()->set_seconds(seconds(entry->updated_at));
    }
    if (entry->state == ActionState::failed) {
      wire::ErrorCode code = wire::INTERNAL;
      if (!wire::ErrorCode_Parse(entry->error_code, &code)) {
        code = wire::INTERNAL;
      }
      *result.mutable_error() = error(code, meta);
    }
    result.set_platform_message_id(entry->result);
    reply = result;
  } else {
    const auto id = std::visit(
        [](const auto& value) -> std::string {
          if constexpr (requires { value.plugin_id; } &&
                        !std::is_same_v<std::decay_t<decltype(value)>, ControlHealth>) {
            return value.plugin_id;
          } else {
            return {};
          }
        },
        request.operation);
    const auto plugins = resources(ControlResource::Plugins, snapshot, state, *user);
    if (std::none_of(plugins.begin(), plugins.end(),
                     [&](const auto& value) { return value.at("id") == id; })) {
      return finish_error(wire::NOT_FOUND);
    }
    if (const auto* enable = std::get_if<ControlSetPluginEnabled>(&request.operation)) {
      snapshot.namespaces["$control.enabled"][id] = {enable->enabled ? "true" : "false", {}};
      reply = ControlAccepted{};
    } else {
      auto& configs = state.at("configs");
      Json config = configs.value(id, Json{{"version", "0"}, {"values", Json::object()}});
      if (const auto* write = std::get_if<ControlWritePluginConfig>(&request.operation)) {
        if (config.at("version") != write->expected_version) {
          return finish_error(wire::CONFLICT);
        }
        const auto values = control_json(write->values);
        if (sensitive(values)) {
          return finish_error(wire::INVALID_ARGUMENT);
        }
        config = {{"version", control_secret()}, {"values", values}};
        configs[id] = config;
      }
      ControlPluginConfig result{config.at("version"), control_struct(config.at("values"))};
      if (std::holds_alternative<ControlReadPluginConfig>(request.operation)) {
        result.delivery = options_.config_delivery && control_plugin_enabled(snapshot, id)
                              ? options_.config_delivery->status(id, result.version)
                              : ConfigDeliveryStatus{};
      }
      reply = std::move(result);
    }
  }
  if (clock_() >= meta.deadline) {
    return error(wire::TIMEOUT, meta);
  }
  if (!policy.mutating) {
    return reply;
  }
  if (receipts.size() >= 10000) {
    return error(wire::RATE_LIMITED, meta);
  }
  Json receipt{
      {"fingerprint", fingerprint}, {"expires", epoch + 86400}, {"reply", cached_reply(reply)}};
  if (token_reply) {
    receipt["session"] = session_id;
    receipt["generation"] = sessions.at(session_id).at("generation");
    if (std::holds_alternative<ControlRefresh>(request.operation)) {
      receipt["credential"] = credential_digest;
    }
  }
  receipts[scope] = receipt;
  snapshot.namespaces["$control"]["state"] = {
      control_encrypt(state.dump(), options_.encryption_key), {}};
  snapshot.audit.push_back({actor, std::string(policy.operation_id), resource, meta.request_id,
                            meta.trace_id, now, true, "OK"});
  const auto committed = transaction->commit();
  if (const auto* failure = std::get_if<StorageError>(&committed)) {
    return mapped(*failure, meta);
  }
  return reply;
}
std::vector<ControlJson> ControlService::resources(ControlResource resource,
                                                   const RuntimeSnapshot& snapshot,
                                                   const ControlJson& state,
                                                   const ControlUser& user) const {
  std::vector<Json> result;
  const auto now = seconds(clock_());
  if (resource == ControlResource::Sessions) {
    for (const auto& [id, session] : state.at("sessions").items()) {
      if (user.role != "admin" && session.at("user") != user.id) {
        continue;
      }
      result.push_back({{"session_id", id},
                        {"user_id", session.at("user")},
                        {"created_at", control_timestamp(WallTime(std::chrono::seconds(
                                           session.at("created").get<std::int64_t>())))},
                        {"expires_at", control_timestamp(WallTime(std::chrono::seconds(
                                           session.at("expires").get<std::int64_t>())))},
                        {"state", session.at("revoked").get<bool>()                  ? "REVOKED"
                                  : session.at("expires").get<std::int64_t>() <= now ? "EXPIRED"
                                                                                     : "ACTIVE"}});
    }
  } else if (resource == ControlResource::Plugins) {
    std::map<std::string, Json> manifests;
    for (const auto& plugin : options_.plugins) {
      manifests[plugin.at("id")] = plugin;
    }
    const auto registered = snapshot.namespaces.find("$control.manifests");
    if (registered != snapshot.namespaces.end()) {
      for (const auto& [id, entry] : registered->second) {
        manifests[id] = Json::parse(entry.value);
      }
    }
    for (auto& [id, manifest] : manifests) {
      // Only the public contract projection is permitted, never entrypoints/config schemas.
      Json item;
      for (const auto* field : {"id", "name", "version", "api_version", "runtime", "isolation",
                                "ordered", "permissions"}) {
        if (manifest.contains(field)) {
          item[field] = manifest.at(field);
        }
      }
      item["state"] = plugin_state(snapshot, id);
      result.push_back(std::move(item));
    }
  } else if (resource == ControlResource::Bots) {
    for (const auto& account : options_.accounts) {
      result.push_back({{"id", account}, {"name", account}, {"state", "CONFIGURED"}});
    }
  } else if (resource == ControlResource::Adapters && !options_.adapter_id.empty()) {
    result.push_back(
        {{"id", options_.adapter_id},
         {"implementation", "nonebot2"},
         {"state",
          options_.adapter_connected && options_.adapter_connected() ? "HEALTHY" : "UNHEALTHY"}});
  } else if (resource == ControlResource::Models && !options_.model_name.empty()) {
    result.push_back({{"id", "default"}, {"name", options_.model_name}, {"state", "CONFIGURED"}});
  } else if (resource == ControlResource::Users) {
    for (const auto& entry : options_.users) {
      result.push_back({{"id", entry.id},
                        {"name", entry.username},
                        {"groups", Json::array()},
                        {"roles", Json::array({entry.role})},
                        {"permissions", control_permissions(entry.role)}});
    }
  } else if (resource == ControlResource::Audit || resource == ControlResource::Logs) {
    for (std::size_t index = 0; index < snapshot.audit.size(); ++index) {
      const auto& entry = snapshot.audit[index];
      const auto id = std::to_string(index);
      if (resource == ControlResource::Audit) {
        result.push_back({{"id", id},
                          {"occurred_at", control_timestamp(entry.occurred_at)},
                          {"actor", entry.actor},
                          {"action", entry.permission},
                          {"resource", entry.resource},
                          {"request_id", entry.request_id}});
      }
    }
  } else if (resource == ControlResource::Tasks) {
    for (const auto& [owner, tasks] : snapshot.tasks) {
      for (const auto& [id, task] : tasks) {
        constexpr std::array names{"UNSPECIFIED", "PENDING", "RUNNING",  "RETRYING",
                                   "COMPLETED",   "FAILED",  "CANCELLED"};
        result.push_back({{"id", owner + "/" + id},
                          {"name", id},
                          {"state", names.at(static_cast<std::size_t>(task.state))}});
      }
    }
  }
  if (resource == ControlResource::Logs && options_.logs) {
    for (const auto& record : options_.logs()) {
      result.push_back({{"id", record.id},
                        {"name", record.operation},
                        {"occurred_at", control_timestamp(record.occurred_at)},
                        {"level", record.level},
                        {"component", record.component},
                        {"message", record.operation + ": " + record.outcome},
                        {"trace_id", record.trace_id}});
    }
  }
  std::sort(result.begin(), result.end(), [&](const auto& left, const auto& right) {
    const auto sort_key = [&](const Json& item) -> std::pair<std::string, std::string> {
      if (resource == ControlResource::Sessions) {
        return {item.at("created_at"), item.at("session_id")};
      }
      if (resource == ControlResource::Audit || resource == ControlResource::Logs) {
        return {item.at("occurred_at"), item.at("id")};
      }
      return {"", item.at("id")};
    };
    return sort_key(left) < sort_key(right);
  });
  return result;
}
PortResult<ControlJson> ControlService::stream_snapshot(const ControlMetadata& metadata) {
  std::lock_guard lock(mutex_);
  try {
    const auto authenticated = invoke_locked({metadata, ControlPrincipalQuery{}});
    if (const auto* failure = std::get_if<wire::Error>(&authenticated)) {
      return *failure;
    }
    const auto& principal = std::get<ControlPrincipal>(std::get<ControlReply>(authenticated));
    const auto read = store_.read();
    if (const auto* failure = std::get_if<StorageError>(&read)) {
      return mapped(*failure, metadata);
    }
    const auto& snapshot = std::get<RuntimeSnapshot>(read);
    const auto state = load(snapshot, options_.encryption_key);
    const auto found = std::find_if(options_.users.begin(), options_.users.end(),
                                    [&](const auto& user) { return user.id == principal.user_id; });
    if (found == options_.users.end()) {
      // Defense in depth: readiness(), stream_snapshot() and invoke() share this
      // state, so a session that outlives a user-table change must not be
      // dereferenced after the user lookup. Authentication already rejects it.
      return error(wire::UNAUTHENTICATED, metadata);
    }
    Json result{
        {"user_id", principal.user_id},
        {"session_id", principal.session_id},
        {"permissions", principal.permissions},
        {"health", Json::array({Json{{"state", "HEALTHY"},
                                     {"components", Json::array({Json{{"component", "storage"},
                                                                      {"state", "HEALTHY"}}})}}})}};
    for (const auto& [name, resource] : std::array{std::pair{"adapters", ControlResource::Adapters},
                                                   std::pair{"plugins", ControlResource::Plugins},
                                                   std::pair{"tasks", ControlResource::Tasks},
                                                   std::pair{"logs", ControlResource::Logs}}) {
      if (control_role_allows(found->role,
                              control_policy(ControlList{resource, 20, {}}).permission)) {
        result[name] = resources(resource, snapshot, state, *found);
        if (resource == ControlResource::Plugins) {
          for (auto& item : result[name]) {
            const auto config = state.at("configs").find(item.at("id").get<std::string>());
            item["_config_version"] = config == state.at("configs").end()
                                          ? "0"
                                          : config->at("version").get<std::string>();
          }
        }
      }
    }
    return result;
  } catch (...) {
    return error(wire::INTERNAL, metadata);
  }
}
} // namespace kiarabot::core
