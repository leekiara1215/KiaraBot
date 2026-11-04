// BOOT-05 controlled-bootstrap seed: opens the initialized Runtime database and writes the business
// rows a real Runtime process uses for first issuance and deployment delivery. It deliberately
// writes NO bridge credential and NO delivery intent, so the first BindAdapter/CreateInstance call
// is the one that issues the digest/version; the fixture never fabricates secrets in SQL.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "kiarabot/core/domain/target/persistence.hpp"
#include "kiarabot/infrastructure/postgres/factories.hpp"

namespace pg = kiarabot::infrastructure::postgres;
namespace target = kiarabot::core::target;

namespace {
std::string read_secret(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string value;
  std::getline(input, value);
  if (value.size() != 64) {
    throw std::runtime_error("fixture secret unavailable");
  }
  return value;
}
} // namespace

int main(int argc, char* argv[]) {
  try {
    if (argc != 9) {
      return 2;
    }
    const std::filesystem::path root{argv[1]};
    const auto port = static_cast<std::uint16_t>(std::stoi(argv[2]));
    const target::ResourceId account{argv[3]};
    const target::InstanceId adapter_instance{argv[4]};
    const target::ResourceId second_account{argv[5]};
    const target::InstanceId second_adapter_instance{argv[6]};
    const target::InstallationId installation{argv[7]};
    const target::ResourceId plugin_type{argv[8]};
    pg::DatabaseConfig config{target::Service::Runtime,
                              "localhost",
                              port,
                              "p03_runtime",
                              "p03_runtime_runtime",
                              read_secret(root / "password-p03_runtime_runtime"),
                              (root / "server.pem").string(),
                              "p03_runtime_migration",
                              "p03_runtime_runtime"};
    auto opened = pg::open_runtime(config);
    if (const auto* failure = std::get_if<target::Failure>(&opened)) {
      std::cerr << "seed open failed code=" << static_cast<int>(failure->code) << '\n';
      return 1;
    }
    auto factory = std::move(std::get<std::unique_ptr<target::RuntimeUnitOfWorkFactory>>(opened));
    auto begun = factory->begin();
    if (std::holds_alternative<target::Failure>(begun)) {
      std::cerr << "seed begin failed\n";
      return 1;
    }
    auto work = std::move(std::get<std::unique_ptr<target::RuntimeUnitOfWork>>(begun));
    const target::BotAccount first{account,         "qq", "bootstrap-1",
                                   "Bootstrap One", {1},  target::LifecycleState::Active};
    const target::BotAccount second{second_account,  "qq", "bootstrap-2",
                                    "Bootstrap Two", {1},  target::LifecycleState::Active};
    const target::AdapterBindingRecord first_binding{
        account, adapter_instance, {1}, target::LifecycleState::Active};
    const target::AdapterBindingRecord second_binding{
        second_account, second_adapter_instance, {1}, target::LifecycleState::Active};
    const target::PluginInstallation installed{
        installation, plugin_type, {1}, target::LifecycleState::Active, {}};
    if (std::holds_alternative<target::Failure>(work->accounts().save(first, std::nullopt)) ||
        std::holds_alternative<target::Failure>(work->accounts().save(second, std::nullopt)) ||
        std::holds_alternative<target::Failure>(
            work->adapter_bindings().save(first_binding, std::nullopt)) ||
        std::holds_alternative<target::Failure>(
            work->adapter_bindings().save(second_binding, std::nullopt)) ||
        std::holds_alternative<target::Failure>(
            work->installations().save(installed, std::nullopt))) {
      std::cerr << "seed writes failed\n";
      return 1;
    }
    if (work->commit() != target::CommitResult::Committed) {
      std::cerr << "seed commit failed\n";
      return 1;
    }
    std::cout << "SEEDED\n";
    return 0;
  } catch (const std::exception&) {
    std::cerr << "seed failed\n";
    return 1;
  }
}
