#pragma once

#include <functional>
#include <memory>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/observation_feed_application.hpp"
#include "kiarabot/core/application/target/operation_observation_application.hpp"
#include "kiarabot/core/application/target/runtime_action_application.hpp"
#include "kiarabot/core/application/target/runtime_audit_application.hpp"
#include "kiarabot/core/application/target/runtime_host_application.hpp"
#include "kiarabot/core/application/target/runtime_resource_application.hpp"
#include "kiarabot/core/application/target/runtime_resources/bootstrap.hpp"
#include "kiarabot/core/application/target/runtime_storage_application.hpp"
#include "kiarabot/v1/runtime.grpc.pb.h"

namespace kiarabot::core::platform {
namespace target = kiarabot::core::target;

// Trusted outer-composition dependency. It authenticates the actual inbound mTLS peer and verifies
// the current Identity delegation/authorization chain for this Runtime method, returning a trusted
// typed AuthenticationContext. It must never infer the caller from request fields, and while the
// Identity chain is not yet wired the composition root supplies a fail-closed implementation.
// A missing/unavailable chain returns Unauthenticated/UpstreamUnavailable, never an allow.
using RuntimePeerAuthorizer = std::function<target::Outcome<target::AuthenticationContext>(
    grpc::ServerContext& transport, const v1::RequestContext& request)>;

// Platform service principal verified from the actual inbound mTLS leaf SAN. Used only by internal
// platform handoffs (Scheduler -> Runtime.AcceptTimerTrigger) where the frozen design authorizes
// service peer + handoff integrity instead of a delegated user. Identity enrollment/assignment for
// this principal is an offline bootstrap prerequisite, not inferred from request fields.
struct RuntimePlatformPeer {
  target::Service service = target::Service::Scheduler;
  std::string san;
};
using RuntimePlatformPeerAuthorizer =
    std::function<target::Outcome<RuntimePlatformPeer>(grpc::ServerContext& transport)>;

struct RuntimeRpcOptions {
  RuntimePeerAuthorizer authorize;
  // Optional platform-peer path for AcceptTimerTrigger; absent keeps the method delegation-only.
  RuntimePlatformPeerAuthorizer platform_peer;
  // P09 internal-dispatcher storage/shared-namespace application. Null keeps the unary storage
  // methods at CAPABILITY_NOT_SUPPORTED (the external surface never gains these handlers by
  // accident). The owner mTLS transport still authenticates the peer and the application performs
  // the current online authorization/member gates for every call.
  target::RuntimeStorageApplication* storage = nullptr;
  // P12e read-only Control audit/log aggregation projection over this Runtime database.
  target::RuntimeAuditApplication* audit = nullptr;
  // Optional P07 Action application. When absent QueryActionResult is still wired but returns
  // CAPABILITY_NOT_SUPPORTED; ListActionMetadata and the internal-dispatcher Action methods stay
  // contract not_implemented per doc/01 (target_spec exposure=internal_dispatcher), not accidental.
  target::RuntimeActionApplication* actions = nullptr;
  // Optional process-level deadline bound (shared shutdown budget); expired -> TIMEOUT.
  std::function<target::Outcome<target::TimePoint>()> invocation_deadline;
  target::OperationObservationApplication* observations = nullptr;
  // v1.2 safe metadata feeds (logs and received-message metadata). Null keeps the handler a typed
  // CAPABILITY_NOT_SUPPORTED instead of an accidental projector.
  target::ObservationFeedApplication* log_changes = nullptr;
  target::ObservationFeedApplication* message_changes = nullptr;
  // BOOT-02 typed controlled bootstrap delivery. Null keeps the five delivery methods at
  // CAPABILITY_NOT_SUPPORTED instead of an accidental handler.
  target::RuntimeBootstrapDeliveryApplication* bootstrap = nullptr;
  // Verified Control platform peer: actual inbound mTLS leaf fingerprint plus explicit deployment
  // allowlist. Absent means delivery reports/queries are refused (fail closed, never implicit).
  RuntimePlatformPeerAuthorizer control_peer;
  // HOST-03 Runtime-owned Host admission gate. Null keeps SetHostAdmissionGate at
  // CAPABILITY_NOT_SUPPORTED; the application validates the actual Host leaf allowlist, live
  // HostBridge fence, current Identity decision and monotonic persisted gate version.
  target::RuntimeHostGateApplication* host_gate = nullptr;
};

// Transport adapter for the P05 production unary surface: accounts, explicit bindings, plugin
// installation/instance/config, typed resource queries and operation queries. The nine P06
// internal_dispatcher unary methods (RegisterAdapter/Heartbeat/PublishEvent/RegisterPlugin/
// HeartbeatPlugin/ClaimEvent/DeliverEvent/AckDelivery/RenewDelivery) are deliberately NOT
// overridden here: they are contract-declared not_implemented and remain generated UNIMPLEMENTED
// by design (doc/01 P06 topology; target_spec exposure=internal_dispatcher). The production
// AdapterBridge/PluginBridge streams are the external entry points. Only conversion and stable
// error mapping live here; all business rules stay in RuntimeResourceApplication.
target::Outcome<std::unique_ptr<v1::RuntimeService::Service>>
make_runtime_rpc(target::RuntimeResourceApplication& application, RuntimeRpcOptions options);

} // namespace kiarabot::core::platform
