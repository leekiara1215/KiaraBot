#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "kiarabot/core/application/target/runtime_resources/types.hpp"
#include "kiarabot/core/ports/target/runtime_bootstrap_deliveries.hpp"

namespace kiarabot::core::target {

// BOOT-02 controlled bootstrap delivery. Adapter and plugin instances use the same two bridge
// families as P06 but never share a credential namespace; their RPC methods stay distinct for the
// same reason. Plaintext exists only in the first committed response's memory.
enum class BootstrapPeerFamily { Adapter, Plugin };

struct BootstrapDeliveryIssue {
  BootstrapDeliveryRecord delivery;
  // Empty when an already-active credential/intent is reported without a new issuance.
  std::string plaintext;
};

struct BootstrapReissueResult {
  BootstrapDeliveryRecord delivery;
  // Present only on the first committed reissue response. Replay and recovery queries never
  // reconstruct it from the persisted SHA-256 digest.
  std::string plaintext;
  // Durable Runtime operation of the reissue (first response and idempotent replay).
  Operation operation{};
};

struct BootstrapDeliveryQuery {
  CommandContext context;
  ResourceRef resource{ResourceKind::Account, {}};
  // Empty asks for the latest intent of the resource; otherwise an exact delivery identity.
  std::string delivery_id;
};

struct ReissueBootstrapCommand {
  CommandContext context;
  ResourceRef resource{ResourceKind::Account, {}};
  std::string recipient_id;
  std::optional<std::uint64_t> expected_credential_version;
};

struct ReportBootstrapDeliveryCommand {
  CommandContext context;
  ResourceRef resource{ResourceKind::Account, {}};
  std::string delivery_id;
  std::string recipient_id;
  std::uint64_t credential_version = 0;
  // Only Delivered or Unknown can be reported; Pending/Revoked are runtime-owned facts.
  BootstrapDeliveryState state = BootstrapDeliveryState::Pending;
  std::string receipt_id;
  TimePoint observed_at{};
  // Set only by the boundary after it verified this call came from the registered Control platform
  // peer (actual mTLS leaf/SAN plus explicit deployment allowlist). A delegated user alone can
  // never mark a delivery DELIVERED.
  bool control_peer_authorized = false;
};

namespace runtime_resources_detail {

inline constexpr std::size_t kMaximumBootstrapIdentifierLength = 128;
// Highest persisted credential version the wire uint64 domain can represent; a reissue at this
// version is rejected instead of overflowing.
inline constexpr std::uint64_t kMaximumBootstrapCredentialVersion =
    std::numeric_limits<std::uint64_t>::max();
// Cached local gate bound for the frozen contract: a revoked/superseded bootstrap credential must
// stop authorizing new bridge proof/refresh within this interval.
inline constexpr auto kBootstrapVersionGateInterval = std::chrono::milliseconds{500};

bool canonical_bootstrap_identifier(std::string_view value);
bool valid_bootstrap_recipient_id(std::string_view value);
bool valid_bootstrap_delivery_id(std::string_view value);
bool valid_bootstrap_receipt_id(std::string_view value);
Permission bootstrap_manage_permission(BootstrapPeerFamily family);

// Normalized persisted version of a credential record. Legacy fixtures may materialize version 0;
// the frozen bridge contract issued kBridgeCredentialVersion (1) for them, so 0 normalizes to 1.
std::uint64_t normalized_bootstrap_credential_version(const BridgeCredentialRecord& record);

// Next persisted credential version: latest+1, or 1 when the resource never had one. The persisted
// version is authoritative; plaintext is never re-derived from it.
Outcome<std::uint64_t>
next_bootstrap_credential_version(const std::optional<BridgeCredentialRecord>& latest);

// Explicit reissue precondition. Absence of an expected version means "whatever is current" (the
// repository CAS still serializes concurrent reissues); a present value must equal the persisted
// version. Never revive a superseded authorization identity.
Status validate_expected_credential_version(std::optional<std::uint64_t> expected,
                                            const std::optional<BridgeCredentialRecord>& latest);

// One Control delivery report applied to the authoritative intent. `current_credential_version` is
// the latest persisted credential version. DELIVERED requires a bounded opaque receipt; a
// superseded version, a downgrade from DELIVERED, a recipient mismatch or a revoked intent fail
// closed. Reporting the same evidence twice is the same result, never a second side effect.
Outcome<BootstrapDeliveryRecord>
apply_bootstrap_delivery_report(const BootstrapDeliveryRecord& current,
                                const ReportBootstrapDeliveryCommand& command,
                                std::uint64_t current_credential_version, TimePoint now);

// No-secret delivery metadata reconstructed for an idempotent BindAdapter/CreateInstance replay.
// Returns empty when the request never opted in, no durable intent exists, or the current intent
// belongs to another fixed recipient. Plaintext is never reconstructed.
Outcome<std::optional<BootstrapDeliveryRecord>>
current_delivery_metadata(RuntimeUnitOfWorkFactory& transactions, const CommandContext& context,
                          const ResourceRef& resource, const std::string& recipient_id);

// Same-UoW issuance used by BindAdapter/CreateInstance after a non-empty explicit recipient opts
// in. Persists digest + real monotonic version + recipient-bound intent + operation; the plaintext
// stays only in the caller's response object. Returns an empty optional when an active credential
// already exists and no matching intent is available (explicit reissue is then the only way to
// obtain a new plaintext).
Outcome<std::optional<BootstrapDeliveryIssue>>
issue_bootstrap_delivery(RuntimeUnitOfWork& work, RuntimeIdPort& ids, RuntimeSecretPort& secrets,
                         const ResourceRef& resource, BootstrapPeerFamily family,
                         const std::string& credential_resource_id, const std::string& recipient_id,
                         const OperationRef& operation, TimePoint now);

} // namespace runtime_resources_detail

struct BootstrapDeliveryDependencies {
  RuntimeUnitOfWorkFactory& transactions;
  RuntimeIdPort& ids;
  RuntimeSecretPort& secrets;
  RuntimeAuthorizationPort& authorization;
};

// Typed application boundary for the five frozen bootstrap delivery methods. Every method performs
// current online authorization before effects, owns its UoWs and never calls the delivery sink.
class RuntimeBootstrapDeliveryApplication {
public:
  virtual ~RuntimeBootstrapDeliveryApplication() = default;
  virtual Outcome<BootstrapDeliveryRecord> get(const BootstrapDeliveryQuery& request) = 0;
  virtual Outcome<BootstrapReissueResult> reissue(const ReissueBootstrapCommand& request,
                                                  BootstrapPeerFamily family) = 0;
  virtual Outcome<BootstrapDeliveryRecord> report(const ReportBootstrapDeliveryCommand& request,
                                                  BootstrapPeerFamily family) = 0;
};

namespace runtime_resources_detail {
Outcome<std::unique_ptr<RuntimeBootstrapDeliveryApplication>>
make_runtime_bootstrap_delivery_application(BootstrapDeliveryDependencies dependencies);
} // namespace runtime_resources_detail

} // namespace kiarabot::core::target
