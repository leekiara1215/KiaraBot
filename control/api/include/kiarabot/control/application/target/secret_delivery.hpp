#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::control::target {

struct SecretDeliveryIdentity {
  std::string delivery_id;
  core::target::ResourceRef resource;
  std::string recipient_id;
  std::uint64_t credential_version = 0;
  bool operator==(const SecretDeliveryIdentity&) const = default;
};

struct SecretDeliveryRequest {
  SecretDeliveryIdentity identity;
  // Zero for the first delivery; then the latest issued version, including an unresolved intent.
  std::uint64_t expected_version = 0;
  // Borrowed only for this synchronous call; never copied into a receipt/query result.
  std::string_view secret;
};

enum class SecretDeliveryState { Delivered, Unknown };
struct SecretDeliveryReceipt {
  SecretDeliveryIdentity identity;
  SecretDeliveryState state = SecretDeliveryState::Unknown;
  std::string receipt_id;
  bool operator==(const SecretDeliveryReceipt&) const = default;
};

// No authority database or authorization decision lives here. The caller must supply an
// owner-authorized intent; the sink additionally enforces its offline exact recipient binding.
// Identity + identical material is idempotent. Conflicts never overwrite a later version.
class SecretDeliveryPort {
public:
  virtual ~SecretDeliveryPort() = default;
  virtual core::target::Outcome<SecretDeliveryReceipt> store(const SecretDeliveryRequest&) = 0;
  virtual core::target::Outcome<std::optional<SecretDeliveryReceipt>>
  query(const SecretDeliveryIdentity&) = 0;
};

class SecretDeliveryApplication {
public:
  explicit SecretDeliveryApplication(SecretDeliveryPort& port) : port_(port) {}
  core::target::Outcome<SecretDeliveryReceipt> store(const SecretDeliveryRequest& request);
  core::target::Outcome<std::optional<SecretDeliveryReceipt>>
  query(const SecretDeliveryIdentity& identity);

private:
  SecretDeliveryPort& port_;
};

// Bounded opaque identifiers, never a path/URL; used for both caller values and trusted bindings.
bool valid_delivery_identity(const SecretDeliveryIdentity& identity);

} // namespace kiarabot::control::target
