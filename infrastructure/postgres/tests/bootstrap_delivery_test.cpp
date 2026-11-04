#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <pqxx/pqxx>
#include <sys/wait.h>

#include "kiarabot/core/application/target/runtime_resources/bootstrap.hpp"
#include "kiarabot/infrastructure/postgres/factories.hpp"

extern char** environ;

namespace pg = kiarabot::infrastructure::postgres;
namespace t = kiarabot::core::target;

namespace {
class TestFailure final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void require(bool condition, const char* reason) {
  if (!condition) {
    throw TestFailure(reason);
  }
}
template <typename Value> Value take(t::Outcome<Value> result, const char* reason) {
  if (const auto* failure = std::get_if<t::Failure>(&result)) {
    throw TestFailure(std::string{reason} + " (code " +
                      std::to_string(static_cast<int>(failure->code)) + ")");
  }
  return std::move(std::get<Value>(result));
}
template <typename Value> bool rejects(const t::Outcome<Value>& result, t::ErrorCode code) {
  const auto* failure = std::get_if<t::Failure>(&result);
  return failure != nullptr && failure->code == code;
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
std::string secret(const std::filesystem::path& file) {
  std::ifstream input(file);
  std::string value;
  std::getline(input, value);
  require(value.size() == 64, "fixture secret unavailable");
  return value;
}
std::string uuid(std::uint64_t value) {
  std::array<char, 40> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "00000000-0000-4000-8000-%012llu",
                static_cast<unsigned long long>(value));
  return std::string{buffer.data()};
}
t::TimePoint wall_clock() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
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

// Only the trusted boundary dependencies are injected. Delivery intents, credential versions,
// receipts and operations stay in the production PostgreSQL UoW.
class DeterministicIds final : public t::RuntimeIdPort {
public:
  t::Outcome<t::ResourceId> account_id() override {
    return t::ResourceId{uuid(counter_++)};
  }
  t::Outcome<t::InstallationId> installation_id() override {
    return t::InstallationId{uuid(counter_++)};
  }
  t::Outcome<t::InstanceId> instance_id() override {
    return t::InstanceId{uuid(counter_++)};
  }
  t::Outcome<t::InstanceId> adapter_instance_id() override {
    return t::InstanceId{uuid(counter_++)};
  }
  t::Outcome<t::ConnectionId> connection_id() override {
    return t::ConnectionId{uuid(counter_++)};
  }
  t::Outcome<t::OperationId> operation_id() override {
    return t::OperationId{uuid(counter_++)};
  }
  t::Outcome<t::RuntimeEpoch> runtime_epoch() override {
    return t::RuntimeEpoch{uuid(counter_++)};
  }
  t::Outcome<std::string> snapshot_id() override {
    return uuid(counter_++);
  }

private:
  std::atomic<std::uint64_t> counter_{1000};
};

class RealDigestSecrets final : public t::RuntimeSecretPort {
public:
  t::Outcome<std::string> bootstrap_credential() override {
    const auto value = counter_++;
    std::string credential(64, 'a');
    const auto text = std::to_string(value);
    credential.replace(64 - text.size(), text.size(), text);
    return credential;
  }
  t::Outcome<std::array<std::byte, 32>> opaque_secret() override {
    return t::rejected(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<t::Sha256Digest> sha256(std::string_view material) override {
    t::Sha256Digest digest;
    unsigned int size = 0;
    if (EVP_Digest(material.data(), material.size(),
                   reinterpret_cast<unsigned char*>(digest.bytes.data()), &size, EVP_sha256(),
                   nullptr) != 1 ||
        size != 32) {
      return t::rejected(t::ErrorCode::Internal);
    }
    return digest;
  }

private:
  std::atomic<std::uint64_t> counter_{1};
};

class OnlineAllow final : public t::RuntimeAuthorizationPort {
public:
  bool available() const noexcept override {
    return true;
  }
  t::Outcome<t::RuntimeAuthorizationDecision>
  check(const t::RuntimeAuthorizationRequest& request) override {
    return t::RuntimeAuthorizationDecision{true,
                                           request.authentication.subject,
                                           request.permission,
                                           request.scope,
                                           {1},
                                           std::chrono::system_clock::now(),
                                           {}};
  }
};

t::CommandContext context(const std::string& method, std::string_view key,
                          const std::string& request_id, t::ResourceRef resource) {
  const auto now = wall_clock();
  t::CommandContext result;
  result.authentication.subject = t::SubjectId{uuid(1)};
  result.authentication.audience = t::Service::Runtime;
  result.authentication.authorization_version = t::ResourceVersion{1};
  result.authentication.verified_at = now;
  result.authentication.expires_at = now + std::chrono::seconds{300};
  result.request_id = request_id;
  result.trace_id = uuid(2);
  result.method = method;
  result.resource = resource;
  result.idempotency_key = std::string{key};
  result.fingerprint = t::Sha256Digest{};
  result.deadline = now + std::chrono::seconds{120};
  return result;
}

void seed_account_and_binding(t::RuntimeUnitOfWorkFactory& factory, const std::string& account,
                              const std::string& adapter_instance) {
  auto work = take(factory.begin(), "seed begin");
  t::BotAccount record{t::ResourceId{account},   "qq", account, "fixture", {1},
                       t::LifecycleState::Active};
  take(work->accounts().save(record, std::nullopt), "seed account");
  t::AdapterBindingRecord binding{
      t::ResourceId{account}, t::InstanceId{adapter_instance}, {1}, t::LifecycleState::Active};
  take(work->adapter_bindings().save(binding, std::nullopt), "seed binding");
  require(work->commit() == t::CommitResult::Committed, "seed commit");
}

void disable_binding(t::RuntimeUnitOfWorkFactory& factory, const std::string& account) {
  auto work = take(factory.begin(), "disable begin");
  auto found = take(work->adapter_bindings().find(t::ResourceId{account}), "disable find");
  require(found.has_value(), "binding exists");
  auto binding = *found;
  binding.state = t::LifecycleState::Disabled;
  binding.version.value += 1;
  take(work->adapter_bindings().save(binding, t::ResourceVersion{1}), "disable save");
  require(work->commit() == t::CommitResult::Committed, "disable commit");
}

std::optional<t::BridgeCredentialRecord> latest_credential(t::RuntimeUnitOfWorkFactory& factory,
                                                           const std::string& resource_id) {
  auto work = take(factory.begin(), "credential read begin");
  auto latest = take(work->bridge_credentials().latest(resource_id), "credential latest");
  work->rollback();
  return latest;
}

std::optional<t::BootstrapDeliveryRecord> current_delivery(t::RuntimeUnitOfWorkFactory& factory,
                                                           t::ResourceRef resource) {
  auto work = take(factory.begin(), "delivery read begin");
  auto found = take(work->bootstrap_deliveries().current(resource), "delivery current");
  work->rollback();
  return found;
}

void run(std::uint16_t port, const std::filesystem::path& root,
         const std::filesystem::path& initializer) {
  std::ifstream admin_password_file(root / "password");
  std::string admin_password;
  std::getline(admin_password_file, admin_password);
  require(admin_password.size() == 64, "fixture administrator secret unavailable");
  const auto role_password = [&](const std::string& role) {
    return secret(root / ("password-" + role));
  };
  pqxx::connection administrator(
      "host=localhost port=" + std::to_string(port) + " dbname=postgres user=p03_admin password=" +
      quote(admin_password) + " sslmode=verify-full channel_binding=require sslrootcert=" +
      quote((root / "server.pem").string()));
  {
    // The compiled driver must prove it owns the actual backend directory before any DDL; the
    // disposable fixture's negative control passes a mismatched directory and requires this exact
    // rejection text and exit code.
    pqxx::read_transaction verify(administrator);
    const auto actual = std::filesystem::canonical(
        verify.exec("SHOW data_directory").one_row()[0].as<std::string>());
    require(!std::filesystem::is_symlink(root / "data") &&
                actual == std::filesystem::canonical(root / "data") && actual.parent_path() == root,
            "fixture backend directory mismatch");
  }
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
  const pg::DatabaseConfig config{t::Service::Runtime,
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
  auto factory = take(std::move(opened), "factory open");
  DeterministicIds ids;
  RealDigestSecrets secrets;
  OnlineAllow authorization;
  auto application = take(t::runtime_resources_detail::make_runtime_bootstrap_delivery_application(
                              {*factory, ids, secrets, authorization}),
                          "delivery application");

  const auto account = uuid(5000);
  const auto adapter_instance = uuid(5001);
  const auto account_resource = t::ResourceRef{t::ResourceKind::Account, {account}};
  seed_account_and_binding(*factory, account, adapter_instance);

  // First issue: pending intent, real version 1, plaintext only in the first response.
  t::ReissueBootstrapCommand first;
  first.context = context("kiarabot.v1.RuntimeService/ReissueAdapterBootstrap",
                          "bootstrap-key-0001", uuid(6001), account_resource);
  first.resource = account_resource;
  first.recipient_id = "recipient-one";
  auto issued = take(application->reissue(first, t::BootstrapPeerFamily::Adapter), "first issue");
  require(issued.plaintext.size() == 64, "first issue returns the one-time plaintext");
  require(issued.delivery.state == t::BootstrapDeliveryState::Pending, "first issue is pending");
  require(issued.delivery.credential_version == 1, "first issue starts at version one");
  require(issued.delivery.recipient_id == "recipient-one", "first issue binds the recipient");
  const auto persisted = latest_credential(*factory, adapter_instance);
  require(persisted.has_value() && persisted->version == 1 && !persisted->revoked,
          "first issue persists the versioned digest");
  const auto digest = take(secrets.sha256(issued.plaintext), "digest check");
  require(persisted->digest == digest, "persisted digest matches the one-time plaintext");
  {
    auto work = take(factory->begin(), "intent read begin");
    const auto intent =
        take(work->bootstrap_deliveries().find(account_resource, issued.delivery.delivery_id),
             "intent read");
    require(intent.has_value() && intent->credential_version == 1, "delivery intent persisted");
    work->rollback();
  }

  // Idempotent replay: same identity returns the same metadata and never the plaintext again.
  auto replay = take(application->reissue(first, t::BootstrapPeerFamily::Adapter), "first replay");
  require(replay.plaintext.empty(), "replay never reconstructs the plaintext");
  require(replay.delivery.delivery_id == issued.delivery.delivery_id,
          "replay returns the original delivery identity");

  // Explicit reissue advances the real version and revokes the previous digest/intent.
  t::ReissueBootstrapCommand second = first;
  second.context = context("kiarabot.v1.RuntimeService/ReissueAdapterBootstrap",
                           "bootstrap-key-0002", uuid(6002), account_resource);
  second.expected_credential_version = 1;
  auto reissued =
      take(application->reissue(second, t::BootstrapPeerFamily::Adapter), "explicit reissue");
  require(!reissued.plaintext.empty(), "reissue returns one new plaintext");
  require(reissued.delivery.credential_version == 2, "reissue advances the version");
  require(reissued.delivery.delivery_id != issued.delivery.delivery_id,
          "reissue allocates a new delivery identity");
  const auto advanced = latest_credential(*factory, adapter_instance);
  require(advanced.has_value() && advanced->version == 2 && !advanced->revoked,
          "reissue persists version two as the active credential");
  {
    auto work = take(factory->begin(), "history read begin");
    const auto previous =
        take(work->bootstrap_deliveries().find(account_resource, issued.delivery.delivery_id),
             "history read");
    require(previous.has_value() && previous->state == t::BootstrapDeliveryState::Revoked,
            "previous delivery intent is revoked");
    work->rollback();
  }

  // A report from the registered Control peer marks only the current version DELIVERED.
  auto report = t::ReportBootstrapDeliveryCommand{};
  report.context = context("kiarabot.v1.RuntimeService/ReportAdapterBootstrapDelivery",
                           "report-key-00001", uuid(6010), account_resource);
  report.resource = account_resource;
  report.delivery_id = reissued.delivery.delivery_id;
  report.recipient_id = "recipient-one";
  report.credential_version = 2;
  report.state = t::BootstrapDeliveryState::Delivered;
  report.receipt_id = "receipt-0001";
  report.observed_at = wall_clock();
  report.control_peer_authorized = true;
  auto reported =
      take(application->report(report, t::BootstrapPeerFamily::Adapter), "first report");
  require(reported.state == t::BootstrapDeliveryState::Delivered,
          "report marks the delivery delivered");
  auto report_replay =
      take(application->report(report, t::BootstrapPeerFamily::Adapter), "report replay");
  require(report_replay.state == t::BootstrapDeliveryState::Delivered,
          "report replay restores the durable state");
  auto conflicting_report = report;
  conflicting_report.receipt_id = "receipt-0002";
  conflicting_report.context = context("kiarabot.v1.RuntimeService/ReportAdapterBootstrapDelivery",
                                       "report-key-00002", uuid(6011), account_resource);
  require(rejects(application->report(conflicting_report, t::BootstrapPeerFamily::Adapter),
                  t::ErrorCode::Conflict),
          "a different receipt never rewrites a delivered version");

  // Late reports for a superseded version, and reports without the Control peer, fail closed.
  auto late = report;
  late.context = context("kiarabot.v1.RuntimeService/ReportAdapterBootstrapDelivery",
                         "report-key-late-0001", uuid(6012), account_resource);
  late.delivery_id = issued.delivery.delivery_id;
  late.credential_version = 1;
  require(
      rejects(application->report(late, t::BootstrapPeerFamily::Adapter), t::ErrorCode::Conflict),
      "a late report for a superseded version is rejected");
  auto unauthorized = report;
  unauthorized.context = context("kiarabot.v1.RuntimeService/ReportAdapterBootstrapDelivery",
                                 "report-key-untrusted-0001", uuid(6013), account_resource);
  unauthorized.control_peer_authorized = false;
  require(rejects(application->report(unauthorized, t::BootstrapPeerFamily::Adapter),
                  t::ErrorCode::PermissionDenied),
          "reports require the registered Control platform peer");
  auto stale_expected = second;
  stale_expected.context = context("kiarabot.v1.RuntimeService/ReissueAdapterBootstrap",
                                   "bootstrap-key-stale-0001", uuid(6014), account_resource);
  require(rejects(application->reissue(stale_expected, t::BootstrapPeerFamily::Adapter),
                  t::ErrorCode::Conflict),
          "reissue with an old expected version is rejected");

  // Concurrent reissue: exactly one of two racers advances the version and the other conflicts.
  const auto before = latest_credential(*factory, adapter_instance);
  require(before.has_value(), "race baseline exists");
  std::atomic<int> ready{0};
  std::atomic<int> winners{0};
  std::atomic<int> conflicts{0};
  const auto race = [&](std::uint64_t request_number, std::string_view key) {
    auto command = second;
    command.context = context("kiarabot.v1.RuntimeService/ReissueAdapterBootstrap", key,
                              uuid(request_number), account_resource);
    command.expected_credential_version = before->version;
    ++ready;
    while (ready.load() != 2) {
      std::this_thread::yield();
    }
    auto outcome = application->reissue(command, t::BootstrapPeerFamily::Adapter);
    if (std::holds_alternative<t::Failure>(outcome)) {
      if (std::get<t::Failure>(outcome).code == t::ErrorCode::Conflict) {
        ++conflicts;
      }
      return;
    }
    ++winners;
  };
  std::thread first_racer(race, std::uint64_t{7001}, std::string_view{"race-key-00000001"});
  std::thread second_racer(race, std::uint64_t{7002}, std::string_view{"race-key-00000002"});
  first_racer.join();
  second_racer.join();
  require(winners.load() == 1 && conflicts.load() == 1,
          "exactly one concurrent reissue wins and the other conflicts");
  const auto after = latest_credential(*factory, adapter_instance);
  require(after.has_value() && after->version == before->version + 1,
          "concurrent reissue advances the version exactly once");

  // A disabled binding invalidates pending delivery: late report/reissue fail closed.
  disable_binding(*factory, account);
  auto disabled_report = report;
  disabled_report.context = context("kiarabot.v1.RuntimeService/ReportAdapterBootstrapDelivery",
                                    "report-key-disabled-0001", uuid(7010), account_resource);
  disabled_report.delivery_id = reissued.delivery.delivery_id;
  require(rejects(application->report(disabled_report, t::BootstrapPeerFamily::Adapter),
                  t::ErrorCode::NotFound),
          "a disabled binding rejects late delivery evidence");
  auto disabled_reissue = second;
  disabled_reissue.context = context("kiarabot.v1.RuntimeService/ReissueAdapterBootstrap",
                                     "reissue-key-disabled-01", uuid(7011), account_resource);
  require(rejects(application->reissue(disabled_reissue, t::BootstrapPeerFamily::Adapter),
                  t::ErrorCode::NotFound),
          "a disabled binding rejects reissue");

  const auto delivery = current_delivery(*factory, account_resource);
  require(delivery.has_value() && delivery->credential_version > 0,
          "the durable delivery history remains queryable without secrets");
}
} // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 5, "fixture arguments required");
    const auto port = std::stoul(argv[2]);
    require(port > 0 && port <= 65535, "invalid fixture port");
    run(static_cast<std::uint16_t>(port), std::filesystem::canonical(argv[1]),
        std::filesystem::canonical(argv[3]));
    std::cout << "BOOT-02 Runtime bootstrap delivery: real PostgreSQL evidence passed\n";
    return 0;
  } catch (const TestFailure& error) {
    std::cerr << "BOOT-02 assertion: " << error.what() << '\n';
    return 1;
  } catch (const std::exception&) {
    std::cerr << "BOOT-02 Runtime bootstrap delivery PostgreSQL test failed (details redacted)\n";
    return 1;
  }
}
