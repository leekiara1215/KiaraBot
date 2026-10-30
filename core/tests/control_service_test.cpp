#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "kiarabot/core/runtime/in_memory_runtime_store.hpp"
#include "kiarabot/control/http_mapper.hpp"
#include "kiarabot/core/application/control_service.hpp"
#include "kiarabot/core/application/plugin_hub.hpp"
#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"

namespace {
using namespace std::chrono_literals;
namespace core = kiarabot::core;
namespace wire = kiarabot::v1;
using Json = core::ControlJson;
void check(bool condition, std::source_location where = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("assertion line " + std::to_string(where.line()));
  }
}
struct Client {
  core::IControlApplication& app;
  core::WallTime& now;
  std::string access;
  unsigned key{0};
  core::PortResult<core::ControlReply> call(core::ControlOperation op,
                                            std::string idempotency = {}) {
    if (idempotency.empty()) {
      idempotency = "test-idempotency-" + std::to_string(++key);
    }
    return app.invoke({{access, "request", "trace", idempotency, now + 30s}, std::move(op)});
  }
};
template <typename T> T value(const core::PortResult<core::ControlReply>& result) {
  if (const auto* failure = std::get_if<wire::Error>(&result)) {
    throw std::runtime_error("unexpected error " + wire::ErrorCode_Name(failure->code()));
  }
  return std::get<T>(std::get<core::ControlReply>(result));
}
void code(const core::PortResult<core::ControlReply>& result, wire::ErrorCode expected) {
  check(std::holds_alternative<wire::Error>(result));
  check(std::get<wire::Error>(result).code() == expected);
}
core::ControlOptions options() {
  const auto hash = core::control_password_hash("private-test-password");
  core::ControlOptions result;
  result.encryption_key = std::string(64, 'a');
  result.users = {{"admin-id", "admin", hash, "admin"}, {"viewer-id", "viewer", hash, "viewer"}};
  result.plugins = {Json{{"id", "test.plugin"},
                         {"name", "test"},
                         {"version", "1"},
                         {"api_version", "kiarabot.plugin.v1"},
                         {"runtime", "python-rpc"}}};
  return result;
}
void configuration_suite(core::RuntimeStore& store) {
  auto now = core::WallTime(1700000000s);
  auto settings = options();
  auto delivery = std::make_shared<core::PluginConfigDelivery>(store, settings.encryption_key,
                                                               [&] { return now; });
  settings.config_delivery = delivery;
  core::ControlService app(store, settings, [&] { return now; });
  Client client{app, now, {}, 0};
  client.access = value<core::ControlTokenPair>(
                      client.call(core::ControlLogin{"admin", "private-test-password"}))
                      .access_token;
  core::PluginHub hub(store, {{"test.plugin", {}}}, [&] { return now; });
  hub.set_configuration(delivery);
  wire::RegisterPluginRequest request;
  request.set_instance_id("instance");
  auto& manifest = *request.mutable_manifest();
  manifest.set_id("test.plugin");
  manifest.set_name("test");
  manifest.set_version("1");
  manifest.set_api_version("kiarabot.plugin.v1");
  manifest.set_runtime("python-rpc");
  manifest.set_isolation("process");
  manifest.set_entrypoint("example:create");
  auto read = [&] {
    return value<core::ControlPluginConfig>(
        client.call(core::ControlReadPluginConfig{"test.plugin"}));
  };
  auto write = [&](int number) {
    return value<core::ControlPluginConfig>(client.call(core::ControlWritePluginConfig{
        "test.plugin", read().version, core::control_struct(Json{{"number", number}})}));
  };
  check(read().delivery->state == "UNAVAILABLE");
  auto legacy = hub.attach("test.plugin", request);
  check(legacy.response.accepted() && !legacy.session.config_enabled);
  check(read().delivery->state == "UNAVAILABLE");
  check(std::holds_alternative<std::monostate>(hub.detach(legacy.session)));
  request.add_supported_features("plugin.config.v1");
  auto registration = hub.attach("test.plugin", request);
  check(registration.response.accepted() && registration.session.config_enabled);
  auto session = registration.session;
  auto next = [&] { return std::get<std::optional<wire::PluginConfig>>(hub.next_config(session)); };
  auto ack = [](const wire::PluginConfig& frame) {
    wire::PluginConfigAck result;
    result.set_delivery_id(frame.delivery_id());
    result.set_version(frame.version());
    result.set_accepted(true);
    return result;
  };
  auto initial = *next();
  check(initial.version() == "0" && initial.values().fields().empty());
  check(read().delivery->state == "PENDING" && !delivery->ready("test.plugin", session.fence));
  check(!next());
  check(std::holds_alternative<std::monostate>(hub.acknowledge_config(session, ack(initial))));
  check(read().delivery->state == "APPLIED" && read().delivery->applied_version == "0");
  const auto first = write(1);
  check(!first.delivery && read().delivery->state == "PENDING");
  auto frame = *next();
  const auto newest = write(2); // Do not publish a second in-flight frame.
  check(!next());
  check(std::holds_alternative<std::monostate>(hub.acknowledge_config(session, ack(frame))));
  check(read().delivery->state == "PENDING" && read().delivery->applied_version == first.version);
  auto latest = *next();
  check(latest.version() == newest.version);
  check(std::holds_alternative<std::monostate>(hub.acknowledge_config(session, ack(frame))));
  check(read().delivery->state == "PENDING");
  auto rejection = ack(latest);
  rejection.set_accepted(false);
  rejection.mutable_error()->set_code(wire::INVALID_ARGUMENT);
  rejection.mutable_error()->set_message("secret should not escape");
  (*rejection.mutable_error()->mutable_details())["secret"] = "hidden";
  check(std::holds_alternative<std::monostate>(hub.acknowledge_config(session, rejection)));
  const auto failed = read();
  check(failed.delivery->state == "REJECTED" &&
        failed.delivery->error->message() == "INVALID_ARGUMENT");
  check(failed.delivery->error->details().empty() && !next());
  check(!delivery->ready("test.plugin", session.fence));
  write(3);
  auto expired = *next();
  now += 11s;
  check(read().delivery->state == "REJECTED" && read().delivery->error->code() == wire::TIMEOUT);
  check(std::holds_alternative<std::monostate>(hub.acknowledge_config(session, ack(expired))));
  check(read().delivery->state == "REJECTED");
  write(4);
  auto recovery = *next();
  check(std::holds_alternative<std::monostate>(hub.acknowledge_config(session, ack(expired))));
  check(read().delivery->state == "PENDING");
  auto invalid = ack(recovery);
  invalid.set_version("forged");
  check(std::get<wire::Error>(hub.acknowledge_config(session, invalid)).code() ==
        wire::INVALID_ARGUMENT);
  check(std::holds_alternative<std::monostate>(hub.acknowledge_config(session, ack(recovery))));
  check(std::holds_alternative<std::monostate>(hub.acknowledge_config(session, ack(recovery))));
  check(read().delivery->state == "APPLIED");
  auto old = session;
  check(std::holds_alternative<std::monostate>(hub.detach(session)));
  check(read().delivery->state == "UNAVAILABLE");
  registration = hub.attach("test.plugin", request);
  check(registration.response.accepted());
  session = registration.session;
  check(std::get<wire::Error>(hub.acknowledge_config(old, ack(recovery))).code() ==
        wire::PERMISSION_DENIED);
  auto reconnect = *next();
  check(reconnect.version() == recovery.version() &&
        reconnect.delivery_id() != recovery.delivery_id());
  check(read().delivery->applied_version.empty());
  value<core::ControlAccepted>(client.call(core::ControlSetPluginEnabled{"test.plugin", false}));
  check(read().delivery->state == "UNAVAILABLE");
  check(std::holds_alternative<std::monostate>(hub.detach(session)));
}

void suite(core::RuntimeStore& store) {
  auto now = core::WallTime(1700000000s);
  auto settings = options();
  core::ControlService app(store, settings, [&] { return now; });
  Client client{app, now, {}, 0};
  code(client.call(core::ControlPrincipalQuery{}), wire::UNAUTHENTICATED);
  code(client.call(core::ControlLogin{"admin", "wrong"}), wire::UNAUTHENTICATED);
  const auto login = core::ControlLogin{"AdMiN", "private-test-password"};
  auto pair = value<core::ControlTokenPair>(client.call(login, "login-idempotent-1"));
  check(value<core::ControlTokenPair>(client.call(login, "login-idempotent-1")).access_token ==
        pair.access_token);
  client.access = pair.access_token;
  const auto principal = value<core::ControlPrincipal>(client.call(core::ControlPrincipalQuery{}));
  check(principal.roles == std::vector<std::string>{"admin"});
  auto viewer = value<core::ControlTokenPair>(
      client.call(core::ControlLogin{"viewer", "private-test-password"}));
  Client other{app, now, viewer.access_token, 100};
  const auto audit_before = std::get<core::RuntimeSnapshot>(store.read()).audit.size();
  code(other.call(core::ControlWritePluginConfig{"test.plugin", "0",
                                                 core::control_struct(Json{{"enabled", true}})}),
       wire::PERMISSION_DENIED);
  code(other.call(core::ControlSetPluginEnabled{"test.plugin", false}), wire::PERMISSION_DENIED);
  code(other.call(core::ControlRevokeSession{principal.session_id}), wire::NOT_FOUND);
  check(std::get<core::RuntimeSnapshot>(store.read()).audit.size() == audit_before + 3);
  const auto first = value<core::ControlPage>(
      client.call(core::ControlList{core::ControlResource::Sessions, 1, {}}));
  check(first.next_page_token.has_value());
  code(other.call(core::ControlList{core::ControlResource::Sessions, 1, *first.next_page_token}),
       wire::INVALID_ARGUMENT);
  code(client.call(core::ControlList{core::ControlResource::Sessions, 2, *first.next_page_token}),
       wire::INVALID_ARGUMENT);
  const auto config = value<core::ControlPluginConfig>(
      client.call(core::ControlWritePluginConfig{"test.plugin", "0",
                                                 core::control_struct(Json{{"enabled", true}})},
                  "config-idempotent-1"));
  check(config.version != "0");
  check(
      value<core::ControlPluginConfig>(
          client.call(core::ControlWritePluginConfig{"test.plugin", "0",
                                                     core::control_struct(Json{{"enabled", true}})},
                      "config-idempotent-1"))
          .version == config.version);
  code(client.call(core::ControlWritePluginConfig{"test.plugin", "0", {}}), wire::CONFLICT);
  code(
      client.call(core::ControlWritePluginConfig{
          "test.plugin", config.version, core::control_struct(Json{{"api_key", "do-not-expose"}})}),
      wire::INVALID_ARGUMENT);
  value<core::ControlAccepted>(client.call(core::ControlSetPluginEnabled{"test.plugin", false}));
  check(
      !core::control_plugin_enabled(std::get<core::RuntimeSnapshot>(store.read()), "test.plugin"));
  const auto refreshed = value<core::ControlTokenPair>(
      client.call(core::ControlRefresh{pair.refresh_token}, "refresh-idempotent-1"));
  code(client.call(core::ControlPrincipalQuery{}), wire::UNAUTHENTICATED);
  client.access = refreshed.access_token;
  check(value<core::ControlTokenPair>(
            client.call(core::ControlRefresh{pair.refresh_token}, "refresh-idempotent-1"))
            .access_token == refreshed.access_token);
  code(client.call(core::ControlRefresh{pair.refresh_token}), wire::UNAUTHENTICATED);
  code(client.call(login, "login-idempotent-1"), wire::UNAUTHENTICATED);
  // A fresh application instance must recover tokens, receipts and configuration.
  core::ControlService restarted(store, settings, [&] { return now; });
  Client restored{restarted, now, refreshed.access_token, 500};
  check(
      value<core::ControlPluginConfig>(restored.call(core::ControlReadPluginConfig{"test.plugin"}))
          .version == config.version);
  check(value<core::ControlPrincipal>(restored.call(core::ControlPrincipalQuery{})).session_id ==
        principal.session_id);
  const auto durable = std::get<core::RuntimeSnapshot>(store.read());
  for (const auto& [name, values] : durable.namespaces) {
    (void)name;
    for (const auto& [key, entry] : values) {
      (void)key;
      check(entry.value.find(refreshed.access_token) == std::string::npos);
      check(entry.value.find("private-test-password") == std::string::npos);
    }
  }
  now += 601s;
  code(client.call(core::ControlList{core::ControlResource::Sessions, 1, *first.next_page_token}),
       wire::INVALID_ARGUMENT);
  value<core::ControlNoContent>(restored.call(core::ControlRevokeSession{principal.session_id}));
  code(client.call(core::ControlPrincipalQuery{}), wire::UNAUTHENTICATED);
  code(client.call(core::ControlRefresh{pair.refresh_token}, "refresh-idempotent-1"),
       wire::UNAUTHENTICATED);
  now += 901s;
  code(other.call(core::ControlPrincipalQuery{}), wire::UNAUTHENTICATED);
}
class FailingApplication final : public core::IControlApplication {
public:
  wire::ErrorCode result{wire::TIMEOUT};
  core::PortResult<core::ControlReply> invoke(const core::ControlRequest&) override {
    wire::Error failure;
    failure.set_code(result);
    return failure;
  }
};
void faults() {
  core::InMemoryRuntimeStore store;
  auto now = core::WallTime(1700000000s);
  core::ControlService app(store, options(), [&] { return now; });
  Client client{app, now, {}, 0};
  store.set_fail_commits(true);
  code(client.call(core::ControlLogin{"admin", "private-test-password"}),
       wire::UPSTREAM_UNAVAILABLE);
  check(std::get<core::RuntimeSnapshot>(store.read()).namespaces.empty());
  store.set_fail_commits(false);
  store.set_fail_reads(true);
  code(client.call(core::ControlPrincipalQuery{}), wire::UPSTREAM_UNAVAILABLE);
  store.set_fail_reads(false);
  code(app.invoke({{"", "request", "trace", "timeout-idempotent", now},
                   core::ControlLogin{"admin", "private-test-password"}}),
       wire::TIMEOUT);
  FailingApplication fake;
  for (const auto& [error, status] :
       std::array{std::pair{wire::TIMEOUT, 504}, std::pair{wire::UPSTREAM_UNAVAILABLE, 503},
                  std::pair{wire::RATE_LIMITED, 429}, std::pair{wire::PERMISSION_DENIED, 403},
                  std::pair{wire::CONFLICT, 409}}) {
    fake.result = error;
    const auto response = kiarabot::control::handle_http(fake, {"GET", "/api/v1/health", {}, ""});
    check(response.status == status);
    check(Json::parse(response.body).at("code") == wire::ErrorCode_Name(error));
    if (status == 429) {
      check(response.headers.contains("Retry-After"));
    }
  }
}
} // namespace
int main() try {
  core::InMemoryRuntimeStore memory;
  suite(memory);
  const auto path =
      std::filesystem::temp_directory_path() / ("control-" + core::control_secret() + ".db");
  {
    auto opened = kiarabot::infrastructure::SqliteRuntimeStore::open(path.string());
    auto store =
        std::get<std::unique_ptr<kiarabot::infrastructure::SqliteRuntimeStore>>(std::move(opened));
    suite(*store);
  }
  std::filesystem::remove(path);
  core::InMemoryRuntimeStore configuration_memory;
  configuration_suite(configuration_memory);
  {
    auto opened = kiarabot::infrastructure::SqliteRuntimeStore::open(path.string());
    auto store =
        std::get<std::unique_ptr<kiarabot::infrastructure::SqliteRuntimeStore>>(std::move(opened));
    configuration_suite(*store);
  }
  std::filesystem::remove(path);
  core::InMemoryRuntimeStore broken;
  core::PluginConfigDelivery configs(broken, std::string(64, 'a'));
  configs.attach("test.plugin", "fence");
  broken.set_fail_reads(true);
  check(std::get<wire::Error>(configs.next("test.plugin", "fence")).code() ==
        wire::UPSTREAM_UNAVAILABLE);
  check(configs.status("test.plugin", "0").state == "PENDING");
  faults();
  std::cout << "control service memory/SQLite/faults passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
