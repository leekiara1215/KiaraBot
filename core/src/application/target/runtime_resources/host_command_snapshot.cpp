#include "common.hpp"
#include "kiarabot/core/domain/target/host_command_records.hpp"

namespace kiarabot::core::target::runtime_resources_detail {
std::string hex_digest(const Sha256Digest& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(digest.bytes.size() * 2);
  for (const std::byte byte : digest.bytes) {
    const auto value = static_cast<unsigned char>(byte);
    text.push_back(kHex[value >> 4]);
    text.push_back(kHex[value & 0x0f]);
  }
  return text;
}
// Enqueues the exact Host command snapshot in the caller's UoW. A Hostless (externally managed)
// instance has no outbox command. Package/profile values are deployment facts; until the P08
// deployment mapping lands the installation manifest digest is the only Core-owned package hash,
// so launch_profile is left empty rather than invented.
Status enqueue_host_command(RuntimeUnitOfWork& work, RuntimeSecretPort& secrets,
                            const PluginInstance& instance, TimePoint now) {
  if (!instance.host) {
    return std::monostate{};
  }
  auto found = work.installations().find(instance.installation);
  if (const auto* failure = std::get_if<Failure>(&found)) {
    return *failure;
  }
  const auto installation = std::get<std::optional<PluginInstallation>>(found);
  if (!installation) {
    return Failure{ErrorCode::NotFound, Acceptance::NotAccepted, Recovery::ReadCurrent, {}, {}};
  }
  const auto generation = instance.current ? instance.current->execution_generation.value : 1;
  const std::string canonical = instance.id.value + "|" + instance.host->value + "|" +
                                std::to_string(generation) + "|" +
                                std::to_string(instance.desired_version.value) + "|" +
                                std::to_string(instance.version.value) + "|" +
                                std::to_string(static_cast<int>(instance.desired));
  auto payload = secrets.sha256(canonical);
  if (const auto* failure = std::get_if<Failure>(&payload)) {
    return *failure;
  }
  auto manifest = secrets.sha256(std::string_view(
      reinterpret_cast<const char*>(installation->manifest.data()), installation->manifest.size()));
  if (const auto* failure = std::get_if<Failure>(&manifest)) {
    return *failure;
  }
  // Deterministic command identity makes a retried mutation idempotent without reusing a random
  // id; a different snapshot hashes differently and is a distinct command.
  HostCommandOutboxRecord record;
  record.host_id = instance.host->value;
  record.instance_id = instance.id.value;
  record.installation_id = instance.installation.value;
  record.kind = instance.desired == DesiredState::Running ? HostCommandKind::ApplyDesiredProcess
                                                          : HostCommandKind::StopExecution;
  record.execution_generation = generation == 0 ? 1 : generation;
  record.desired_version = instance.desired_version.value;
  record.desired_state = instance.desired;
  record.package_sha256 = hex_digest(std::get<Sha256Digest>(manifest));
  record.payload_sha256 = hex_digest(std::get<Sha256Digest>(payload));
  record.command_id = "hc-" + record.payload_sha256;
  record.state = HostCommandDeliveryState::Pending;
  record.created_at = now;
  record.updated_at = now;
  auto inserted = work.host_commands().insert(record);
  if (const auto* failure = std::get_if<Failure>(&inserted)) {
    return *failure;
  }
  return std::monostate{};
}

} // namespace kiarabot::core::target::runtime_resources_detail
