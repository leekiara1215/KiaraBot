#include <array>
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
#include <variant>
#include <vector>

#include <sys/wait.h>

extern char** environ;

#include <pqxx/pqxx>

#include "kiarabot/core/application/target/runtime_host_application.hpp"
#include "kiarabot/infrastructure/postgres/factories.hpp"

namespace pg = kiarabot::infrastructure::postgres;
namespace target = kiarabot::core::target;
using std::chrono_literals::operator""s;

namespace {
class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
void require(bool condition, const char* scenario) {
  if (!condition)
    throw TestFailure(scenario);
}
std::string quote(std::string_view value) {
  std::string output = "'";
  for (const char character : value) {
    if (character == '\'' || character == '\\')
      output += '\\';
    output += character;
  }
  return output + '\'';
}
template <typename Value> Value take(target::Outcome<Value> outcome, const char* scenario) {
  if (const auto* failure = std::get_if<target::Failure>(&outcome)) {
    throw TestFailure(std::string{scenario} + " failed with error code " +
                      std::to_string(static_cast<int>(failure->code)));
  }
  return std::get<Value>(std::move(outcome));
}
template <typename Value>
void expect_failure(const target::Outcome<Value>& outcome, target::ErrorCode code,
                    const char* scenario) {
  const auto* failure = std::get_if<target::Failure>(&outcome);
  require(failure != nullptr && failure->code == code, scenario);
}
void expect_failure(const target::Status& status, target::ErrorCode code, const char* scenario) {
  const auto* failure = std::get_if<target::Failure>(&status);
  require(failure != nullptr && failure->code == code, scenario);
}
int initialize_process(const std::filesystem::path& executable, const pg::DatabaseConfig& config,
                       const std::filesystem::path& password_file) {
  const std::array<std::string, 10> arguments{
      executable.string(),   "--initialize-empty",        "runtime",
      config.host,           std::to_string(config.port), config.database,
      config.migration_role, config.runtime_role,         config.root_certificate,
      password_file.string()};
  std::array<char*, 11> argv{};
  for (std::size_t index = 0; index < arguments.size(); ++index)
    argv[index] = const_cast<char*>(arguments[index].c_str());
  pid_t process = 0;
  require(::posix_spawn(&process, executable.c_str(), nullptr, nullptr, argv.data(), environ) == 0,
          "initializer process spawn failed");
  int status = 0;
  require(::waitpid(process, &status, 0) == process && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "runtime database initialization failed");
  return WEXITSTATUS(status);
}
class FakeClock final : public target::HostClockPort {
public:
  target::TimePoint current{std::chrono::seconds{1000}};
  target::Outcome<target::TimePoint> now() override {
    return current;
  }
  target::Outcome<std::string> runtime_epoch() override {
    return std::string{"epoch-1"};
  }
  target::Outcome<std::uint64_t> next_connection_generation() override {
    return std::uint64_t{1};
  }
};
target::HostRegistrationCommand command(std::string epoch = "epoch-1") {
  target::HostRegistrationCommand value;
  value.certificate_sha256 = "certificate-1";
  value.host_id = "host-1";
  value.runtime_epoch = std::move(epoch);
  value.capabilities = {"host.execute"};
  return value;
}
std::unique_ptr<target::RuntimeUnitOfWork> begin(target::RuntimeUnitOfWorkFactory& factory,
                                                 bool* failed = nullptr) {
  auto begun = factory.begin();
  if (const auto* failure = std::get_if<target::Failure>(&begun)) {
    if (failed != nullptr)
      *failed = true;
    throw TestFailure("runtime UoW begin failed with error code " +
                      std::to_string(static_cast<int>(failure->code)));
  }
  return std::get<std::unique_ptr<target::RuntimeUnitOfWork>>(std::move(begun));
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
  FakeClock clock;
  {
    auto opened = pg::open_runtime_with_host_allowlist(
        config, std::vector<target::HostCertificateBinding>{{"host-1", "certificate-1"}});
    if (const auto* failure = std::get_if<target::Failure>(&opened)) {
      throw TestFailure("runtime factory open failed with error code " +
                        std::to_string(static_cast<int>(failure->code)));
    }
    auto factory = std::get<std::unique_ptr<target::RuntimeUnitOfWorkFactory>>(std::move(opened));
    {
      auto work = begin(*factory);
      target::RuntimeHostApplication app(work->host_registration(), clock);
      const auto registered = take(app.register_host(command()), "register host");
      require(registered.registration.version.value == 1, "create host record version 1");
      require(registered.connection.online, "new connection online");
      require(registered.connection.fence.connection_generation == 1, "first generation");
      const auto found = take(work->host_registration().find_host({"host-1"}), "find host");
      require(found && found->state == target::LifecycleState::Active, "host record active");
      const auto connection =
          take(work->host_registration().connection({"host-1"}), "read connection");
      require(connection && connection->online, "connection persisted online");
      require(work->commit() == target::CommitResult::Committed, "registration commit");
    }
    {
      auto work = begin(*factory);
      target::RuntimeHostApplication app(work->host_registration(), clock);
      const auto registered = take(app.register_host(command("epoch-2")), "replace connection");
      require(registered.connection.fence.connection_generation == 2, "generation increments");
      require(registered.registration.version.value == 2, "host record version bumps");
      require(work->commit() == target::CommitResult::Committed, "replacement commit");
    }
    {
      // HOST-03 Runtime-owned admission gate: durable CAS/version rows on real PG. A replay of
      // the exact intent returns the same gate; changed intent needs the current version.
      class GateAuthorization final : public target::HostGateAuthorizationPort {
      public:
        bool allow = true;
        int calls = 0;
        target::Outcome<target::ResourceVersion> check(const target::ResourceRef& host,
                                                       const target::Sha256Digest& digest,
                                                       target::TimePoint) override {
          ++calls;
          require(host.kind == target::ResourceKind::Host && host.id.value == "host-1",
                  "gate exact host scope");
          require(digest.bytes[0] == std::byte{0xaa} && digest.bytes[31] == std::byte{0xaa},
                  "gate current Host leaf digest");
          if (!allow) {
            return target::Failure{target::ErrorCode::PermissionDenied,
                                   target::Acceptance::NotAccepted, target::Recovery::ReadCurrent,
                                   std::nullopt, std::nullopt};
          }
          return target::ResourceVersion{7};
        }
      };
      GateAuthorization authorization;
      target::HostFenceRegistry fences;
      const target::HostFence fence{target::ResourceId{"host-1"}, 2, "epoch-2"};
      fences.record(fence);
      const std::string certificate(64, 'a');
      auto app = target::RuntimeHostGateApplication(
          [&factory]() -> target::Outcome<std::unique_ptr<target::RuntimeUnitOfWork>> {
            return factory->begin();
          },
          fences, authorization, clock,
          std::vector<target::HostCertificateBinding>{{"host-1", certificate}});
      target::HostAdmissionGateCommand command_value;
      command_value.certificate_sha256 = certificate;
      command_value.host = target::ResourceId{"host-1"};
      command_value.fence = fence;
      command_value.operation = target::OperationRef{
          target::Service::Host, target::OperationId{std::string(36, 'a')},
          target::ResourceRef{target::ResourceKind::Host, target::ResourceId{"host-1"}}};
      command_value.maintenance_version = 1;
      command_value.admission_open = false;
      command_value.deadline = clock.current + 10s;
      const auto first = take(app.set_admission_gate(command_value), "first gate");
      require(first.gate.gate_version == 1 && !first.gate.admission_open &&
                  first.gate.maintenance_version == 1,
              "first gate version 1 durable rows");
      const auto replay = take(app.set_admission_gate(command_value), "gate replay");
      require(replay.gate.gate_version == 1, "same intent replay returns same gate");
      auto changed = command_value;
      changed.expected_gate_version = 1;
      changed.maintenance_version = 2;
      changed.admission_open = true;
      const auto second = take(app.set_admission_gate(changed), "gate advance");
      require(second.gate.gate_version == 2 && second.gate.admission_open,
              "expected version advances gate monotonically");
      auto stale = changed;
      stale.maintenance_version = 3;
      stale.admission_open = false;
      stale.expected_gate_version = 1;
      expect_failure(app.set_admission_gate(stale), target::ErrorCode::Conflict,
                     "stale expected gate version conflict");
      authorization.allow = false;
      auto next = command_value;
      next.maintenance_version = 4;
      next.expected_gate_version = 2;
      expect_failure(app.set_admission_gate(next), target::ErrorCode::PermissionDenied,
                     "current Identity denial blocks gate persistence");
      authorization.allow = true;
      fences.erase(fence);
      expect_failure(app.set_admission_gate(next), target::ErrorCode::Unauthenticated,
                     "missing live fence blocks gate persistence");
      fences.record(fence);
      auto verify_work = begin(*factory);
      const auto stored =
          take(verify_work->host_admission_gates().current({"host-1"}), "read stored gate");
      require(stored && stored->gate_version == 2 && stored->admission_open &&
                  stored->maintenance_version == 2 &&
                  stored->operation.id.value == std::string(36, 'a'),
              "gate rows persist without a fabricated admission/process success");
      verify_work->rollback();
    }
    {
      auto work = begin(*factory);
      auto current = take(work->host_registration().connection({"host-1"}), "read replacement");
      require(current.has_value(), "replacement connection present");
      auto stale = *current;
      stale.fence.connection_generation = 1;
      expect_failure(work->host_registration().save_connection(stale, 1),
                     target::ErrorCode::Conflict, "stale generation save conflict");
      // A conflict can poison the conditional-write transaction; complete the projection in a
      // fresh UoW rather than reusing it.
      work->rollback();
    }
    {
      auto work = begin(*factory);
      target::RuntimeHostApplication app(work->host_registration(), clock);
      auto current = take(work->host_registration().connection({"host-1"}), "read current");
      require(current.has_value(), "current connection present");
      current->last_heartbeat_at = clock.current;
      current->online = true;
      current->desired_config_version.value = 42;
      const auto saved = work->host_registration().save_connection(*current, 2);
      if (const auto* failure = std::get_if<target::Failure>(&saved)) {
        throw TestFailure("heartbeat save failed with error code " +
                          std::to_string(static_cast<int>(failure->code)));
      }
      clock.current += 20s;
      require(take(app.project_offline({{"host-1"}}), "offline projection") == 1,
              "offline projection applied");
      const auto projected =
          take(work->host_registration().connection({"host-1"}), "read projection");
      require(projected && !projected->online, "offline flips online only");
      require(projected->desired_config_version.value == 42, "desired config preserved");
      require(work->commit() == target::CommitResult::Committed, "projection commit");
    }
    {
      auto work = begin(*factory);
      auto found = take(work->host_registration().find_host({"host-1"}), "find for delete");
      require(found.has_value(), "host record for delete");
      auto record = *found;
      const auto expected = record.version;
      record.state = target::LifecycleState::Deleting;
      record.version.value = expected.value + 1;
      require(work->host_registration().save_host(record, expected).index() == 0,
              "deleting state save");
      target::RuntimeHostApplication app(work->host_registration(), clock);
      expect_failure(app.register_host(command()), target::ErrorCode::PermissionDenied,
                     "deleting host denied");
      work->rollback();
    }
    factory.reset();
  }
  for (const auto& allowlist :
       {std::vector<target::HostCertificateBinding>{},
        std::vector<target::HostCertificateBinding>{{"host-1", "other-certificate"}}}) {
    auto opened = pg::open_runtime_with_host_allowlist(config, allowlist);
    auto factory = std::get<std::unique_ptr<target::RuntimeUnitOfWorkFactory>>(std::move(opened));
    auto work = begin(*factory);
    target::RuntimeHostApplication app(work->host_registration(), clock);
    expect_failure(app.register_host(command()), target::ErrorCode::PermissionDenied,
                   "allowlist mismatch/empty denies");
    work->rollback();
  }
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
    std::cerr << "P08 assertion: " << error.what() << '\n';
    return 1;
  } catch (const std::exception&) {
    std::cerr << "P08 host registration storage test failed (details redacted)\n";
    return 1;
  }
}
