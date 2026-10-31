#pragma once

#include <mutex>

#include <nlohmann/json.hpp>

#include "kiarabot/core/application/control_application.hpp"
#include "kiarabot/core/runtime/message_projection.hpp"
#include "kiarabot/core/runtime/runtime_log.hpp"

namespace kiarabot::core {
struct ControlUser {
  std::string id;
  std::string username;
  std::string password_hash;
  std::string role;
  // Permissions granted outside the frozen role table. v1 grants only
  // message.read here; no default role holds it, so the messages stream stays
  // opt-in per user instead of a wildcard or admin privilege.
  std::vector<std::string> permissions;
};
struct ControlOptions {
  std::vector<ControlUser> users;
  // Canonical public manifests of installed plugins, supplied by the composition root.
  std::vector<ControlJson> plugins;
  std::vector<std::string> accounts;
  std::string adapter_id;
  std::string model_name;
  std::function<bool()> adapter_connected;
  std::function<std::vector<RuntimeLog>()> logs;
  // Bounded metadata-only projection of accepted messages; empty disables the
  // messages channel projection even for explicitly granted users.
  std::function<std::vector<MessageMetadata>()> messages;
  std::string encryption_key; // 32 bytes, hex encoded; never returned or logged.
  std::size_t audit_limit{100000};
  std::shared_ptr<PluginConfigDelivery> config_delivery;
};
// Thread safe. Store outlives this service; all effects/receipts/audit share one transaction.
// Reserved namespaces begin with '$', which is forbidden in plugin identities.
class ControlService final : public IControlApplication {
public:
  ControlService(RuntimeStore& store, ControlOptions options, RuntimeClock clock = {});
  PortResult<ControlReply> invoke(const ControlRequest& request) override;
  PortStatus readiness() override;
  // Current authenticated projection, also used by event streaming. No secret state escapes.
  PortResult<ControlJson> stream_snapshot(const ControlMetadata& metadata) override;

private:
  PortResult<ControlReply> invoke_locked(const ControlRequest& request);
  std::vector<ControlJson> resources(ControlResource resource, const RuntimeSnapshot& snapshot,
                                     const ControlJson& state, const ControlUser& user) const;
  RuntimeStore& store_;
  ControlOptions options_;
  RuntimeClock clock_;
  std::mutex mutex_;
  struct PageSnapshot {
    std::string binding;
    WallTime expires;
    std::vector<ControlJson> items;
    std::size_t offset;
  };
  std::unordered_map<std::string, PageSnapshot> pages_;
  std::unordered_map<std::string, std::pair<WallTime, unsigned>> login_attempts_;
};
ControlJson control_json(const google::protobuf::Message& message);
google::protobuf::Struct control_struct(const ControlJson& value);
std::string control_timestamp(WallTime value);
std::vector<std::string> control_permissions(std::string_view role);
// Effective permissions: the frozen role table plus explicitly granted permissions.
std::vector<std::string> control_permissions(std::string_view role,
                                             const std::vector<std::string>& grants);
// Permissions a private control configuration may grant explicitly. v1 has only
// message.read: everything else stays fixed by the role table.
const std::vector<std::string>& control_grantable_permissions();
bool control_user_allows(const ControlUser& user, std::string_view permission);
// Cryptographic operations use libsodium Argon2id, keyed BLAKE2b and secretbox.
std::string control_password_hash(std::string_view password);
bool control_password_verify(std::string_view hash, std::string_view password);
std::string control_secret();
std::string control_digest(std::string_view value, std::string_view key);
std::string control_encrypt(std::string_view value, std::string_view key);
std::string control_decrypt(std::string_view value, std::string_view key);
bool control_plugin_enabled(const RuntimeSnapshot& snapshot, const std::string& plugin_id);
} // namespace kiarabot::core
