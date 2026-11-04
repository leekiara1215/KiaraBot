#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

// BOOT-02 controlled bootstrap delivery. These values never contain the credential plaintext, its
// digest, a filesystem path, an owner UID/GID or an environment value; the only persisted secret
// material remains the SHA-256 digest owned by bridge_credentials. DELIVERED only means the exact
// version was durably installed at the fixed deployment recipient; it never claims that the peer
// registered, that a process ran or that configuration was applied.
enum class BootstrapDeliveryState { Pending, Delivered, Unknown, Revoked };

struct BootstrapDeliveryRecord {
  std::string delivery_id;
  ResourceRef resource{ResourceKind::Account, {}};
  std::string recipient_id;
  std::uint64_t credential_version = 0;
  BootstrapDeliveryState state = BootstrapDeliveryState::Pending;
  OperationRef operation{Service::Runtime, {}, {ResourceKind::Account, {}}};
  TimePoint created_at{};
  TimePoint observed_at{};
  // Opaque sink receipt identity supplied by the registered Control peer. Never a secret.
  std::string receipt_id;
  bool operator==(const BootstrapDeliveryRecord&) const = default;
};

// Runtime-owned, no-secret delivery intent/receipt rows. Implementations participate in the
// caller's transaction (no commit/rollback, no remote or filesystem I/O, no authorization rule) and
// are only reached through the owning Runtime unit of work.
class BootstrapDeliveryRepository {
public:
  virtual ~BootstrapDeliveryRepository() = default;
  // Exact delivery identity for a resource; empty when it never existed.
  virtual Outcome<std::optional<BootstrapDeliveryRecord>> find(const ResourceRef& resource,
                                                               const std::string& delivery_id) = 0;
  // Latest intent for the resource, including Pending/Unknown/Revoked rows.
  virtual Outcome<std::optional<BootstrapDeliveryRecord>> current(const ResourceRef& resource) = 0;
  // Insert a new intent. A duplicate delivery identity is CONFLICT, never an overwrite.
  virtual Status insert(const BootstrapDeliveryRecord& record) = 0;
  // Compare-and-set state/observed_at/receipt_id of an existing intent. A stale expected state is
  // CONFLICT; a missing row is NOT_FOUND.
  virtual Status transition(const BootstrapDeliveryRecord& next,
                            BootstrapDeliveryState expected_state) = 0;
};

} // namespace kiarabot::core::target
