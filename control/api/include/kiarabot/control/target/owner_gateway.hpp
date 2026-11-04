#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "kiarabot/control/application/target/secret_delivery.hpp"
#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/v1/host.pb.h"
#include "kiarabot/v1/identity.pb.h"
#include "kiarabot/v1/model.pb.h"
#include "kiarabot/v1/policy.pb.h"
#include "kiarabot/v1/runtime.pb.h"
#include "kiarabot/v1/scheduler.pb.h"

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
  virtual OwnerOutcome<kiarabot::v1::ResourceScope>
  catalog_scope(kiarabot::v1::Permission, const kiarabot::v1::ResourceRef&,
                kiarabot::v1::ResourceKind, const std::optional<kiarabot::v1::ResourceScope>&,
                std::string_view, core::target::TimePoint) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }

  virtual OwnerOutcome<std::string> issue(std::string_view audience,
                                          kiarabot::v1::Permission permission,
                                          const kiarabot::v1::ResourceScope& scope,
                                          std::string_view access_token,
                                          core::target::TimePoint deadline) = 0;
};

// Runtime owner port: forwards exactly the frozen QueryActionResult RPC. Control does not infer
// action success/failure from transport errors.
// Identity principal catalog: human principals only, snapshot page. Identity filters by the
// caller's currently granted scope; Control only forwards.
class IdentityPrincipalsPort {
public:
  virtual ~IdentityPrincipalsPort() = default;
  virtual OwnerOutcome<kiarabot::v1::IdentityListPrincipalsResponse>
  list_principals(const kiarabot::v1::IdentityListPrincipalsRequest& request,
                  std::string_view delegation) = 0;
};

// Control-owned observation of the four direct business dependencies. healthy() means the
// dependency is usable right now; configured distinguishes "endpoint not set" from "unavailable".
struct HealthObservation {
  bool identity_ready = false;
  bool runtime_ready = false;
  bool scheduler_ready = false;
  bool model_ready = false;
  bool runtime_configured = false;
  bool scheduler_configured = false;
  bool model_configured = false;
  core::target::TimePoint checked_at;
};
class HealthPort {
public:
  virtual ~HealthPort() = default;
  virtual HealthObservation observe() = 0;
};

// Remaining P12 owner RPCs exposed through the same one-use-delegation gateway.
class RuntimeResourcesPort {
public:
  virtual ~RuntimeResourcesPort() = default;
  virtual OwnerOutcome<kiarabot::v1::RuntimeCreateAccountResponse>
  create_account(const kiarabot::v1::RuntimeCreateAccountRequest&, std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }

  virtual OwnerOutcome<kiarabot::v1::RuntimeListAccountsResponse>
  list_accounts(const kiarabot::v1::RuntimeListAccountsRequest&, std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }

  virtual OwnerOutcome<kiarabot::v1::RuntimeListInstallationsResponse>
  list_installations(const kiarabot::v1::RuntimeListInstallationsRequest&, std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }

  virtual OwnerOutcome<kiarabot::v1::RuntimeListInstancesResponse>
  list_instances(const kiarabot::v1::RuntimeListInstancesRequest&, std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }

  virtual OwnerOutcome<kiarabot::v1::RuntimeListAdaptersResponse>
  list_adapters(const kiarabot::v1::RuntimeListAdaptersRequest&, std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }

  virtual OwnerOutcome<kiarabot::v1::RuntimeListHostsResponse>
  list_hosts(const kiarabot::v1::RuntimeListHostsRequest&, std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }

  virtual OwnerOutcome<kiarabot::v1::RuntimeBindAdapterResponse>
  bind_adapter(const kiarabot::v1::RuntimeBindAdapterRequest&, std::string_view) = 0;
  virtual OwnerOutcome<kiarabot::v1::RuntimeGetBootstrapDeliveryResponse>
  get_bootstrap_delivery(const kiarabot::v1::RuntimeGetBootstrapDeliveryRequest&,
                         std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }
  virtual OwnerOutcome<kiarabot::v1::RuntimeReissueBootstrapResponse>
  reissue_adapter_bootstrap(const kiarabot::v1::RuntimeReissueAdapterBootstrapRequest&,
                            std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }
  virtual OwnerOutcome<kiarabot::v1::RuntimeReissueBootstrapResponse>
  reissue_plugin_bootstrap(const kiarabot::v1::RuntimeReissuePluginBootstrapRequest&,
                           std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }
  virtual OwnerOutcome<kiarabot::v1::RuntimeGetBootstrapDeliveryResponse>
  report_adapter_bootstrap_delivery(
      const kiarabot::v1::RuntimeReportAdapterBootstrapDeliveryRequest&, std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }
  virtual OwnerOutcome<kiarabot::v1::RuntimeGetBootstrapDeliveryResponse>
  report_plugin_bootstrap_delivery(const kiarabot::v1::RuntimeReportPluginBootstrapDeliveryRequest&,
                                   std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }
  virtual OwnerOutcome<kiarabot::v1::RuntimeUnbindAdapterResponse>
  unbind_adapter(const kiarabot::v1::RuntimeUnbindAdapterRequest&, std::string_view) = 0;
  virtual OwnerOutcome<kiarabot::v1::RuntimeCreateInstallationResponse>
  create_installation(const kiarabot::v1::RuntimeCreateInstallationRequest&, std::string_view) = 0;
  virtual OwnerOutcome<kiarabot::v1::RuntimeCreateInstanceResponse>
  create_instance(const kiarabot::v1::RuntimeCreateInstanceRequest&, std::string_view) = 0;
  virtual OwnerOutcome<kiarabot::v1::RuntimeSetDesiredStateResponse>
  set_desired_state(const kiarabot::v1::RuntimeSetDesiredStateRequest&, std::string_view) = 0;
  virtual OwnerOutcome<kiarabot::v1::RuntimeUpdateConfigResponse>
  update_config(const kiarabot::v1::RuntimeUpdateConfigRequest&, std::string_view) = 0;
  virtual OwnerOutcome<kiarabot::v1::RuntimeGetInstanceConfigResponse>
  get_instance_config(const kiarabot::v1::RuntimeGetInstanceConfigRequest&, std::string_view) = 0;
  virtual OwnerOutcome<kiarabot::v1::RuntimeGetActionPolicyResponse>
  get_action_policy(const kiarabot::v1::RuntimeGetActionPolicyRequest&, std::string_view) = 0;
  virtual OwnerOutcome<kiarabot::v1::RuntimeUpdateActionPolicyResponse>
  update_action_policy(const kiarabot::v1::RuntimeUpdateActionPolicyRequest&, std::string_view) = 0;
  virtual OwnerOutcome<kiarabot::v1::RuntimeBeginDeleteResponse>
  begin_delete(const kiarabot::v1::RuntimeBeginDeleteRequest&, std::string_view) = 0;
  virtual OwnerOutcome<kiarabot::v1::RuntimeRestoreResourceResponse>
  restore_resource(const kiarabot::v1::RuntimeRestoreResourceRequest&, std::string_view) = 0;
};
class SchedulerTasksPort {
public:
  virtual ~SchedulerTasksPort() = default;
  virtual OwnerOutcome<kiarabot::v1::SchedulerListTasksResponse>
  list_tasks(const kiarabot::v1::SchedulerListTasksRequest&, std::string_view) = 0;
  virtual OwnerOutcome<kiarabot::v1::SchedulerCancelTaskResponse>
  cancel_task(const kiarabot::v1::SchedulerCancelTaskRequest&, std::string_view) = 0;
  virtual OwnerOutcome<kiarabot::v1::SchedulerGetTaskResponse>
  get_task(const kiarabot::v1::SchedulerGetTaskRequest&, std::string_view) = 0;
  virtual OwnerOutcome<kiarabot::v1::SchedulerGetOccurrenceResponse>
  get_occurrence(const kiarabot::v1::SchedulerGetOccurrenceRequest&, std::string_view) = 0;
};

// P12e read-only audit/log projections aggregated across Identity and Runtime. Each source keeps
// its own authority: Identity authenticates Control as its platform peer directly; Runtime requires
// a per-request Control delegation for log.read(48)/audit.read(49) on the Control catalog scope.
enum class AuditLogKind { Audit, Logs };
struct AuditLogFilter {
  std::optional<core::target::TimePoint> occurred_from;
  std::optional<core::target::TimePoint> occurred_until;
  std::string actor;
  std::optional<kiarabot::v1::ResourceRef> resource;
  std::uint32_t page_size = 20;
  std::string page_token;
};
struct AuditLogRow {
  kiarabot::v1::ResourceKind resource_kind = kiarabot::v1::RESOURCE_KIND_UNSPECIFIED;
  std::string entry_id;
  std::string owner;
  std::string actor;
  std::string resource_id;
  std::string request_id;
  std::string trace_id;
  std::string method;
  std::string rejection;
  std::string severity;
  std::string component;
  std::string message;
  std::string account_id;
  std::string conversation_id;
  std::uint64_t authorization_version = 0;
  core::target::TimePoint occurred_at;
};
struct AuditLogSourcePage {
  std::vector<AuditLogRow> rows;
  std::string next_token;
};
class AuditLogSourcePort {
public:
  virtual ~AuditLogSourcePort() = default;
  virtual OwnerOutcome<AuditLogSourcePage> identity(AuditLogKind kind,
                                                    const AuditLogFilter& filter) = 0;
  virtual OwnerOutcome<AuditLogSourcePage> runtime(AuditLogKind kind, const AuditLogFilter& filter,
                                                   std::string_view delegation) = 0;
};

// Authoritative owner operation snapshot. Each owner stores its own operation; Control only
// routes to the named owner and never synthesizes progress.
class OperationPort {
public:
  virtual ~OperationPort() = default;
  virtual OwnerOutcome<kiarabot::v1::Operation> get(std::string_view owner_service,
                                                    const kiarabot::v1::OperationRef& operation,
                                                    std::string_view delegation) = 0;
};

class RuntimeActionsPort {
public:
  virtual ~RuntimeActionsPort() = default;
  virtual OwnerOutcome<kiarabot::v1::RuntimeQueryActionResultResponse>
  query_action(const kiarabot::v1::RuntimeQueryActionResultRequest& request,
               std::string_view delegation) = 0;
};

// P11d Model owner port: forwards exactly the frozen ModelService management/query RPCs. Control
// only converts REST<->typed RPC, issues one delegation and maps stable errors; it never reads
// provider secrets, re-implements lifecycle rules or infers UNKNOWN results from transport errors.
class ModelManagementPort {
public:
  virtual ~ModelManagementPort() = default;
  virtual OwnerOutcome<kiarabot::v1::ModelGetModelMetadataResponse>
  metadata(const kiarabot::v1::ModelGetModelMetadataRequest& request,
           std::string_view delegation) = 0;
  virtual OwnerOutcome<kiarabot::v1::ModelListModelMetadataResponse>
  list_metadata(const kiarabot::v1::ModelListModelMetadataRequest& request,
                std::string_view delegation) = 0;
  virtual OwnerOutcome<kiarabot::v1::ModelUpdateModelResponse>
  update(const kiarabot::v1::ModelUpdateModelRequest& request, std::string_view delegation) = 0;
  virtual OwnerOutcome<kiarabot::v1::ModelDisableModelResponse>
  disable(const kiarabot::v1::ModelDisableModelRequest& request, std::string_view delegation) = 0;
  virtual OwnerOutcome<kiarabot::v1::ModelEnableModelResponse>
  enable(const kiarabot::v1::ModelEnableModelRequest& request, std::string_view delegation) = 0;
  virtual OwnerOutcome<kiarabot::v1::ModelRotateProviderSecretResponse>
  rotate_secret(const kiarabot::v1::ModelRotateProviderSecretRequest& request,
                std::string_view delegation) = 0;
  virtual OwnerOutcome<kiarabot::v1::ModelGetModelConfigurationResponse>
  configuration(const kiarabot::v1::ModelGetModelConfigurationRequest& request,
                std::string_view delegation) = 0;
  virtual OwnerOutcome<kiarabot::v1::ModelGetCallStatusResponse>
  call_status(const kiarabot::v1::ModelGetCallStatusRequest& request,
              std::string_view delegation) = 0;
  virtual OwnerOutcome<kiarabot::v1::ModelResolveModelCallResponse>
  resolve(const kiarabot::v1::ModelResolveModelCallRequest& request,
          std::string_view delegation) = 0;
  virtual OwnerOutcome<kiarabot::v1::ModelGetOperationResponse>
  operation(const kiarabot::v1::ModelGetOperationRequest& request, std::string_view delegation) = 0;
};

// Host owner port (HOST-04): forwards exactly the frozen HostManagementService RPCs to the fixed
// offline registered Host endpoint. Control never supplies an endpoint, synthesizes progress or
// converts a transport failure into a completed maintenance operation.
class HostManagementPort {
public:
  virtual ~HostManagementPort() = default;
  virtual OwnerOutcome<kiarabot::v1::HostGetMaintenanceResponse>
  get_maintenance(const kiarabot::v1::HostGetMaintenanceRequest&, std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }
  virtual OwnerOutcome<kiarabot::v1::HostMaintenanceResponse>
  begin_maintenance(const kiarabot::v1::HostBeginMaintenanceRequest&, std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }
  virtual OwnerOutcome<kiarabot::v1::HostMaintenanceResponse>
  resume_admission(const kiarabot::v1::HostResumeAdmissionRequest&, std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }
  virtual OwnerOutcome<kiarabot::v1::HostGetOperationResponse>
  get_operation(const kiarabot::v1::HostGetOperationRequest&, std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }
  virtual OwnerOutcome<kiarabot::v1::HostGetOperationResponse>
  cancel_operation(const kiarabot::v1::HostCancelOperationRequest&, std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }
  virtual OwnerOutcome<kiarabot::v1::OperationChangesResponse>
  list_operation_changes(const kiarabot::v1::OperationChangesRequest&, std::string_view) {
    return OwnerFailure{kiarabot::v1::CAPABILITY_NOT_SUPPORTED, false};
  }
};

// Stable status/body helpers shared by the Control HTTPS boundary.
unsigned int owner_http_status(kiarabot::v1::ErrorCode code);
std::string owner_error_body(kiarabot::v1::ErrorCode code, std::string_view request_id,
                             std::string_view trace_id, bool retryable);

// REST boundary for owner-backed health/principal/catalog queries, Runtime resources and
// Actions, Scheduler tasks/occurrences, Model management, owner operations and audit/log pages.
// Internal resource-family handlers share request correlation and stable error conversion;
// authorization and business state remain with the existing delegation/owner ports.
// Returns nullopt when the request belongs to another HTTPS handler.
// Offline fixed-recipient registry predicate. Never derived from a delivery/HTTP request.
class BootstrapRecipientsPort {
public:
  virtual ~BootstrapRecipientsPort() = default;
  virtual bool known(std::string_view recipient_id,
                     const kiarabot::v1::ResourceRef& resource) const = 0;
};

// BOOT-04 composition: parses the offline non-secret recipient registry (`schema`/`recipients`)
// and builds the fixed-directory SecretDeliveryPort, its application and the recipient predicate.
struct ControlBootstrapDelivery {
  std::unique_ptr<SecretDeliveryPort> port;
  std::unique_ptr<SecretDeliveryApplication> application;
  std::unique_ptr<BootstrapRecipientsPort> recipients;
};
core::target::Outcome<ControlBootstrapDelivery>
make_control_bootstrap_delivery(const std::string& registry_path);

class OwnerGateway {
public:
  OwnerGateway(DelegationPort& delegation, RuntimeActionsPort& runtime,
               IdentityPrincipalsPort* principals = nullptr, HealthPort* health = nullptr,
               RuntimeResourcesPort* resources = nullptr, SchedulerTasksPort* tasks = nullptr,
               OperationPort* operations = nullptr, ModelManagementPort* model = nullptr,
               AuditLogSourcePort* audit_logs = nullptr,
               SecretDeliveryApplication* secret_delivery = nullptr,
               BootstrapRecipientsPort* bootstrap_recipients = nullptr,
               HostManagementPort* host_management = nullptr)
      : delegation_(delegation), runtime_(runtime), principals_(principals), health_(health),
        resources_(resources), tasks_(tasks), operations_(operations), model_(model),
        audit_logs_(audit_logs), secret_delivery_(secret_delivery),
        bootstrap_recipients_(bootstrap_recipients), host_management_(host_management) {}
  std::optional<OwnerHttpResult> handle(const OwnerHttpRequest& request);
  // Health probes are local process facts plus the composition-provided readiness answer.
  static OwnerHttpResult live();
  static OwnerHttpResult ready(bool ready, std::string_view request_id);

private:
  DelegationPort& delegation_;
  RuntimeActionsPort& runtime_;
  IdentityPrincipalsPort* principals_;
  HealthPort* health_;
  RuntimeResourcesPort* resources_;
  SchedulerTasksPort* tasks_;
  OperationPort* operations_;
  ModelManagementPort* model_;
  AuditLogSourcePort* audit_logs_;
  SecretDeliveryApplication* secret_delivery_;
  BootstrapRecipientsPort* bootstrap_recipients_;
  HostManagementPort* host_management_;
};

} // namespace kiarabot::control::target
