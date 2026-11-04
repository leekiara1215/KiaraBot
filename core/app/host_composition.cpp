#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/host_maintenance.hpp"
#include "kiarabot/core/application/target/host_management.hpp"
#include "kiarabot/core/application/target/host_service.hpp"
#include "kiarabot/core/runtime/platform/host_bridge_client.hpp"
#include "kiarabot/core/runtime/platform/host_clock.hpp"
#include "kiarabot/core/runtime/platform/host_identity_client.hpp"
#include "kiarabot/core/runtime/platform/host_management_rpc.hpp"
#include "kiarabot/core/runtime/platform/host_process.hpp"
#include "kiarabot/core/runtime/platform/host_runtime_client.hpp"
#include "kiarabot/core/runtime/platform/service_config.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"
#include "kiarabot/core/runtime/platform/service_rpc.hpp"
#include "kiarabot/infrastructure/host/host_journal.hpp"
#include "kiarabot/infrastructure/host/host_maintenance_sqlite.hpp"

namespace {

namespace t = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace v1 = kiarabot::v1;

t::Failure rejection(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
std::string random_epoch_seed() {
  std::random_device device;
  std::mt19937_64 generator{device()};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string seed(32, '0');
  for (std::size_t index = 0; index < seed.size(); index += 16) {
    const auto value = generator();
    for (std::size_t offset = 0; offset < 16; ++offset) {
      seed[index + offset] = kHex[(value >> ((15 - offset) * 4)) & 0x0fU];
    }
  }
  return seed;
}
// Offline/no-Identity deployments keep management RPCs fail-closed instead of fake authorization.
class UnavailableAuthorizer final : public t::HostMaintenanceAuthorizationPort {
public:
  t::Outcome<t::ResourceVersion> check(const t::AuthenticationContext&, t::Permission,
                                       const t::ResourceRef&, t::TimePoint) override {
    return rejection(t::ErrorCode::CapabilityNotSupported);
  }
};
// The background worker and owner RPCs share one HostMaintenanceApplication/mutex: both persist
// bounded local phases, so a single entry point keeps the SQLite transaction order deterministic.
class LockedManagementPort final : public t::HostManagementApplicationPort {
public:
  LockedManagementPort(std::mutex& mutex, t::HostMaintenanceApplication& application)
      : mutex_(mutex), application_(application) {}
  t::Outcome<t::HostMaintenanceState> maintenance_state(const t::AuthenticationContext& context,
                                                        t::TimePoint deadline) override {
    std::lock_guard lock(mutex_);
    return application_.state(context, deadline);
  }
  t::Outcome<t::HostMaintenanceOperation> begin(const t::HostMaintenanceRequest& request) override {
    std::lock_guard lock(mutex_);
    return application_.begin(request);
  }
  t::Outcome<t::HostMaintenanceOperation>
  resume(const t::HostMaintenanceRequest& request) override {
    std::lock_guard lock(mutex_);
    return application_.resume(request);
  }
  t::Outcome<t::HostMaintenanceOperation>
  cancel(const t::HostMaintenanceCancelRequest& request) override {
    std::lock_guard lock(mutex_);
    return application_.cancel(request);
  }
  t::Outcome<t::HostMaintenanceOperation> operation(const t::AuthenticationContext& context,
                                                    t::TimePoint deadline,
                                                    const t::OperationId& operation) override {
    std::lock_guard lock(mutex_);
    return application_.operation(context, deadline, operation);
  }
  t::Outcome<t::OperationObservationPage> changes(const t::AuthenticationContext& context,
                                                  t::TimePoint deadline,
                                                  t::OperationObservationQuery query) override {
    std::lock_guard lock(mutex_);
    return application_.changes(context, deadline, std::move(query));
  }

private:
  std::mutex& mutex_;
  t::HostMaintenanceApplication& application_;
};

struct CompositionState {
  // Declaration order is lifetime order: the readiness application is destroyed after the RPC
  // adapters, and every borrowed dependency outlives its consumer.
  std::unique_ptr<t::HostJournal> journal;
  std::unique_ptr<t::ProcessSupervisor> supervisor;
  std::unique_ptr<t::HostClock> clock;
  std::unique_ptr<platform::HostBridgeClient> bridge;
  std::unique_ptr<platform::HostRuntimeClient> runtime_client;
  std::unique_ptr<platform::HostIdentityClient> identity_client;
  std::unique_ptr<UnavailableAuthorizer> unavailable_authorizer;
  std::unique_ptr<platform::HostMaintenanceIdentityAuthorizer> identity_authorizer;
  std::unique_ptr<t::HostMaintenanceStore> maintenance_store;
  std::unique_ptr<t::HostMaintenanceApplication> maintenance;
  std::unique_ptr<LockedManagementPort> management_port;
  std::mutex maintenance_mutex;
  std::unique_ptr<t::HostService> service;
};

} // namespace

int run_host_service(int argc, char* argv[]) {
  return platform::run_service(
      t::Service::Host,
      [](const platform::ProcessConfig& config) -> t::Outcome<platform::ServiceComposition> {
        if (!config.host) {
          return rejection(t::ErrorCode::InvalidArgument);
        }
        const auto& material = *config.host;
        auto state = std::make_shared<CompositionState>();
        state->journal =
            kiarabot::infrastructure::host::make_sqlite_host_journal(material.sqlite_path);
        state->supervisor = t::make_posix_process_supervisor();
        state->clock = t::make_system_host_clock();
        grpc::SslCredentialsOptions tls;
        tls.pem_root_certs = config.tls.ca;
        tls.pem_cert_chain = config.tls.certificate;
        tls.pem_private_key = config.tls.private_key;
        platform::HostBridgeClientConfig bridge_config;
        bridge_config.endpoint = material.runtime_endpoint;
        bridge_config.heartbeat_interval = std::chrono::seconds{5};
        t::HostIdentity identity;
        identity.host_id = material.host_id;
        for (const auto& package : material.packages) {
          identity.packages.push_back({package.plugin_type_id, package.root, package.sha256,
                                       package.launch_profile, package.allowed_argument_names});
        }
        bridge_config.identity = std::move(identity);
        bridge_config.credentials = grpc::SslCredentials(tls);
        state->bridge = std::make_unique<platform::HostBridgeClient>(std::move(bridge_config));
        std::vector<t::LaunchProfile> profiles;
        for (const auto& profile : material.profiles) {
          profiles.push_back({profile.name, profile.executable, profile.fixed_arguments,
                              profile.allowed_environment_names, profile.credential_environment});
        }
        auto credentials = std::shared_ptr<grpc::ChannelCredentials>(grpc::SslCredentials(tls));
        platform::HostRuntimeClientOptions runtime_options;
        runtime_options.endpoint = material.runtime_endpoint;
        runtime_options.channel_credentials = credentials;
        runtime_options.budget = std::chrono::seconds{2};
        runtime_options.maximum_budget = std::chrono::seconds{5};
        platform::HostBridgeClient* bridge = state->bridge.get();
        runtime_options.fence = [bridge]() -> t::Outcome<t::HostFence> {
          return bridge->current_fence();
        };
        auto runtime_client = platform::HostRuntimeClient::create(std::move(runtime_options));
        if (const auto* failed = std::get_if<t::Failure>(&runtime_client)) {
          return *failed;
        }
        state->runtime_client =
            std::get<std::unique_ptr<platform::HostRuntimeClient>>(std::move(runtime_client));
        if (config.identity_endpoint) {
          platform::HostIdentityClientOptions identity_options;
          identity_options.endpoint = *config.identity_endpoint;
          identity_options.channel_credentials = credentials;
          identity_options.budget = std::chrono::milliseconds{1500};
          identity_options.maximum_budget = std::chrono::milliseconds{3000};
          auto identity_client = platform::HostIdentityClient::create(std::move(identity_options));
          if (const auto* failed = std::get_if<t::Failure>(&identity_client)) {
            return *failed;
          }
          state->identity_client =
              std::get<std::unique_ptr<platform::HostIdentityClient>>(std::move(identity_client));
          state->identity_authorizer =
              std::make_unique<platform::HostMaintenanceIdentityAuthorizer>(
                  *state->identity_client);
        } else {
          state->unavailable_authorizer = std::make_unique<UnavailableAuthorizer>();
        }
        t::HostIdentity host_identity;
        host_identity.host_id = material.host_id;
        for (const auto& package : material.packages) {
          host_identity.packages.push_back({package.plugin_type_id, package.root, package.sha256,
                                            package.launch_profile,
                                            package.allowed_argument_names});
        }
        t::HostServiceDependencies dependencies{*state->journal,
                                                *state->supervisor,
                                                *state->bridge,
                                                *state->clock,
                                                std::move(host_identity),
                                                std::move(profiles),
                                                material.environment,
                                                material.credential_directory,
                                                std::chrono::seconds{10},
                                                64,
                                                state};
        state->service = std::make_unique<t::HostService>(std::move(dependencies));
        const auto started = state->service->start();
        if (const auto* failed = std::get_if<t::Failure>(&started)) {
          return *failed;
        }
        auto maintenance_store = kiarabot::infrastructure::host::make_sqlite_host_maintenance(
            material.sqlite_path, t::ResourceId{material.host_id}, random_epoch_seed(),
            *state->clock);
        if (const auto* failed = std::get_if<t::Failure>(&maintenance_store)) {
          return *failed;
        }
        state->maintenance_store =
            std::get<std::unique_ptr<t::HostMaintenanceStore>>(std::move(maintenance_store));
        auto& authorizer =
            state->identity_authorizer
                ? static_cast<t::HostMaintenanceAuthorizationPort&>(*state->identity_authorizer)
                : static_cast<t::HostMaintenanceAuthorizationPort&>(*state->unavailable_authorizer);
        state->maintenance = std::make_unique<t::HostMaintenanceApplication>(
            t::ResourceId{material.host_id}, *state->maintenance_store, *state->service,
            *state->runtime_client, authorizer, *state->clock);
        state->management_port =
            std::make_unique<LockedManagementPort>(state->maintenance_mutex, *state->maintenance);
        auto* host_service = state->service.get();
        auto service = std::unique_ptr<t::ServiceReadinessPort>(std::move(state->service));
        state->service.reset();
        platform::ServiceComposition composition{std::move(service), {}, {}, {}};
        composition.configure_rpc = [state](platform::ServiceRpc& rpc) -> t::Status {
          auto* identity = state->identity_client.get();
          platform::HostManagementRpcDependencies rpc_dependencies{
              *state->management_port,
              [identity](grpc::ServerContext& transport, const v1::RequestContext& request,
                         std::string_view,
                         const v1::ResourceRef&) -> t::Outcome<platform::HostManagementPeer> {
                if (identity == nullptr) {
                  return rejection(t::ErrorCode::CapabilityNotSupported);
                }
                auto authenticated = identity->authenticate(transport, request);
                if (const auto* failed = std::get_if<t::Failure>(&authenticated)) {
                  return *failed;
                }
                return platform::HostManagementPeer{
                    std::get<t::AuthenticationContext>(std::move(authenticated)),
                    t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
                        (std::chrono::system_clock::now() + std::chrono::seconds{30})
                            .time_since_epoch())}};
              },
              {}};
          auto adapter = platform::make_host_management_rpc(std::move(rpc_dependencies));
          if (const auto* failed = std::get_if<t::Failure>(&adapter)) {
            return *failed;
          }
          return rpc.add_service(
              t::Service::Host,
              std::move(std::get<std::unique_ptr<v1::HostManagementService::Service>>(adapter)));
        };
        // Bounded background progress: at most one durable phase (or one bounded process stop)
        // per process-thread tick; owner RPCs and this worker share the maintenance mutex.
        composition.check_dependencies = [state]() -> t::Status {
          try {
            std::lock_guard lock(state->maintenance_mutex);
            const auto current = state->maintenance_store->maintenance_state();
            if (std::holds_alternative<t::Failure>(current)) {
              return t::Status{std::monostate{}};
            }
            const auto& value = std::get<t::HostMaintenanceState>(current);
            if (!value.active_operation) {
              return t::Status{std::monostate{}};
            }
            (void)state->maintenance->advance(value.active_operation->id);
            return t::Status{std::monostate{}};
          } catch (...) {
            return t::Status{std::monostate{}};
          }
        };
        // SIGTERM/SIGINT: finish the bounded Host drain (plugin TERM->KILL, observation flush,
        // SQLite journal close) before the gRPC listeners are released. An unconfirmed process
        // termination stays UNKNOWN; this hook never rewrites it to stopped/succeeded.
        composition.before_stop = [host_service]() -> t::Status {
          host_service->stop();
          return std::monostate{};
        };
        return composition;
      },
      argc, argv);
}
