#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/v1/policy.pb.h"
#include "kiarabot/v1/runtime.pb.h"

namespace kiarabot::control::target {

struct OwnerHttpRequest {
  std::string method;
  std::string target;
  std::string body;
  std::map<std::string, std::string> headers;
  // Bearer access token supplied by the authenticated Control caller. It is only used as the
  // subject proof for Identity IssueDelegation; it is never logged or persisted.
  std::string access_token;
  core::target::TimePoint deadline;
};
struct OwnerHttpResult {
  unsigned int status = 200;
  std::string body;
};
struct OwnerFailure {
  kiarabot::v1::ErrorCode code = kiarabot::v1::INTERNAL;
  bool retryable = false;
};
template <typename Value> using OwnerOutcome = std::variant<Value, OwnerFailure>;

// Issues a single-use delegation for exactly one owner audience/permission/scope. Implementations
// call Identity.IssueDelegation with the caller's proof and Control's mTLS identity; they never
// authorize locally or reuse a cached decision.
class DelegationPort {
public:
  virtual ~DelegationPort() = default;
  virtual OwnerOutcome<std::string> issue(std::string_view audience,
                                          kiarabot::v1::Permission permission,
                                          const kiarabot::v1::ResourceScope& scope,
                                          std::string_view access_token,
                                          core::target::TimePoint deadline) = 0;
};

// Runtime owner port: forwards exactly the frozen QueryActionResult RPC. Control does not infer
// action success/failure from transport errors.
class RuntimeActionsPort {
public:
  virtual ~RuntimeActionsPort() = default;
  virtual OwnerOutcome<kiarabot::v1::RuntimeQueryActionResultResponse>
  query_action(const kiarabot::v1::RuntimeQueryActionResultRequest& request,
               std::string_view delegation) = 0;
};

// Stable status/body helpers shared by the Control HTTPS boundary.
unsigned int owner_http_status(kiarabot::v1::ErrorCode code);
std::string owner_error_body(kiarabot::v1::ErrorCode code, std::string_view request_id,
                             std::string_view trace_id, bool retryable);

// P12c first-batch REST surface backed by owner RPCs:
//  - GET /api/v1/actions/{actionId}?scope=<json>            (action.inspect)
//  - GET /api/v1/actions/by-idempotency-key?scope&owner_instance (+ Idempotency-Key header)
//  - GET /api/v1/logs, /api/v1/audit: authoritative source not yet registered -> 503 UNAVAILABLE
//    (never a successful empty page; log.read/audit.read permissions land with the P12a contract)
// Returns nullopt for paths owned by another handler (auth, health, model, tasks).
class OwnerGateway {
public:
  OwnerGateway(DelegationPort& delegation, RuntimeActionsPort& runtime)
      : delegation_(delegation), runtime_(runtime) {}
  std::optional<OwnerHttpResult> handle(const OwnerHttpRequest& request);
  // Health probes are local process facts plus the composition-provided readiness answer.
  static OwnerHttpResult live();
  static OwnerHttpResult ready(bool ready, std::string_view request_id);

private:
  DelegationPort& delegation_;
  RuntimeActionsPort& runtime_;
};

} // namespace kiarabot::control::target
