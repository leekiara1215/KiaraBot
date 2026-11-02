#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>

#include <sys/wait.h>

extern char** environ;

#include <pqxx/pqxx>

#include "kiarabot/core/application/target/runtime_storage_application.hpp"
#include "kiarabot/core/domain/target/namespace_rules.hpp"
#include "kiarabot/infrastructure/postgres/database.hpp"
#include "kiarabot/infrastructure/postgres/factories.hpp"

namespace pg = kiarabot::infrastructure::postgres;
namespace target = kiarabot::core::target;
using std::chrono_literals::operator""s;
namespace {
class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
void require(bool condition, const char* message) {
  if (!condition) {
    throw TestFailure(message);
  }
}
template <typename Value>
bool failed(const target::Outcome<Value>& result, target::ErrorCode code) {
  const auto* failure = std::get_if<target::Failure>(&result);
  return failure && failure->code == code;
}
std::string quote(std::string_view value) {
  std::string output = "'";
  for (const char character : value) {
    if (character == '\'' || character == '\\') {
      output += '\\';
    }
    output += character;
  }
  return output + '\'';
}
int initialize_process(const std::filesystem::path& executable, const pg::DatabaseConfig& config,
                       const std::filesystem::path& password_file) {
  const std::array<std::string, 10> arguments{
      executable.string(),   "--initialize-empty",        "runtime",
      config.host,           std::to_string(config.port), config.database,
      config.migration_role, config.runtime_role,         config.root_certificate,
      password_file.string()};
  std::array<char*, 11> argv{};
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    argv[index] = const_cast<char*>(arguments[index].c_str());
  }
  pid_t process = 0;
  require(::posix_spawn(&process, executable.c_str(), nullptr, nullptr, argv.data(), environ) == 0,
          "initializer process spawn failed");
  int status = 0;
  require(::waitpid(process, &status, 0) == process && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "runtime database initialization failed");
  return WEXITSTATUS(status);
}
std::string uuid(unsigned value) {
  std::string text = "00000000-0000-4000-8000-000000000000";
  constexpr char digits[] = "0123456789abcdef";
  std::string suffix = std::to_string(value);
  for (std::size_t index = 0; index < suffix.size(); ++index) {
    text[text.size() - 1 - index] = suffix[suffix.size() - 1 - index];
  }
  (void)digits;
  return text;
}
target::Sha256Digest digest(unsigned seed) {
  target::Sha256Digest value;
  value.bytes[0] = static_cast<std::byte>(seed);
  return value;
}
target::TimePoint base_now() {
  return target::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
class Authorizer final : public target::RuntimeAuthorizationPort {
public:
  bool allow = true;
  bool available() const noexcept override {
    return true;
  }
  target::Outcome<target::RuntimeAuthorizationDecision>
  check(const target::RuntimeAuthorizationRequest& request) override {
    if (!allow) {
      return target::Failure{target::ErrorCode::PermissionDenied, target::Acceptance::NotAccepted,
                             target::Recovery::ReadCurrent, std::nullopt, std::nullopt};
    }
    return target::RuntimeAuthorizationDecision{true,
                                                request.authentication.subject,
                                                request.permission,
                                                request.scope,
                                                {9},
                                                base_now(),
                                                std::nullopt};
  }
};
class Ids final : public target::RuntimeIdPort {
public:
  target::Outcome<target::ResourceId> account_id() override {
    return target::ResourceId{uuid(++seed_)};
  }
  target::Outcome<target::InstallationId> installation_id() override {
    return target::InstallationId{uuid(++seed_)};
  }
  target::Outcome<target::InstanceId> instance_id() override {
    return target::InstanceId{uuid(++seed_)};
  }
  target::Outcome<target::InstanceId> adapter_instance_id() override {
    return target::InstanceId{uuid(++seed_)};
  }
  target::Outcome<target::ConnectionId> connection_id() override {
    return target::ConnectionId{uuid(++seed_)};
  }
  target::Outcome<target::OperationId> operation_id() override {
    return target::OperationId{uuid(++seed_)};
  }
  target::Outcome<target::RuntimeEpoch> runtime_epoch() override {
    return target::RuntimeEpoch{"epoch"};
  }
  target::Outcome<std::string> snapshot_id() override {
    return uuid(++seed_);
  }

private:
  unsigned seed_ = 100;
};
target::AuthenticationContext authentication(const std::string& subject) {
  return {target::SubjectId{subject},
          target::PlatformPeer{target::Service::Control, target::SubjectId{subject}},
          target::Service::Runtime,
          {target::SubjectId{subject}},
          {{target::ResourceRef{target::ResourceKind::Catalog, {"catalog"}}, "storage.namespace"}},
          {9},
          base_now() - 1s,
          base_now() + 300s};
}
target::CommandContext management_context(const std::string& installation,
                                          const std::string& method, unsigned idem) {
  return {authentication(uuid(99)),
          uuid(1),
          uuid(2),
          method,
          {target::ResourceKind::Installation, {installation}},
          "0123456789abcdef" + std::to_string(idem),
          digest(1),
          base_now() + 120s,
          std::nullopt};
}
target::StorageCommandContext plugin_context(const std::string& instance, const std::string& method,
                                             unsigned idem, bool shared) {
  return {authentication(instance),
          uuid(3),
          uuid(4),
          method,
          "0123456789abcdef" + std::to_string(idem),
          digest(static_cast<unsigned char>(idem)),
          base_now() + 120s,
          {instance},
          shared ? target::RequestedScopeKind::InstallationShared
                 : target::RequestedScopeKind::DefaultPrivate,
          {}};
}
void run(std::uint16_t port, const std::filesystem::path& root,
         const std::filesystem::path& initializer) {
  std::ifstream admin_password_file(root / "password");
  std::string admin_password;
  std::getline(admin_password_file, admin_password);
  require(admin_password.size() == 64, "fixture administrator secret unavailable");
  const auto role_password = [&](const std::string& role) {
    std::ifstream input(root / ("password-" + role));
    std::string value;
    std::getline(input, value);
    require(value.size() == 64, "fixture role secret unavailable");
    return value;
  };
  pqxx::connection administrator(
      "host=localhost port=" + std::to_string(port) + " dbname=postgres user=p03_admin password=" +
      quote(admin_password) + " sslmode=verify-full channel_binding=require sslrootcert=" +
      quote((root / "server.pem").string()));
  {
    pqxx::nontransaction deployment(administrator);
    for (const auto& role : {"p03_runtime_migration", "p03_runtime_runtime"}) {
      deployment.exec("CREATE ROLE " + deployment.quote_name(role) +
                      " LOGIN NOINHERIT NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION "
                      "NOBYPASSRLS PASSWORD " +
                      deployment.quote(role_password(role)));
    }
    deployment.exec("CREATE DATABASE p03_runtime OWNER p03_runtime_migration");
    deployment.exec("REVOKE ALL ON DATABASE p03_runtime FROM PUBLIC");
    deployment.exec("GRANT CONNECT ON DATABASE p03_runtime TO p03_runtime_runtime");
  }
  pg::DatabaseConfig config{target::Service::Runtime,
                            "localhost",
                            port,
                            "p03_runtime",
                            "p03_runtime_runtime",
                            role_password("p03_runtime_runtime"),
                            (root / "server.pem").string(),
                            "p03_runtime_migration",
                            "p03_runtime_runtime"};
  require(initialize_process(initializer, config, root / "password-p03_runtime_migration") == 0,
          "empty Runtime database initialization failed");
  auto opened = pg::open_runtime(config);
  require(std::holds_alternative<std::unique_ptr<target::RuntimeUnitOfWorkFactory>>(opened),
          "runtime factory open");
  auto factory = std::move(std::get<std::unique_ptr<target::RuntimeUnitOfWorkFactory>>(opened));
  {
    auto begun = factory->begin();
    require(std::holds_alternative<std::unique_ptr<target::RuntimeUnitOfWork>>(begun),
            "seed UoW begin");
    auto work = std::move(std::get<std::unique_ptr<target::RuntimeUnitOfWork>>(begun));
    target::PluginInstallation installation;
    installation.id = {uuid(11)};
    installation.plugin_type = {"plugin"};
    installation.version = {1};
    require(std::holds_alternative<std::monostate>(
                work->installations().save(installation, std::nullopt)),
            "seed installation");
    target::PluginInstance instance;
    instance.id = {uuid(12)};
    instance.installation = {uuid(11)};
    instance.plugin_type = {"plugin"};
    instance.desired_version = {1};
    instance.applied_config_version = {1};
    instance.version = {1};
    require(std::holds_alternative<std::monostate>(work->instances().save(instance, std::nullopt)),
            "seed instance");
    require(work->commit() == target::CommitResult::Committed, "seed commit");
  }
  Ids ids;
  Authorizer authorizer;
  auto app_result = target::make_runtime_storage_application({*factory, ids, authorizer});
  require(std::holds_alternative<std::unique_ptr<target::RuntimeStorageApplication>>(app_result),
          "storage application composition");
  auto app = std::move(std::get<std::unique_ptr<target::RuntimeStorageApplication>>(app_result));
  const auto installation = uuid(11);
  const auto instance = uuid(12);
  // Private LWW write, get, CAS conflict, delete and receipt replay.
  auto context = plugin_context(instance, "SetStorage", 1, false);
  auto set = app->set_storage(
      {context, "alpha", {std::byte{1}, std::byte{2}}, std::nullopt, std::nullopt});
  require(std::holds_alternative<target::StorageEntryResult>(set), "private set");
  require(std::get<target::StorageEntryResult>(set).value.version.value == 1, "private set v1");
  auto replay = app->set_storage({context, "alpha", {std::byte{9}}, std::nullopt, std::nullopt});
  require(std::holds_alternative<target::StorageEntryResult>(replay) &&
              std::get<target::StorageEntryResult>(replay).value.bytes.size() == 2,
          "private receipt replay returns original value");
  auto get = app->get_storage({plugin_context(instance, "GetStorage", 2, false), "alpha"});
  require(std::holds_alternative<target::StorageEntryResult>(get) &&
              std::get<target::StorageEntryResult>(get).value.bytes.size() == 2,
          "private get");
  auto cas =
      app->compare_and_set_storage({plugin_context(instance, "CompareAndSetStorage", 3, false),
                                    "alpha",
                                    {std::byte{5}},
                                    target::ResourceVersion{99},
                                    std::nullopt});
  require(failed(cas, target::ErrorCode::Conflict), "CAS stale version conflicts");
  auto del = app->delete_storage(
      {plugin_context(instance, "DeleteStorage", 4, false), "alpha", std::nullopt});
  require(std::holds_alternative<target::StorageDeleteResult>(del) &&
              std::get<target::StorageDeleteResult>(del).deleted,
          "private delete");
  // Shared namespace management and plugin member gate.
  auto create = app->create_shared_namespace(
      {management_context(installation, "CreateSharedNamespace", 5), {installation}, std::nullopt});
  require(std::holds_alternative<target::NamespaceMutation>(create), "shared namespace create");
  const auto namespace_id =
      std::get<target::NamespaceMutation>(create).namespace_record.namespace_id;
  auto members = app->set_shared_members({management_context(installation, "SetSharedMembers", 6),
                                          namespace_id,
                                          {installation},
                                          {{target::InstanceId{instance}, true, true}},
                                          std::nullopt});
  require(std::holds_alternative<target::SharedMembersMutation>(members), "members set");
  auto plugin_shared = plugin_context(instance, "SetStorage", 7, true);
  auto shared_set = app->set_storage(
      {plugin_shared, "shared-key", {std::byte{3}}, std::nullopt, target::ExpectedAbsent{}});
  require(std::holds_alternative<target::StorageEntryResult>(shared_set), "shared set");
  auto shared_get =
      app->get_storage({plugin_context(instance, "GetStorage", 8, true), "shared-key"});
  require(std::holds_alternative<target::StorageEntryResult>(shared_get), "shared get");
  auto view = app->get_shared_namespace(
      {management_context(installation, "GetSharedNamespace", 9), namespace_id});
  require(std::holds_alternative<target::NamespaceView>(view) &&
              std::get<target::NamespaceView>(view).namespace_record.version.value == 2,
          "shared namespace view after members");
  // TTL expiry: one second lifetime, then a fresh read observes NotFound (lazy cleanup).
  auto ttl = app->set_storage({plugin_context(instance, "SetStorage", 10, false),
                               "ttl",
                               {std::byte{1}},
                               base_now() + 1s,
                               std::nullopt});
  require(std::holds_alternative<target::StorageEntryResult>(ttl), "TTL set");
  std::this_thread::sleep_for(std::chrono::milliseconds{1200});
  require(failed(app->get_storage({plugin_context(instance, "GetStorage", 11, false), "ttl"}),
                 target::ErrorCode::NotFound),
          "expired key is absent");
  // Authorization denial fails closed.
  authorizer.allow = false;
  require(failed(app->get_storage({plugin_context(instance, "GetStorage", 12, false), "alpha"}),
                 target::ErrorCode::PermissionDenied),
          "authorization denial");
  authorizer.allow = true;
  // Disable namespace then plugin access fails closed.
  auto disable = app->disable_shared_namespace(
      {management_context(installation, "DisableSharedNamespace", 13), namespace_id, std::nullopt});
  require(std::holds_alternative<target::SharedMembersMutation>(disable), "namespace disable");
  require(failed(app->get_storage({plugin_context(instance, "GetStorage", 14, true), "shared-key"}),
                 target::ErrorCode::Conflict),
          "disabled namespace rejects plugin access");
  std::cout << "P09 Runtime namespace/CAS/quota PostgreSQL integration passed\n";
}
} // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 5, "fixture arguments required");
    const auto port = std::stoul(argv[2]);
    require(port > 0 && port <= 65535, "invalid fixture port");
    run(static_cast<std::uint16_t>(port), std::filesystem::canonical(argv[1]),
        std::filesystem::canonical(argv[3]));
    return 0;
  } catch (const TestFailure& error) {
    std::cerr << "P09 assertion: " << error.what() << '\n';
    return 1;
  } catch (const std::exception&) {
    std::cerr << "P09 Runtime storage PostgreSQL test failed (details redacted)\n";
    return 1;
  }
}
