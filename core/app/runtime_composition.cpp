#include "runtime_composition.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <openssl/rand.h>

#include "kiarabot/core/application/target/runtime_action_application.hpp"
#include "kiarabot/core/application/target/runtime_application.hpp"
#include "kiarabot/core/application/target/runtime_audit_application.hpp"
#include "kiarabot/core/application/target/runtime_bridge_application.hpp"
#include "kiarabot/core/application/target/runtime_lifecycle_participants.hpp"
#include "kiarabot/core/application/target/runtime_readiness.hpp"
#include "kiarabot/core/application/target/runtime_resource_application.hpp"
#include "kiarabot/core/application/target/runtime_storage_application.hpp"
#include "kiarabot/core/application/target/runtime_task_application.hpp"
#include "kiarabot/core/runtime/platform/host_bridge_server.hpp"
#include "kiarabot/core/runtime/platform/instance_delegation_provider.hpp"
#include "kiarabot/core/runtime/platform/peer_certificate.hpp"
#include "kiarabot/core/runtime/platform/runtime_bridge.hpp"
#include "kiarabot/core/runtime/platform/runtime_identity_client.hpp"
#include "kiarabot/core/runtime/platform/runtime_model_client.hpp"
#include "kiarabot/core/runtime/platform/runtime_rpc.hpp"
#include "kiarabot/core/runtime/platform/scheduler_task_client.hpp"
#include "kiarabot/core/runtime/platform/sealed_instance_delegation_source.hpp"
#include "kiarabot/core/runtime/platform/service_rpc.hpp"
#include "kiarabot/infrastructure/identity_crypto/sha256.hpp"
#include "kiarabot/infrastructure/postgres/factories.hpp"
#include "postgres_service.hpp"

namespace kiarabot::core::composition {
namespace {
namespace target = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace postgres = kiarabot::infrastructure::postgres;

target::Failure invalid_composition() {
  return {target::ErrorCode::InvalidArgument,
          target::Acceptance::NotAccepted,
          target::Recovery::CorrectInput,
          {},
          {}};
}
target::Failure unavailable() {
  return {target::ErrorCode::UpstreamUnavailable,
          target::Acceptance::NotAccepted,
          target::Recovery::Backoff,
          {},
          {}};
}
// CSPRNG UUIDv4 ids for owner commands and opaque page tokens. No fallback id is generated on
// entropy failure; the caller receives UpstreamUnavailable instead.
class RandomIds final : public target::RuntimeIdPort {
public:
  target::Outcome<target::ResourceId> account_id() override {
    return typed<target::ResourceId>();
  }
  target::Outcome<target::InstallationId> installation_id() override {
    return typed<target::InstallationId>();
  }
  target::Outcome<target::InstanceId> instance_id() override {
    return typed<target::InstanceId>();
  }
  target::Outcome<target::InstanceId> adapter_instance_id() override {
    return typed<target::InstanceId>();
  }
  target::Outcome<target::ConnectionId> connection_id() override {
    return typed<target::ConnectionId>();
  }
  target::Outcome<target::OperationId> operation_id() override {
    return typed<target::OperationId>();
  }
  target::Outcome<target::RuntimeEpoch> runtime_epoch() override {
    return typed<target::RuntimeEpoch>();
  }
  target::Outcome<std::string> snapshot_id() override {
    auto value = uuid();
    if (const auto* failure = std::get_if<target::Failure>(&value)) {
      return *failure;
    }
    return std::get<std::string>(value);
  }

private:
  template <typename Identifier> target::Outcome<Identifier> typed() {
    auto value = uuid();
    if (const auto* failure = std::get_if<target::Failure>(&value)) {
      return *failure;
    }
    return Identifier{std::get<std::string>(value)};
  }
  target::Outcome<std::string> uuid() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
      return unavailable();
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      if (index == 4 || index == 6 || index == 8 || index == 10) {
        result += '-';
      }
      result += kDigits[bytes[index] >> 4U];
      result += kDigits[bytes[index] & 0x0fU];
    }
    return result;
  }
};
// Composition-owned CSPRNG bootstrap credential and one-way digest. Plaintext is returned once and
// never persisted; only the SHA-256 digest reaches the Runtime database.
class RandomSecrets final : public target::RuntimeSecretPort {
public:
  target::Outcome<std::string> bootstrap_credential() override {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
      return unavailable();
    }
    constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : bytes) {
      result += kDigits[byte >> 4U];
      result += kDigits[byte & 0x0fU];
    }
    return result;
  }
  target::Outcome<std::array<std::byte, 32>> opaque_secret() override {
    std::array<std::byte, 32> bytes{};
    if (RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()),
                   static_cast<int>(bytes.size())) != 1) {
      return unavailable();
    }
    return bytes;
  }
  target::Outcome<target::Sha256Digest> sha256(std::string_view value) override {
    return digests_.sha256(value);
  }

private:
  infrastructure::identity_crypto::Sha256 digests_;
};
// Fail-closed until the real Identity current-authorization/delegation client is wired. It never
// allows a request, and its availability observation is false so readiness stays false.
class FailClosedAuthorization final : public target::RuntimeAuthorizationPort {
public:
  bool available() const noexcept override {
    return false;
  }
  target::Outcome<target::RuntimeAuthorizationDecision>
  check(const target::RuntimeAuthorizationRequest&) override {
    return target::Failure{target::ErrorCode::UpstreamUnavailable,
                           target::Acceptance::NotAccepted,
                           target::Recovery::Backoff,
                           {},
                           {}};
  }
};
// Fail-closed peer authorizer until the real mTLS peer + Identity delegation verification lands.
platform::RuntimePeerAuthorizer fail_closed_authorizer() {
  return [](grpc::ServerContext&,
            const v1::RequestContext&) -> target::Outcome<target::AuthenticationContext> {
    return target::Failure{target::ErrorCode::Unauthenticated,
                           target::Acceptance::NotAccepted,
                           target::Recovery::Authenticate,
                           {},
                           {}};
  };
}
#ifdef KIARABOT_P13G_TEST_COMPOSITION
// Test-only P13g composition: the production Scheduler participant and Identity authorizer are
// intentionally not wired (P13f option C). This process is built as a separate binary and must
// never be used as production evidence; the fake scheduler mode is supplied by the test harness.
target::TimePoint p13g_now() {
  return target::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
class P13gAuthorization final : public target::RuntimeAuthorizationPort {
public:
  bool available() const noexcept override {
    return true;
  }
  target::Outcome<target::RuntimeAuthorizationDecision>
  check(const target::RuntimeAuthorizationRequest& request) override {
    target::RuntimeAuthorizationDecision decision;
    decision.allowed = true;
    decision.subject = request.authentication.subject;
    decision.permission = request.permission;
    decision.scope = request.scope;
    decision.authorization_version = {1};
    decision.checked_at = p13g_now();
    return decision;
  }
};
class P13gSchedulerTaskPort final : public target::SchedulerTaskPort {
public:
  P13gSchedulerTaskPort() {
    if (const char* value = std::getenv("KIARABOT_P13G_SCHEDULER_MODE")) {
      mode_ = value;
    }
    if (const char* value = std::getenv("KIARABOT_P13G_SCHEDULER_STATE")) {
      state_path_ = value;
    }
    if (const char* value = std::getenv("KIARABOT_P13G_SCHEDULER_MODE_FILE")) {
      mode_path_ = value;
    }
  }
  bool available() const override {
    return true;
  }
  target::Outcome<target::ScheduledTask> create_task(const target::SchedulerTaskContext&,
                                                     const target::TaskCreationInput&) override {
    return failure();
  }
  target::Outcome<target::ScheduledTask> get_task(const target::SchedulerTaskContext&,
                                                  std::string_view) override {
    return failure();
  }
  target::Outcome<std::vector<target::ScheduledTask>>
  list_tasks(const target::SchedulerTaskContext& context, std::optional<target::TaskState>,
             std::size_t) override {
    record("list " + context.owner.value);
    if (!check_mode()) {
      return failure();
    }
    // The test-only port models exactly one pending task owned by the target instance; the real
    // client/authorization matrix is covered by P13h-1/P13h-3.
    target::ScheduledTask task;
    task.key = {context.owner, "p13g-task"};
    task.state = target::TaskState::Pending;
    task.run_at = now() + std::chrono::seconds{60};
    return std::vector<target::ScheduledTask>{task};
  }
  target::Outcome<target::ScheduledTask> cancel_task(const target::SchedulerTaskContext& context,
                                                     std::string_view task_id) override {
    record("cancel " + context.owner.value + " " + std::string{task_id});
    if (!check_mode()) {
      return failure();
    }
    target::ScheduledTask task;
    task.key = {context.owner, std::string{task_id}};
    task.state = target::TaskState::Cancelled;
    task.run_at = now() + std::chrono::seconds{60};
    return task;
  }

private:
  static target::TimePoint now() {
    return target::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())};
  }
  static target::Failure failure() {
    return {target::ErrorCode::CapabilityNotSupported, target::Acceptance::NotAccepted,
            target::Recovery::Diagnose, std::nullopt, std::nullopt};
  }
  bool check_mode() {
    if (!mode_path_.empty()) {
      std::ifstream mode_input(mode_path_);
      std::string current;
      if (std::getline(mode_input, current) && !current.empty()) {
        mode_ = current;
      }
    }
    return mode_ == "done";
  }
  void record(std::string_view line) {
    if (state_path_.empty()) {
      return;
    }
    std::ofstream output(state_path_, std::ios::app);
    output << line << '\n';
  }
  std::string mode_ = "done";
  std::string state_path_;
  std::string mode_path_;
};

class P13gSchedulerLifecycle final : public target::SchedulerLifecycleParticipantPort {
public:
  P13gSchedulerLifecycle(target::SchedulerTaskPort& tasks, target::RuntimeIdPort& ids)
      : participant_(target::make_runtime_scheduler_lifecycle_participant(tasks, ids)) {}
  target::LifecycleParticipantResult cancel_tasks(const target::ResourceRef& resource) override {
    if (const auto* failure = std::get_if<target::Failure>(&participant_)) {
      return {target::LifecycleParticipantDisposition::Blocked, failure->code, std::nullopt};
    }
    return std::get<std::unique_ptr<target::SchedulerLifecycleParticipantPort>>(participant_)
        ->cancel_tasks(resource);
  }

private:
  target::Outcome<std::unique_ptr<target::SchedulerLifecycleParticipantPort>> participant_;
};
#endif

struct State {
  std::unique_ptr<target::RuntimeIdPort> ids;
  std::unique_ptr<target::RuntimeSecretPort> secrets;
  std::unique_ptr<platform::RuntimeIdentityClient> identity_client;
  std::unique_ptr<target::RuntimeAuthorizationPort> fail_closed_authorization;
  target::RuntimeAuthorizationPort* authorization = nullptr;
  target::RuntimeResourceApplication* application = nullptr;
  std::unique_ptr<target::RuntimeStorageApplication> storage;
  std::unique_ptr<target::RuntimeActionApplication> actions;
  std::unique_ptr<target::RuntimeAuditApplication> audit;
  std::unique_ptr<target::OperationObservationApplication> observations;
  std::unique_ptr<target::RuntimeBootstrapDeliveryApplication> bootstrap_delivery;
  target::RuntimeActionApplication* action_application = nullptr;
  std::unique_ptr<target::RuntimeBridgeApplication> bridge;
  std::unique_ptr<target::HostLifecycleParticipantPort> host_lifecycle;
  std::unique_ptr<platform::RuntimeModelClient> model_client;
  std::unique_ptr<platform::RuntimeInstanceDelegationProvider> instance_delegation;
  std::unique_ptr<target::SchedulerTaskPort> scheduler_tasks;
  std::unique_ptr<target::SchedulerTaskPort> scheduler_lifecycle_tasks;
  std::unique_ptr<target::SchedulerLifecycleParticipantPort> scheduler_lifecycle;
  std::unique_ptr<target::RuntimeTaskApplication> task_application;
#ifdef KIARABOT_P13G_TEST_COMPOSITION
  std::unique_ptr<P13gAuthorization> p13g_authorization;
  std::unique_ptr<P13gSchedulerTaskPort> p13g_scheduler_tasks;
  std::unique_ptr<P13gSchedulerLifecycle> p13g_scheduler;
#endif
};
// Readiness merges the composition-observed database state with the authorization-chain observation
// and the assembled business implementation. Unobserved inputs remain Unverified and never ready.
class RuntimeReadiness final : public target::ServiceReadinessPort {
public:
  RuntimeReadiness(std::unique_ptr<target::ServiceReadinessPort> database_observed,
                   std::shared_ptr<State> state)
      : database_observed_(std::move(database_observed)), state_(std::move(state)) {}
  target::Service service() const noexcept override {
    return target::Service::Runtime;
  }
  target::ServiceReadiness readiness() const override {
    target::RuntimeReadinessObservation observation;
    if (database_observed_) {
      const auto base = database_observed_->readiness();
      for (const auto& dependency : base.dependencies) {
        if (dependency.dependency == target::Dependency::BusinessImplementation) {
          observation.business_assembled = dependency.state;
        }
        if (dependency.dependency == target::Dependency::DatabaseSchema) {
          observation.database_schema = dependency.state;
        }
      }
    }
    if (state_ && state_->authorization) {
      observation.identity_authorization = state_->authorization->available()
                                               ? target::DependencyState::Available
                                               : target::DependencyState::Unavailable;
    }
    return target::evaluate_runtime_readiness(observation);
  }

private:
  std::unique_ptr<target::ServiceReadinessPort> database_observed_;
  std::shared_ptr<State> state_;
};
} // namespace

target::Outcome<platform::ServiceComposition>
compose_runtime_service(const platform::ProcessConfig& config) {
  try {
    if (config.service != target::Service::Runtime) {
      return invalid_composition();
    }
    if (!config.database) {
      // P02 lifecycle/smoke fixtures start every process without a database. Keep the independent
      // Runtime process startable with an explicit non-ready application and no business RPC, as
      // before P05; the typed PG composition is selected only when a database is configured.
      return compose_database_service<target::RuntimeUnitOfWork>(
          config, postgres::open_runtime, [](auto&) { return target::make_runtime_application(); });
    }
    auto state = std::make_shared<State>();
    auto composition = compose_database_service<target::RuntimeUnitOfWork>(
        config, postgres::open_runtime,
        [state, &config](auto& factory) -> std::unique_ptr<target::ServiceReadinessPort> {
          state->ids = std::make_unique<RandomIds>();
          state->secrets = std::make_unique<RandomSecrets>();
          if (config.identity_endpoint) {
            grpc::SslCredentialsOptions ssl;
            ssl.pem_root_certs = config.tls.ca;
            ssl.pem_cert_chain = config.tls.certificate;
            ssl.pem_private_key = config.tls.private_key;
            auto client = platform::RuntimeIdentityClient::create(
                {*config.identity_endpoint, grpc::SslCredentials(ssl),
                 std::chrono::milliseconds{500}, std::chrono::milliseconds{1000}});
            if (std::holds_alternative<target::Failure>(client)) {
              return {};
            }
            state->identity_client =
                std::move(std::get<std::unique_ptr<platform::RuntimeIdentityClient>>(client));
          } else {
            state->fail_closed_authorization = std::make_unique<FailClosedAuthorization>();
          }
          state->authorization =
              state->identity_client
                  ? static_cast<target::RuntimeAuthorizationPort*>(state->identity_client.get())
                  : state->fail_closed_authorization.get();
#ifdef KIARABOT_P13G_TEST_COMPOSITION
          state->p13g_authorization = std::make_unique<P13gAuthorization>();
          state->authorization = state->p13g_authorization.get();
          state->p13g_scheduler_tasks = std::make_unique<P13gSchedulerTaskPort>();
          state->p13g_scheduler =
              std::make_unique<P13gSchedulerLifecycle>(*state->p13g_scheduler_tasks, *state->ids);
#endif
          auto host_lifecycle =
              target::make_runtime_host_lifecycle_participant(factory, *state->secrets);
          if (std::holds_alternative<target::Failure>(host_lifecycle)) {
            return {};
          }
          state->host_lifecycle = std::move(
              std::get<std::unique_ptr<target::HostLifecycleParticipantPort>>(host_lifecycle));
          // Shared B1 sealed source/provider for task and model envelopes. Runtime only validates
          // and exchanges the material; no token is invented, extended or written back. Model-only
          // deployments configure just model_instance_delegation_sealed_file.
          if (config.instance_delegation_sealed_file ||
              config.model_instance_delegation_sealed_file) {
            if (!state->identity_client) {
              return {};
            }
            const auto& task_path = config.instance_delegation_sealed_file;
            const auto& model_path = config.model_instance_delegation_sealed_file;
            const std::string primary_path = task_path ? *task_path : *model_path;
            auto source = platform::SealedInstanceDelegationSource::create(
                {primary_path, model_path.value_or("")});
            if (std::holds_alternative<target::Failure>(source)) {
              return {};
            }
            std::shared_ptr<platform::InstanceParentDelegationSource> parent_source(std::move(
                std::get<std::unique_ptr<platform::SealedInstanceDelegationSource>>(source)));
            auto provider = platform::RuntimeInstanceDelegationProvider::create(
                {std::move(parent_source), config.tls.certificate, state->identity_client.get(),
                 std::chrono::milliseconds{5000}});
            if (std::holds_alternative<target::Failure>(provider)) {
              return {};
            }
            state->instance_delegation = std::move(
                std::get<std::unique_ptr<platform::RuntimeInstanceDelegationProvider>>(provider));
          }
          if (config.scheduler_endpoint) {
            grpc::SslCredentialsOptions ssl;
            ssl.pem_root_certs = config.tls.ca;
            ssl.pem_cert_chain = config.tls.certificate;
            ssl.pem_private_key = config.tls.private_key;
            platform::SchedulerTaskClientOptions::DelegationProvider task_delegation =
                platform::unavailable_scheduler_task_delegation();
            if (state->instance_delegation) {
              auto* instance_delegation = state->instance_delegation.get();
              task_delegation = [instance_delegation](const target::SchedulerTaskContext& context,
                                                      target::Permission permission) {
                return instance_delegation->scheduler_task_delegation(context, permission);
              };
            }
            auto client = platform::SchedulerTaskClient::create(
                {*config.scheduler_endpoint, grpc::SslCredentials(ssl), task_delegation,
                 std::chrono::milliseconds{5000}});
            if (std::holds_alternative<target::Failure>(client)) {
              return {};
            }
            state->scheduler_tasks =
                std::move(std::get<std::unique_ptr<platform::SchedulerTaskClient>>(client));
            // P13h-2: separate platform-identity client for the lifecycle participant. It sends
            // no user delegation and uses only ListTasks/CancelTask with the explicit instance
            // scope; the plugin task client above keeps its delegated (currently fail-closed)
            // path. Scheduler P13h-1 authorizes exactly these two methods for the Runtime peer.
            auto lifecycle_client = platform::SchedulerTaskClient::create(
                {*config.scheduler_endpoint, grpc::SslCredentials(ssl), nullptr,
                 std::chrono::milliseconds{2000}, true});
            if (std::holds_alternative<target::Failure>(lifecycle_client)) {
              return {};
            }
            state->scheduler_lifecycle_tasks = std::move(
                std::get<std::unique_ptr<platform::SchedulerTaskClient>>(lifecycle_client));
            auto scheduler_lifecycle = target::make_runtime_scheduler_lifecycle_participant(
                *state->scheduler_lifecycle_tasks, *state->ids);
            if (std::holds_alternative<target::Failure>(scheduler_lifecycle)) {
              return {};
            }
            state->scheduler_lifecycle =
                std::move(std::get<std::unique_ptr<target::SchedulerLifecycleParticipantPort>>(
                    scheduler_lifecycle));
          }
          std::map<target::ResourceKind, target::ResourceId> catalogs;
          for (const auto& [family, kind] :
               std::vector<std::pair<std::string, target::ResourceKind>>{
                   {"accounts", target::ResourceKind::Account},
                   {"installations", target::ResourceKind::Installation},
                   {"instances", target::ResourceKind::Instance},
                   {"adapters", target::ResourceKind::Adapter},
                   {"hosts", target::ResourceKind::Host}}) {
            const auto found = config.resource_catalogs.find(family);
            if (found != config.resource_catalogs.end()) {
              catalogs.emplace(kind, target::ResourceId{found->second});
            }
          }
          auto built = target::make_runtime_resource_application(
              {factory,
               *state->ids,
               *state->secrets,
               *state->authorization,
               {"action.send_message.text.v1", "action.react_message.v1",
                "action.delete_message.v1", "action.mute_member.v1", "action.kick_member.v1",
                "action.approve_request.v1"},
               state->host_lifecycle.get(),
#ifdef KIARABOT_P13G_TEST_COMPOSITION
               state->p13g_scheduler.get(),
#else
               state->scheduler_lifecycle.get(),
#endif
               std::move(catalogs)});
          if (std::holds_alternative<target::Failure>(built)) {
            return {};
          }
          auto application =
              std::move(std::get<std::unique_ptr<target::RuntimeResourceApplication>>(built));
          state->application = application.get();
          auto audit = target::make_runtime_audit_application({factory, *state->authorization});
          if (std::holds_alternative<target::Failure>(audit)) {
            return {};
          }
          state->audit =
              std::move(std::get<std::unique_ptr<target::RuntimeAuditApplication>>(audit));
          auto observations = target::make_operation_observation_application(
              {target::Service::Runtime,
               [&factory]() -> target::Outcome<std::unique_ptr<target::UnitOfWork>> {
                 auto begun = factory.begin();
                 if (const auto* failure = std::get_if<target::Failure>(&begun)) {
                   return *failure;
                 }
                 return std::unique_ptr<target::UnitOfWork>(
                     std::move(std::get<std::unique_ptr<target::RuntimeUnitOfWork>>(begun)));
               },
               *state->authorization,
               {}});
          if (std::holds_alternative<target::Failure>(observations)) {
            return {};
          }
          state->observations = std::move(
              std::get<std::unique_ptr<target::OperationObservationApplication>>(observations));
          auto bootstrap =
              target::runtime_resources_detail::make_runtime_bootstrap_delivery_application(
                  {factory, *state->ids, *state->secrets, *state->authorization});
          if (std::holds_alternative<target::Failure>(bootstrap)) {
            return {};
          }
          state->bootstrap_delivery = std::move(
              std::get<std::unique_ptr<target::RuntimeBootstrapDeliveryApplication>>(bootstrap));
          auto action = target::make_runtime_action_application(
              {factory, *state->ids, *state->secrets, state->authorization,
               target::ActionCapacity{}, target::ActionQuotaLimits{}});
          if (std::holds_alternative<target::Failure>(action)) {
            return {};
          }
          state->actions =
              std::move(std::get<std::unique_ptr<target::RuntimeActionApplication>>(action));
          state->action_application = state->actions.get();
          auto storage = target::make_runtime_storage_application(
              {factory, *state->ids, *state->authorization});
          if (std::holds_alternative<target::Failure>(storage)) {
            return {};
          }
          state->storage =
              std::move(std::get<std::unique_ptr<target::RuntimeStorageApplication>>(storage));
          state->task_application = std::make_unique<target::RuntimeTaskApplication>(
              target::RuntimeTaskApplicationDependencies{state->scheduler_tasks.get()});
          if (config.bridge_listen) {
            // Advertised feature catalog. host.model.stream.v1 is only negotiable when this
            // composition actually has a Model endpoint; the Bridge still fails closed when the
            // requested flow is unavailable for a connection, and the chain is not advertised by
            // the SDK before its own end-to-end acceptance.
            std::vector<std::string> bridge_features{"plugin.config.v1", "action.result.v1",
                                                     "host.tasks.v1", "host.model.v1"};
            if (config.model_endpoint) {
              bridge_features.push_back("host.model.stream.v1");
            }
            auto bridge = target::make_runtime_bridge_application(
                {factory, *state->ids, *state->secrets, *state->application,
                 state->action_application, bridge_features});
            if (std::holds_alternative<target::Failure>(bridge)) {
              return {};
            }
            if (config.model_endpoint) {
              grpc::SslCredentialsOptions ssl;
              ssl.pem_root_certs = config.tls.ca;
              ssl.pem_cert_chain = config.tls.certificate;
              ssl.pem_private_key = config.tls.private_key;
              platform::ModelDelegationProvider model_delegation =
                  platform::unavailable_model_delegation();
              if (state->instance_delegation) {
                auto* instance_delegation = state->instance_delegation.get();
                model_delegation = [instance_delegation](
                                       const target::RuntimeModelChatRequest& request) {
                  target::SchedulerTaskContext context;
                  context.owner = request.instance;
                  context.installation = request.installation;
                  context.request_id = request.request_id;
                  context.trace_id = request.request_id;
                  context.idempotency_key = request.logical_call_id;
                  context.deadline =
                      target::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())};
                  return instance_delegation->model_chat_delegation(context, request.model,
                                                                    target::Permission::ModelChat);
                };
              }
              auto model_client = platform::RuntimeModelClient::create(
                  {*config.model_endpoint, grpc::SslCredentials(ssl), std::move(model_delegation),
                   std::chrono::milliseconds{30000}});
              if (std::holds_alternative<target::Failure>(model_client)) {
                return {};
              }
              state->model_client =
                  std::move(std::get<std::unique_ptr<platform::RuntimeModelClient>>(model_client));
            }
            state->bridge =
                std::move(std::get<std::unique_ptr<target::RuntimeBridgeApplication>>(bridge));
          }
          return application;
        });
    if (const auto* failure = std::get_if<target::Failure>(&composition)) {
      return *failure;
    }
    auto result = std::move(std::get<platform::ServiceComposition>(composition));
    if (!state->application) {
      return unavailable();
    }
    // Bridge maintenance runs on the bounded once-per-second dependency hook: expire dispatcher
    // leases, advance retries/dead letters and claim/push due deliveries for connected plugins.
    {
      auto base = std::move(result.check_dependencies);
      result.check_dependencies = [base = std::move(base), state]() mutable -> target::Status {
        if (base) {
          auto checked = base();
          if (std::holds_alternative<target::Failure>(checked)) {
            return checked;
          }
        }
        if (state->bridge) {
          auto maintained = state->bridge->maintenance();
          if (std::holds_alternative<target::Failure>(maintained)) {
            return maintained;
          }
        }
        if (state->action_application) {
          auto swept = state->action_application->maintenance();
          if (std::holds_alternative<target::Failure>(swept)) {
            return std::get<target::Failure>(swept);
          }
        }
        // P13a: bounded lifecycle reconciliation and the ad-hoc 60s retention batch run on the
        // same dependency hook. A per-operation cleanup failure is non-fatal (the hook itself is
        // process-fatal); P13d surfaces it as a counts-only structured maintenance line so the
        // degraded batch and blocked participants are greppable without a new API/permission.
        auto lifecycle = state->application->lifecycle_maintenance();
        if (std::holds_alternative<target::Failure>(lifecycle)) {
          return std::get<target::Failure>(lifecycle);
        }
        // P13d: counts-only structured line on every non-idle/attention maintenance round. A
        // cleanup failure is also returned to readiness by the maintenance call's own failure path;
        // this line makes the degraded batch and blocked participants greppable in owner logs.
        const auto& maintained = std::get<target::LifecycleMaintenance>(lifecycle);
        if (maintained.reconciled != 0 || maintained.completed != 0 || maintained.blocked != 0 ||
            maintained.purged != 0 || maintained.pruned != 0 || maintained.cleanup_failed) {
          std::cerr << target::format_lifecycle_maintenance_log(maintained) << '\n';
        }
        return std::monostate{};
      };
    }
    auto* application = state->application;
    std::vector<target::HostCertificateBinding> host_allowlist;
    host_allowlist.reserve(config.hosts.size());
    for (const auto& host : config.hosts) {
      host_allowlist.push_back(
          target::HostCertificateBinding{host.host_id, host.certificate_sha256});
    }
    auto configure = std::move(result.configure_rpc);
    auto control_peers = config.control_runtime_peers;
    result.configure_rpc = [configure = std::move(configure), state, application,
                            host_allowlist = std::move(host_allowlist),
                            control_peers = std::move(control_peers)](
                               platform::ServiceRpc& rpc) mutable -> target::Status {
      if (configure) {
        auto configured = configure(rpc);
        if (const auto* failure = std::get_if<target::Failure>(&configured)) {
          return *failure;
        }
      }
      platform::RuntimeRpcOptions options;
      options.storage = state->storage.get();
      options.actions = state->action_application;
      options.audit = state->audit.get();
      options.observations = state->observations.get();
      options.bootstrap = state->bootstrap_delivery.get();
      // BOOT-02 Control platform peer: only the deployment-enrolled actual mTLS leaf fingerprint
      // carrying the Control URI SAN may report/query delivery evidence. Empty allowlist leaves
      // `options.control_peer` unset, so the runtime refuses those calls (fail closed).
      if (!control_peers.empty()) {
        const auto& allowed = control_peers;
        options.control_peer =
            [allowed](
                grpc::ServerContext& transport) -> target::Outcome<platform::RuntimePlatformPeer> {
          auto evidence = platform::extract_peer_certificate(*transport.auth_context());
          if (const auto* rejected = std::get_if<target::Failure>(&evidence)) {
            return *rejected;
          }
          const auto& certificate = std::get<platform::PeerCertificateEvidence>(evidence);
          constexpr char kHex[] = "0123456789abcdef";
          std::string fingerprint;
          fingerprint.reserve(64);
          for (const auto byte : certificate.leaf_fingerprint.bytes) {
            const auto value = static_cast<unsigned char>(byte);
            fingerprint.push_back(kHex[value >> 4]);
            fingerprint.push_back(kHex[value & 0x0f]);
          }
          std::string san;
          for (const auto& candidate : certificate.uri_sans) {
            if (candidate.starts_with("spiffe://kiarabot/control")) {
              san = candidate;
              break;
            }
          }
          const bool enrolled = std::find_if(allowed.begin(), allowed.end(), [&](const auto& peer) {
                                  return peer.certificate_sha256 == fingerprint;
                                }) != allowed.end();
          if (san.empty() || !enrolled) {
            return target::Failure{target::ErrorCode::PermissionDenied,
                                   target::Acceptance::NotAccepted, target::Recovery::Authenticate,
                                   std::nullopt, std::nullopt};
          }
          return platform::RuntimePlatformPeer{target::Service::Control, san};
        };
      }
      if (state->identity_client) {
        auto* client = state->identity_client.get();
        options.authorize = [client](grpc::ServerContext& transport,
                                     const v1::RequestContext& request) {
          return client->authenticate(transport, request);
        };
      } else {
        options.authorize = fail_closed_authorizer();
      }
#ifdef KIARABOT_P13G_TEST_COMPOSITION
      options.authorize =
          [](grpc::ServerContext&,
             const v1::RequestContext&) -> target::Outcome<target::AuthenticationContext> {
        const auto now = p13g_now();
        return target::AuthenticationContext{
            target::SubjectId{"77777777-7777-4777-8777-777777777777"},
            {},
            target::Service::Runtime,
            {},
            {},
            target::ResourceVersion{1},
            now,
            now + std::chrono::seconds{300},
            {}};
      };
#endif
      // Platform service principal path for the internal Scheduler->Runtime timer handoff. The
      // leaf SAN is matched against the deployment-enrolled Scheduler platform peer; its explicit
      // timer.accept assignment is an offline bootstrap prerequisite (P05 platform_peers), and the
      // application still validates every handoff integrity field. No delegated user is involved.
      options.platform_peer =
          [](grpc::ServerContext& transport) -> target::Outcome<platform::RuntimePlatformPeer> {
        auto evidence = platform::extract_peer_certificate(*transport.auth_context());
        if (const auto* rejected = std::get_if<target::Failure>(&evidence)) {
          return *rejected;
        }
        const auto& certificate = std::get<platform::PeerCertificateEvidence>(evidence);
        for (const auto& san : certificate.uri_sans) {
          if (san.starts_with("spiffe://kiarabot/scheduler")) {
            return platform::RuntimePlatformPeer{target::Service::Scheduler, san};
          }
        }
        return target::Failure{target::ErrorCode::Unauthenticated, target::Acceptance::NotAccepted,
                               target::Recovery::Authenticate, std::nullopt, std::nullopt};
      };
      auto adapter = platform::make_runtime_rpc(*application, std::move(options));
      if (const auto* failure = std::get_if<target::Failure>(&adapter)) {
        return *failure;
      }
      auto registered = rpc.add_service(
          target::Service::Runtime,
          std::move(std::get<std::unique_ptr<v1::RuntimeService::Service>>(adapter)));
      if (const auto* failure = std::get_if<target::Failure>(&registered)) {
        return *failure;
      }
      // P08 HostBridge is an owner mTLS internal service, never registered on bridge_listen.
      // The deployment certificate allowlist is consumed only here; empty means every Host is
      // rejected (fail closed). Persistence callbacks open short owner UoWs; the transport never
      // commits Runtime transactions itself. The Host lifecycle participant (P13c) is independent.
      platform::HostBridgePersistence host_persistence;
      if (application != nullptr) {
        host_persistence.pending = [application](const std::string& host_id,
                                                 std::uint64_t generation, std::size_t limit) {
          return application->host_pending_commands(host_id, generation, limit);
        };
        host_persistence.dispatched = [application](const std::string& command_id,
                                                    std::uint64_t generation,
                                                    std::string_view runtime_epoch) {
          return application->host_command_dispatched(command_id, generation, runtime_epoch);
        };
        host_persistence.receipt = [application](const std::string& command_id,
                                                 std::uint64_t generation, bool persisted,
                                                 std::optional<target::ExecutionOutcome> outcome,
                                                 std::optional<std::string> marker) {
          return application->host_command_receipt(command_id, generation, persisted, outcome,
                                                   std::move(marker));
        };
        host_persistence.observation =
            [application](const target::HostObservationRecord& observation) {
              auto recorded = application->host_observation(observation);
              if (const auto* failure = std::get_if<target::Failure>(&recorded)) {
                return target::Status{*failure};
              }
              return target::Status{std::monostate{}};
            };
      }
      auto host_bridge = platform::make_deployment_host_bridge(*state->ids, host_allowlist, {},
                                                               std::move(host_persistence));
      if (const auto* failure = std::get_if<target::Failure>(&host_bridge)) {
        return *failure;
      }
      registered = rpc.add_service(
          target::Service::Runtime,
          std::move(std::get<std::unique_ptr<v1::HostBridge::Service>>(host_bridge)));
      if (const auto* failure = std::get_if<target::Failure>(&registered)) {
        return *failure;
      }
      if (!state->bridge) {
        return std::monostate{};
      }
      auto bridge_application = platform::make_adapter_bridge(*state->bridge);
      if (const auto* failure = std::get_if<target::Failure>(&bridge_application)) {
        return *failure;
      }
      registered = rpc.add_bridge_service(
          target::Service::Runtime,
          std::move(std::get<std::unique_ptr<v1::AdapterBridge::Service>>(bridge_application)));
      if (const auto* failure = std::get_if<target::Failure>(&registered)) {
        return *failure;
      }
      auto plugin_bridge = platform::make_plugin_bridge(
          *state->bridge,
          {state->storage.get(), state->model_client.get(), state->task_application.get()});
      if (const auto* failure = std::get_if<target::Failure>(&plugin_bridge)) {
        return *failure;
      }
      return rpc.add_bridge_service(
          target::Service::Runtime,
          std::move(std::get<std::unique_ptr<v1::PluginBridge::Service>>(plugin_bridge)));
    };
    result.before_stop = [state]() -> target::Status {
      if (!state->bridge) {
        return std::monostate{};
      }
      auto sent = state->bridge->shutdown_notice();
      // Bounded drain: the writer threads flush the notice frames while the process releases
      // listeners; a slow peer can never extend shutdown beyond this fixed budget.
      std::this_thread::sleep_for(std::chrono::milliseconds{500});
      return sent;
    };
    result.application = std::make_unique<RuntimeReadiness>(std::move(result.application), state);
    return result;
  } catch (...) {
    return unavailable();
  }
}

} // namespace kiarabot::core::composition
