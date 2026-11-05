#include "kiarabot/core/runtime/platform/host_management_rpc.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "kiarabot/core/runtime/platform/operation_observation_rpc.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace t = target;
namespace v1 = kiarabot::v1;
namespace codec = target_codec;

constexpr std::string_view kGetMethod = "kiarabot.v1.HostManagementService/GetMaintenance";
constexpr std::string_view kBeginMethod = "kiarabot.v1.HostManagementService/BeginMaintenance";
constexpr std::string_view kResumeMethod = "kiarabot.v1.HostManagementService/ResumeAdmission";
constexpr std::string_view kOperationMethod = "kiarabot.v1.HostManagementService/GetOperation";
constexpr std::string_view kCancelMethod = "kiarabot.v1.HostManagementService/CancelOperation";

t::Failure invalid() {
  return {
      t::ErrorCode::InvalidArgument, t::Acceptance::NotAccepted, t::Recovery::CorrectInput, {}, {}};
}
t::Failure unauthenticated() {
  return {
      t::ErrorCode::Unauthenticated, t::Acceptance::NotAccepted, t::Recovery::Authenticate, {}, {}};
}
t::Failure timeout() {
  return {
      t::ErrorCode::Timeout, t::Acceptance::NotAccepted, t::Recovery::QuerySameIdentity, {}, {}};
}
t::Failure internal() {
  return {t::ErrorCode::Internal, t::Acceptance::Unknown, t::Recovery::QuerySameIdentity, {}, {}};
}
t::Failure not_found() {
  return {t::ErrorCode::NotFound, t::Acceptance::NotAccepted, t::Recovery::ReadCurrent, {}, {}};
}
bool canonical_uuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
      continue;
    }
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}
t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
bool timestamp(t::TimePoint value, google::protobuf::Timestamp* output) {
  const auto encoded = codec::to_wire(value);
  if (const auto* wire = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *output = *wire;
    return true;
  }
  return false;
}
bool resource_to_wire(const t::ResourceRef& input, v1::ResourceRef* output) {
  const auto encoded = codec::to_wire(input);
  if (!std::holds_alternative<v1::ResourceRef>(encoded)) {
    return false;
  }
  *output = std::get<v1::ResourceRef>(encoded);
  return true;
}
std::optional<v1::ResourceRef> host_scope(std::string_view host_id) {
  if (host_id.empty() || host_id.size() > 128) {
    return std::nullopt;
  }
  v1::ResourceRef scope;
  scope.set_kind(v1::RESOURCE_HOST);
  scope.set_id(std::string(host_id));
  return scope;
}
grpc::Status failure_status(const t::Failure& input, const std::string& request,
                            const std::string& trace) {
  v1::RpcFailure result;
  const auto error = codec::make_error(input.code, request, trace,
                                       input.recovery == t::Recovery::Backoff ||
                                           input.recovery == t::Recovery::QuerySameIdentity);
  const auto* stable = std::get_if<v1::Error>(&error);
  if (!stable) {
    return grpc::Status{grpc::StatusCode::INTERNAL, "INTERNAL"};
  }
  *result.mutable_error() = *stable;
  auto* context = result.mutable_context();
  switch (input.acceptance) {
  case t::Acceptance::NotAccepted:
    context->set_acceptance(v1::NOT_ACCEPTED);
    break;
  case t::Acceptance::Accepted:
    context->set_acceptance(v1::DURABLY_ACCEPTED);
    break;
  default:
    context->set_acceptance(v1::ACCEPTANCE_UNKNOWN);
    break;
  }
  switch (input.recovery) {
  case t::Recovery::CorrectInput:
    context->set_recovery(v1::CORRECT_INPUT);
    break;
  case t::Recovery::Authenticate:
    context->set_recovery(v1::REAUTHENTICATE);
    break;
  case t::Recovery::ReadCurrent:
    context->set_recovery(v1::RESYNC_SNAPSHOT);
    break;
  case t::Recovery::QuerySameIdentity:
    context->set_recovery(v1::QUERY_SAME_INTENT);
    break;
  case t::Recovery::Backoff:
    context->set_recovery(v1::RETRY_SAME_ID);
    break;
  default:
    context->set_recovery(v1::MANUAL_REVIEW);
    break;
  }
  if (input.operation) {
    auto* operation = context->mutable_operation();
    operation->set_owner_service("host");
    operation->set_operation_id(input.operation->id.value);
    if (!resource_to_wire(input.operation->resource, operation->mutable_resource())) {
      return grpc::Status{grpc::StatusCode::INTERNAL, "INTERNAL"};
    }
  }
  const auto status = codec::grpc_status(input.code);
  if (!std::holds_alternative<grpc::StatusCode>(status)) {
    return grpc::Status{grpc::StatusCode::INTERNAL, "INTERNAL"};
  }
  return grpc::Status{std::get<grpc::StatusCode>(status), result.error().message(),
                      result.SerializeAsString()};
}
bool operation(const t::Operation& input, const v1::RequestContext& context,
               v1::Operation* output) {
  const auto state = static_cast<int>(input.state);
  const auto outcome = static_cast<int>(input.execution_outcome);
  if (state < 0 || state > 6 || outcome < 0 || outcome > 3 || input.updated_at <= t::TimePoint{} ||
      input.ref.owner != t::Service::Host ||
      (input.state == t::OperationState::Blocked &&
       (!input.blocked_reason || !input.next_attempt))) {
    return false;
  }
  output->mutable_ref()->set_owner_service("host");
  output->mutable_ref()->set_operation_id(input.ref.id.value);
  if (!resource_to_wire(input.ref.resource, output->mutable_ref()->mutable_resource())) {
    return false;
  }
  output->set_version(input.version.value);
  output->set_state(static_cast<v1::OperationState>(state + 1));
  output->set_execution_outcome(static_cast<v1::ExecutionOutcome>(outcome + 1));
  if (input.blocked_reason) {
    const auto reason =
        codec::make_error(*input.blocked_reason, context.request_id(), context.trace_id(), true);
    const auto* encoded = std::get_if<v1::Error>(&reason);
    if (!encoded) {
      return false;
    }
    *output->mutable_reason() = *encoded;
  }
  if (input.next_attempt && !timestamp(*input.next_attempt, output->mutable_next_attempt_at())) {
    return false;
  }
  if (!timestamp(input.updated_at, output->mutable_observed_at())) {
    return false;
  }
  for (const auto& item : input.participants) {
    const auto participant_state = static_cast<int>(item.state);
    if (participant_state < 0 || participant_state > 6 || item.participant.empty() ||
        item.participant.size() > 128) {
      return false;
    }
    auto* participant = output->add_participants();
    participant->set_participant(item.participant);
    participant->set_state(static_cast<v1::OperationState>(participant_state + 1));
    if (item.reason) {
      const auto reason =
          codec::make_error(*item.reason, context.request_id(), context.trace_id(), true);
      const auto* encoded = std::get_if<v1::Error>(&reason);
      if (!encoded) {
        return false;
      }
      *participant->mutable_reason() = *encoded;
    }
    if (item.next_attempt &&
        !timestamp(*item.next_attempt, participant->mutable_next_attempt_at())) {
      return false;
    }
  }
  return true;
}
bool maintenance_record(const t::HostMaintenanceState& state, v1::HostMaintenanceRecord* output) {
  if (state.host.value.empty() || state.version.value == 0) {
    return false;
  }
  output->set_host_id(state.host.value);
  output->set_version(state.version.value);
  output->set_admission(state.admission_open ? v1::HOST_ADMISSION_OPEN : v1::HOST_ADMISSION_CLOSED);
  if (state.active_operation) {
    output->mutable_operation()->set_owner_service("host");
    output->mutable_operation()->set_operation_id(state.active_operation->id.value);
    if (!resource_to_wire(state.active_operation->resource,
                          output->mutable_operation()->mutable_resource())) {
      return false;
    }
  }
  return timestamp(state.observed_at, output->mutable_observed_at());
}
bool operation_identity(const v1::OperationRef& wire, std::string* identifier,
                        std::string* host_id) {
  if (wire.owner_service() != "host" || !canonical_uuid(wire.operation_id()) ||
      !wire.has_resource() || wire.resource().kind() != v1::RESOURCE_HOST ||
      wire.resource().id().empty() || wire.resource().id().size() > 128) {
    return false;
  }
  *identifier = wire.operation_id();
  *host_id = wire.resource().id();
  return true;
}
} // namespace

class HostManagementServiceAdapter final : public v1::HostManagementService::Service {
public:
  explicit HostManagementServiceAdapter(HostManagementRpcDependencies dependencies)
      : dependencies_(std::move(dependencies)) {}

  grpc::Status GetMaintenance(grpc::ServerContext* transport,
                              const v1::HostGetMaintenanceRequest* request,
                              v1::HostGetMaintenanceResponse* response) override {
    try {
      const auto& context = request->context();
      if (!request->has_context() || !valid_context(context, false)) {
        return failure_status(invalid(), context.request_id(), context.trace_id());
      }
      if (request->host_id().empty() || request->host_id().size() > 128) {
        return failure_status(invalid(), context.request_id(), context.trace_id());
      }
      if (request->host_id() != dependencies_.application.host_id().value) {
        // This owner serves only its stored host identity; a different wire label is NOT_FOUND.
        return failure_status(not_found(), context.request_id(), context.trace_id());
      }
      const auto scope = host_scope(request->host_id());
      if (!scope) {
        return failure_status(invalid(), context.request_id(), context.trace_id());
      }
      auto peer = authenticate(*transport, context, kGetMethod, *scope);
      if (const auto* failure = std::get_if<t::Failure>(&peer)) {
        return failure_status(*failure, context.request_id(), context.trace_id());
      }
      auto deadline = bounded_deadline(*transport, context, std::chrono::milliseconds{5000});
      if (const auto* failure = std::get_if<t::Failure>(&deadline)) {
        return failure_status(*failure, context.request_id(), context.trace_id());
      }
      auto state = dependencies_.application.maintenance_state(
          std::get<t::AuthenticationContext>(peer), std::get<t::TimePoint>(deadline));
      if (const auto* failure = std::get_if<t::Failure>(&state)) {
        return failure_status(*failure, context.request_id(), context.trace_id());
      }
      if (!maintenance_record(std::get<t::HostMaintenanceState>(state),
                              response->mutable_maintenance())) {
        return failure_status(internal(), context.request_id(), context.trace_id());
      }
      return grpc::Status::OK;
    } catch (...) {
      return grpc::Status{grpc::StatusCode::INTERNAL, "INTERNAL"};
    }
  }

  grpc::Status BeginMaintenance(grpc::ServerContext* transport,
                                const v1::HostBeginMaintenanceRequest* request,
                                v1::HostMaintenanceResponse* response) override {
    return maintain(transport, request->has_context(), request->context(), request->host_id(),
                    request->has_expected_version() ? request->expected_version() : 0,
                    request->grace_ms(), true, response, kBeginMethod);
  }

  grpc::Status ResumeAdmission(grpc::ServerContext* transport,
                               const v1::HostResumeAdmissionRequest* request,
                               v1::HostMaintenanceResponse* response) override {
    return maintain(transport, request->has_context(), request->context(), request->host_id(),
                    request->has_expected_version() ? request->expected_version() : 0, 0, false,
                    response, kResumeMethod);
  }

  grpc::Status GetOperation(grpc::ServerContext* transport,
                            const v1::HostGetOperationRequest* request,
                            v1::HostGetOperationResponse* response) override {
    try {
      const auto& context = request->context();
      if (!request->has_context() || !valid_context(context, false) || !request->has_operation()) {
        return failure_status(invalid(), context.request_id(), context.trace_id());
      }
      std::string identifier;
      std::string host_id;
      if (!operation_identity(request->operation(), &identifier, &host_id)) {
        return failure_status(invalid(), context.request_id(), context.trace_id());
      }
      if (host_id != dependencies_.application.host_id().value) {
        // Stored resource identity, not the caller wire label, decides visibility.
        return failure_status(not_found(), context.request_id(), context.trace_id());
      }
      const auto scope = host_scope(host_id);
      if (!scope) {
        return failure_status(invalid(), context.request_id(), context.trace_id());
      }
      auto peer = authenticate(*transport, context, kOperationMethod, *scope);
      if (const auto* failure = std::get_if<t::Failure>(&peer)) {
        return failure_status(*failure, context.request_id(), context.trace_id());
      }
      auto deadline = bounded_deadline(*transport, context, std::chrono::milliseconds{5000});
      if (const auto* failure = std::get_if<t::Failure>(&deadline)) {
        return failure_status(*failure, context.request_id(), context.trace_id());
      }
      auto value = dependencies_.application.operation(std::get<t::AuthenticationContext>(peer),
                                                       std::get<t::TimePoint>(deadline),
                                                       t::OperationId{identifier});
      if (const auto* failure = std::get_if<t::Failure>(&value)) {
        return failure_status(*failure, context.request_id(), context.trace_id());
      }
      if (!operation(std::get<t::HostMaintenanceOperation>(value).operation, context,
                     response->mutable_operation())) {
        return failure_status(internal(), context.request_id(), context.trace_id());
      }
      return grpc::Status::OK;
    } catch (...) {
      return grpc::Status{grpc::StatusCode::INTERNAL, "INTERNAL"};
    }
  }

  grpc::Status CancelOperation(grpc::ServerContext* transport,
                               const v1::HostCancelOperationRequest* request,
                               v1::HostGetOperationResponse* response) override {
    try {
      const auto& context = request->context();
      if (!request->has_context() || !valid_context(context, true) || !request->has_operation() ||
          !request->has_expected_version() || request->expected_version() == 0) {
        return failure_status(invalid(), context.request_id(), context.trace_id());
      }
      std::string identifier;
      std::string host_id;
      if (!operation_identity(request->operation(), &identifier, &host_id)) {
        return failure_status(invalid(), context.request_id(), context.trace_id());
      }
      if (host_id != dependencies_.application.host_id().value) {
        // Stored resource identity, not the caller wire label, decides visibility.
        return failure_status(not_found(), context.request_id(), context.trace_id());
      }
      const auto scope = host_scope(host_id);
      if (!scope) {
        return failure_status(invalid(), context.request_id(), context.trace_id());
      }
      auto peer = authenticate(*transport, context, kCancelMethod, *scope);
      if (const auto* failure = std::get_if<t::Failure>(&peer)) {
        return failure_status(*failure, context.request_id(), context.trace_id());
      }
      auto deadline = bounded_deadline(*transport, context, std::chrono::seconds{30});
      if (const auto* failure = std::get_if<t::Failure>(&deadline)) {
        return failure_status(*failure, context.request_id(), context.trace_id());
      }
      t::HostMaintenanceCancelRequest command;
      command.context.authentication = std::get<t::AuthenticationContext>(peer);
      command.context.request_id = context.request_id();
      command.context.trace_id = context.trace_id();
      command.context.idempotency_key = context.idempotency_key();
      command.context.host_id = host_id;
      command.context.expected_version = request->expected_version();
      command.context.deadline = std::get<t::TimePoint>(deadline);
      command.operation = t::OperationId{identifier};
      auto value = dependencies_.application.cancel(command);
      if (const auto* failure = std::get_if<t::Failure>(&value)) {
        return failure_status(*failure, context.request_id(), context.trace_id());
      }
      if (!operation(std::get<t::HostMaintenanceOperation>(value).operation, context,
                     response->mutable_operation())) {
        return failure_status(internal(), context.request_id(), context.trace_id());
      }
      return grpc::Status::OK;
    } catch (...) {
      return grpc::Status{grpc::StatusCode::INTERNAL, "INTERNAL"};
    }
  }

  grpc::Status ListOperationChanges(grpc::ServerContext* transport,
                                    const v1::OperationChangesRequest* request,
                                    v1::OperationChangesResponse* response) override {
    try {
      auto prepared = prepare_operation_observation(*transport, *request, t::Service::Host,
                                                    dependencies_.invocation_deadline);
      if (const auto* failure = std::get_if<t::Failure>(&prepared)) {
        return failure_status(*failure, request->context().request_id(),
                              request->context().trace_id());
      }
      auto& command = std::get<t::OperationObservationCommand>(prepared);
      auto page = dependencies_.application.changes(command.context.authentication,
                                                    command.context.deadline, command.query);
      if (const auto* failure = std::get_if<t::Failure>(&page)) {
        return failure_status(*failure, request->context().request_id(),
                              request->context().trace_id());
      }
      const auto encoded = encode_operation_observation_page(
          std::get<t::OperationObservationPage>(page), request->context(), *response);
      if (const auto* failure = std::get_if<t::Failure>(&encoded)) {
        return failure_status(*failure, request->context().request_id(),
                              request->context().trace_id());
      }
      return grpc::Status::OK;
    } catch (...) {
      return grpc::Status{grpc::StatusCode::INTERNAL, "INTERNAL"};
    }
  }

private:
  bool valid_context(const v1::RequestContext& context, bool idempotent) const {
    if (!context.has_deadline() || !canonical_uuid(context.request_id()) ||
        !canonical_uuid(context.trace_id())) {
      return false;
    }
    if (idempotent &&
        (context.idempotency_key().size() < 16 || context.idempotency_key().size() > 128)) {
      return false;
    }
    return true;
  }
  t::Outcome<t::AuthenticationContext> authenticate(grpc::ServerContext& transport,
                                                    const v1::RequestContext& context,
                                                    std::string_view method,
                                                    const v1::ResourceRef& scope) {
    if (!dependencies_.authorize) {
      return unauthenticated();
    }
    auto peer = dependencies_.authorize(transport, context, method, scope);
    if (const auto* failure = std::get_if<t::Failure>(&peer)) {
      return *failure;
    }
    return std::get<HostManagementPeer>(peer).authentication;
  }
  t::Outcome<t::TimePoint> bounded_deadline(grpc::ServerContext& transport,
                                            const v1::RequestContext& context,
                                            std::chrono::milliseconds maximum) {
    const auto decoded = codec::from_wire(context.deadline());
    if (!std::holds_alternative<t::TimePoint>(decoded)) {
      return invalid();
    }
    auto deadline = std::min(std::get<t::TimePoint>(decoded), now() + maximum);
    if (transport.deadline() != std::chrono::system_clock::time_point::max()) {
      deadline =
          std::min(deadline, t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 transport.deadline().time_since_epoch())});
    }
    if (dependencies_.invocation_deadline) {
      auto own = dependencies_.invocation_deadline();
      if (const auto* failure = std::get_if<t::Failure>(&own)) {
        return *failure;
      }
      deadline = std::min(deadline, std::get<t::TimePoint>(own));
    }
    if (transport.IsCancelled() || deadline <= now()) {
      return timeout();
    }
    return deadline;
  }
  grpc::Status maintain(grpc::ServerContext* transport, bool has_context,
                        const v1::RequestContext& context, const std::string& host_id,
                        std::uint64_t expected_version, std::uint32_t grace_ms, bool begin,
                        v1::HostMaintenanceResponse* response, std::string_view method) {
    try {
      if (!has_context || !valid_context(context, true)) {
        return failure_status(invalid(), context.request_id(), context.trace_id());
      }
      if (host_id.empty() || host_id.size() > 128) {
        return failure_status(invalid(), context.request_id(), context.trace_id());
      }
      if (host_id != dependencies_.application.host_id().value) {
        return failure_status(not_found(), context.request_id(), context.trace_id());
      }
      const auto scope = host_scope(host_id);
      if (!scope || expected_version == 0) {
        return failure_status(invalid(), context.request_id(), context.trace_id());
      }
      auto peer = authenticate(*transport, context, method, *scope);
      if (const auto* failure = std::get_if<t::Failure>(&peer)) {
        return failure_status(*failure, context.request_id(), context.trace_id());
      }
      auto deadline = bounded_deadline(*transport, context, std::chrono::seconds{30});
      if (const auto* failure = std::get_if<t::Failure>(&deadline)) {
        return failure_status(*failure, context.request_id(), context.trace_id());
      }
      const auto authentication = std::get<t::AuthenticationContext>(peer);
      t::HostMaintenanceRequest command{authentication,
                                        context.request_id(),
                                        context.trace_id(),
                                        context.idempotency_key(),
                                        host_id,
                                        expected_version,
                                        grace_ms,
                                        std::get<t::TimePoint>(deadline)};
      auto value = begin ? dependencies_.application.begin(command)
                         : dependencies_.application.resume(command);
      if (const auto* failure = std::get_if<t::Failure>(&value)) {
        return failure_status(*failure, context.request_id(), context.trace_id());
      }
      if (!operation(std::get<t::HostMaintenanceOperation>(value).operation, context,
                     response->mutable_operation())) {
        return failure_status(internal(), context.request_id(), context.trace_id());
      }
      // The caller already passed the exact host.execute check; the mutation response must not
      // demand a second host.read permission from an execute-only delegation.
      auto state = dependencies_.application.state_projection();
      if (const auto* failure = std::get_if<t::Failure>(&state)) {
        return failure_status(*failure, context.request_id(), context.trace_id());
      }
      if (!maintenance_record(std::get<t::HostMaintenanceState>(state),
                              response->mutable_maintenance())) {
        return failure_status(internal(), context.request_id(), context.trace_id());
      }
      return grpc::Status::OK;
    } catch (...) {
      return grpc::Status{grpc::StatusCode::INTERNAL, "INTERNAL"};
    }
  }

  HostManagementRpcDependencies dependencies_;
};

target::Outcome<std::unique_ptr<v1::HostManagementService::Service>>
make_host_management_rpc(HostManagementRpcDependencies dependencies) {
  if (!dependencies.authorize) {
    return target::Failure{target::ErrorCode::InvalidArgument, target::Acceptance::NotAccepted,
                           target::Recovery::CorrectInput, std::nullopt, std::nullopt};
  }
  return std::unique_ptr<v1::HostManagementService::Service>(
      std::make_unique<HostManagementServiceAdapter>(std::move(dependencies)));
}

} // namespace kiarabot::core::platform
