// P06 bridge acceptance seed: opens the initialized Runtime database, writes the account/binding,
// external plugin instance and SHA-256 bootstrap credentials that a real Runtime process will later
// resolve through the external Bridge listener. The plaintext credentials never reach the database.
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <openssl/sha.h>

#include "kiarabot/core/domain/target/persistence.hpp"
#include "kiarabot/core/ports/target/business_ports.hpp"
#include "kiarabot/infrastructure/postgres/database.hpp"
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
target::Sha256Digest digest(std::string_view value) {
  target::Sha256Digest result;
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(),
         reinterpret_cast<unsigned char*>(result.bytes.data()));
  return result;
}
std::string hex(const target::Sha256Digest& value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (const auto byte : value.bytes) {
    text += kDigits[std::to_integer<unsigned>(byte) >> 4U];
    text += kDigits[std::to_integer<unsigned>(byte) & 0x0fU];
  }
  return text;
}
// Deterministic UUIDv4-shaped fixture identity for the scale adapter bindings.
std::string fixture_uuid(std::string_view label, int index) {
  const std::string seed = std::string{label} + "-" + std::to_string(index);
  target::Sha256Digest hashed;
  SHA256(reinterpret_cast<const unsigned char*>(seed.data()), seed.size(),
         reinterpret_cast<unsigned char*>(hashed.bytes.data()));
  auto text = hex(hashed);
  std::string result;
  result.reserve(36);
  for (int position = 0; position < 32; ++position) {
    if (position == 8 || position == 12 || position == 16 || position == 20) {
      result += '-';
    }
    if (position == 12) {
      result += '4';
      continue;
    }
    if (position == 16) {
      result += "89ab"[static_cast<unsigned char>(text[position]) % 4];
      continue;
    }
    result += text[position];
  }
  return result;
}
} // namespace

int main(int argc, char* argv[]) {
  try {
    if (argc != 11 && argc != 13) {
      return 2;
    }
    const std::filesystem::path root{argv[1]};
    const auto port = static_cast<std::uint16_t>(std::stoi(argv[2]));
    const target::InstanceId adapter_instance{argv[3]};
    const target::ResourceId account{argv[4]};
    const target::InstanceId instance{argv[5]};
    const std::string adapter_bootstrap{argv[6]};
    const std::string plugin_bootstrap{argv[7]};
    const target::InstallationId installation{argv[8]};
    const target::InstanceId shutdown_instance{argv[9]};
    const std::string shutdown_bootstrap{argv[10]};
    const int extra_adapter_count = argc == 13 ? std::stoi(argv[11]) : 0;
    const std::filesystem::path extra_mapping =
        argc == 13 ? std::filesystem::path{argv[12]} : std::filesystem::path{};
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
    if (const auto* failure = std::get_if<target::Failure>(&begun)) {
      std::cerr << "seed begin failed\n";
      return 1;
    }
    auto work = std::move(std::get<std::unique_ptr<target::RuntimeUnitOfWork>>(begun));
    auto sampled = work->current_time();
    if (std::holds_alternative<target::Failure>(sampled)) {
      std::cerr << "seed time failed\n";
      return 1;
    }
    const auto now = std::get<target::TimePoint>(sampled);
    target::BotAccount account_record{account,
                                      "qq",
                                      "10001",
                                      "Bridge Bot",
                                      target::ResourceVersion{1},
                                      target::LifecycleState::Active};
    target::AdapterBindingRecord binding{account, adapter_instance, target::ResourceVersion{1},
                                         target::LifecycleState::Active};
    target::PluginInstallation installation_record{installation,
                                                   {"99999999-9999-4999-8999-999999999999"},
                                                   target::ResourceVersion{1},
                                                   target::LifecycleState::Active,
                                                   {}};
    target::PluginInstance instance_record{instance,
                                           installation,
                                           installation_record.plugin_type,
                                           target::ResourceVersion{1},
                                           target::ResourceVersion{0},
                                           std::nullopt,
                                           target::ResourceVersion{1},
                                           target::LifecycleState::Active,
                                           std::nullopt,
                                           target::DesiredState::Running};
    target::BridgeCredentialRecord adapter_credential{"66666666-6666-4666-8666-666666666666",
                                                      adapter_instance.value,
                                                      digest(adapter_bootstrap), now, false};
    target::BridgeCredentialRecord plugin_credential{"77777777-7777-4777-8777-777777777777",
                                                     instance.value, digest(plugin_bootstrap), now,
                                                     false};
    target::PluginInstance shutdown_record{shutdown_instance,
                                           installation,
                                           installation_record.plugin_type,
                                           target::ResourceVersion{1},
                                           target::ResourceVersion{0},
                                           std::nullopt,
                                           target::ResourceVersion{1},
                                           target::LifecycleState::Active,
                                           std::nullopt,
                                           target::DesiredState::Running};
    target::BridgeCredentialRecord shutdown_credential{"88888888-8888-4888-8888-888888888888",
                                                       shutdown_instance.value,
                                                       digest(shutdown_bootstrap), now, false};
    const auto saved_account = work->accounts().save(account_record, std::nullopt);
    const auto saved_binding = work->adapter_bindings().save(binding, std::nullopt);
    const auto saved_installation = work->installations().save(installation_record, std::nullopt);
    const auto saved_instance = work->instances().save(instance_record, std::nullopt);
    const auto saved_shutdown_instance = work->instances().save(shutdown_record, std::nullopt);
    const auto saved_shutdown_credential = work->bridge_credentials().insert(shutdown_credential);
    std::vector<target::BridgeCredentialRecord> extra_credentials;
    std::vector<target::BotAccount> extra_accounts;
    std::vector<target::AdapterBindingRecord> extra_bindings;
    extra_credentials.reserve(static_cast<std::size_t>(extra_adapter_count));
    extra_accounts.reserve(static_cast<std::size_t>(extra_adapter_count));
    extra_bindings.reserve(static_cast<std::size_t>(extra_adapter_count));
    for (int index = 1; index <= extra_adapter_count; ++index) {
      const target::InstanceId scale_instance{fixture_uuid("scale-adapter-instance", index)};
      const target::ResourceId scale_account{fixture_uuid("scale-account", index)};
      const std::string bootstrap =
          hex(digest(std::string{"p12-scale-adapter-bootstrap-"} + std::to_string(index)));
      extra_accounts.push_back(
          target::BotAccount{scale_account, "qq", "scale-" + std::to_string(index),
                             "Scale Adapter " + std::to_string(index), target::ResourceVersion{1},
                             target::LifecycleState::Active});
      extra_bindings.push_back(target::AdapterBindingRecord{scale_account, scale_instance,
                                                            target::ResourceVersion{1},
                                                            target::LifecycleState::Active});
      extra_credentials.push_back(
          target::BridgeCredentialRecord{fixture_uuid("scale-adapter-credential", index),
                                         scale_instance.value, digest(bootstrap), now, false});
    }
    const auto saved_adapter_credential = work->bridge_credentials().insert(adapter_credential);
    const auto saved_plugin_credential = work->bridge_credentials().insert(plugin_credential);
    for (std::size_t index = 0; index < extra_accounts.size(); ++index) {
      const auto account_saved = work->accounts().save(extra_accounts[index], std::nullopt);
      const auto binding_saved = work->adapter_bindings().save(extra_bindings[index], std::nullopt);
      const auto credential_saved = work->bridge_credentials().insert(extra_credentials[index]);
      if (std::holds_alternative<target::Failure>(account_saved) ||
          std::holds_alternative<target::Failure>(binding_saved) ||
          std::holds_alternative<target::Failure>(credential_saved)) {
        std::cerr << "scale seed writes failed\n";
        return 1;
      }
    }
    if (std::holds_alternative<target::Failure>(saved_account) ||
        std::holds_alternative<target::Failure>(saved_binding) ||
        std::holds_alternative<target::Failure>(saved_installation) ||
        std::holds_alternative<target::Failure>(saved_instance) ||
        std::holds_alternative<target::Failure>(saved_shutdown_instance) ||
        std::holds_alternative<target::Failure>(saved_shutdown_credential) ||
        std::holds_alternative<target::Failure>(saved_adapter_credential) ||
        std::holds_alternative<target::Failure>(saved_plugin_credential)) {
      std::cerr << "seed writes failed\n";
      return 1;
    }
    if (work->commit() != target::CommitResult::Committed) {
      std::cerr << "seed commit failed\n";
      return 1;
    }
    if (extra_adapter_count > 0) {
      std::ofstream mapping{extra_mapping};
      if (!mapping) {
        std::cerr << "scale mapping unavailable\n";
        return 1;
      }
      mapping << "[";
      for (int index = 1; index <= extra_adapter_count; ++index) {
        if (index > 1) {
          mapping << ",";
        }
        const auto instance = fixture_uuid("scale-adapter-instance", index);
        const auto account = fixture_uuid("scale-account", index);
        const auto bootstrap =
            hex(digest(std::string{"p12-scale-adapter-bootstrap-"} + std::to_string(index)));
        mapping << "{\"account\":\"" << account << "\",\"adapter_instance\":\"" << instance
                << "\",\"bootstrap\":\"" << bootstrap << "\"}";
      }
      mapping << "]\n";
    }
    std::cout << "SEEDED\n";
    return 0;
  } catch (const std::exception&) {
    std::cerr << "seed failed\n";
    return 1;
  }
}
