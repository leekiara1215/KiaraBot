#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kiarabot/control/application/target/secret_delivery.hpp"

namespace kiarabot::control::target {

// Trusted composition input, never parsed from a delivery/HTTP request. One fixed peer per
// recipient; Account bindings name the assigned adapter peer, Instance bindings name the plugin.
// `resource.id` may be empty for a KIND-LEVEL recipient: it then accepts every resource of
// `resource.kind` but keeps this fixed directory/owner and derives the installed file name from the
// exact delivery resource id (`<resource_id>.bootstrap`). A non-empty id is exact and must match.
struct SecretFileRecipient {
  std::string recipient_id;
  core::target::ResourceRef resource;
  std::string peer_id;
  std::string directory;
  std::uint32_t owner_uid = 0;
  std::uint32_t owner_gid = 0;
};

enum class SecretFilePhase { IntentDurable, TargetTemporaryDurable, TargetDurable, ReceiptDurable };
// Deterministic process-crash harness only. A production composition leaves this null. The
// harness observes durability with the peer file lock held; it must not call a delivery API.
using SecretFileCheckpoint = void (*)(SecretFilePhase);

// POSIX fixed-directory implementation; SHA-256 uses OpenSSL only in this outer adapter.
// Opens each path component without symlinks, never shells out and never returns secret/digest.
core::target::Outcome<std::unique_ptr<SecretDeliveryPort>>
make_secret_file_delivery(std::vector<SecretFileRecipient> recipients,
                          SecretFileCheckpoint checkpoint_for_testing = nullptr);

} // namespace kiarabot::control::target
