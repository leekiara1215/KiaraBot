#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/host_service.hpp"
#include "kiarabot/core/runtime/platform/host_bridge_client.hpp"
#include "kiarabot/core/runtime/platform/host_clock.hpp"
#include "kiarabot/core/runtime/platform/host_process.hpp"
#include "kiarabot/core/runtime/platform/service_config.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"
#include "kiarabot/infrastructure/host/host_journal.hpp"

namespace {

namespace t = kiarabot::core::target;
namespace platform = kiarabot::core::platform;

t::Failure rejection(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}

struct CompositionState {
  std::unique_ptr<t::HostJournal> journal;
  std::unique_ptr<t::ProcessSupervisor> supervisor;
  std::unique_ptr<t::HostClock> clock;
  std::unique_ptr<platform::HostBridgeClient> bridge;
  std::unique_ptr<t::HostService> service;
};

} // namespace

int run_host_service(int argc, char* argv[]) {
  return platform::run_service(
      t::Service::Host,
      [](const platform::ProcessConfig& config)
          -> t::Outcome<platform::ServiceComposition> {
        if (!config.host) {
          return rejection(t::ErrorCode::InvalidArgument);
        }
        const auto& material = *config.host;
        auto state = std::make_shared<CompositionState>();
        state->journal = kiarabot::infrastructure::host::make_sqlite_host_journal(
            material.sqlite_path);
        state->supervisor = t::make_posix_process_supervisor();
        state->clock = t::make_system_host_clock();
        platform::HostBridgeClientConfig bridge_config;
        bridge_config.endpoint = material.runtime_endpoint;
        bridge_config.heartbeat_interval = std::chrono::seconds{5};
        t::HostIdentity identity;
        identity.host_id = material.host_id;
        for (const auto& package : material.packages) {
          identity.packages.push_back(
              {package.plugin_type_id, package.root, package.sha256, package.launch_profile,
               package.allowed_argument_names});
        }
        bridge_config.identity = identity;
        grpc::SslCredentialsOptions tls;
        tls.pem_root_certs = config.tls.ca;
        tls.pem_cert_chain = config.tls.certificate;
        tls.pem_private_key = config.tls.private_key;
        bridge_config.credentials = grpc::SslCredentials(tls);
        state->bridge = std::make_unique<platform::HostBridgeClient>(std::move(bridge_config));
        std::vector<t::LaunchProfile> profiles;
        for (const auto& profile : material.profiles) {
          profiles.push_back({profile.name, profile.executable, profile.fixed_arguments,
                              profile.allowed_environment_names,
                              profile.credential_environment});
        }
        t::HostServiceDependencies dependencies{
            *state->journal, *state->supervisor, *state->bridge, *state->clock, identity,
            std::move(profiles), material.environment, material.credential_directory,
            std::chrono::seconds{10}, 64, state};
        state->service = std::make_unique<t::HostService>(std::move(dependencies));
        const auto started = state->service->start();
        if (const auto* failed = std::get_if<t::Failure>(&started)) {
          return *failed;
        }
        auto service = std::unique_ptr<t::ServiceReadinessPort>(std::move(state->service));
        state->service.reset();
        return platform::ServiceComposition{std::move(service), {}, {}, {}};
      },
      argc, argv);
}
