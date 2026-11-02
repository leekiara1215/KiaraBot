#include "kiarabot/core/runtime/platform/service_rpc.hpp"

#include <chrono>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "kiarabot/v1/host.grpc.pb.h"
#include "kiarabot/v1/identity.grpc.pb.h"
#include "kiarabot/v1/model.grpc.pb.h"
#include "kiarabot/v1/runtime.grpc.pb.h"
#include "kiarabot/v1/scheduler.grpc.pb.h"
#include "kiarabot/v1/services.grpc.pb.h"

namespace kiarabot::core::platform {

namespace {
// Bounded shutdown of one gRPC server before the next listener family is torn down.
void stop_server(std::unique_ptr<grpc::Server>& server) {
  if (server) {
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds{2});
    server->Wait();
    server.reset();
  }
}
} // namespace

class ServiceRpc::Impl {
public:
  explicit Impl(target::Service value) : owner(value) {}
  ~Impl() {
    // The external Bridge listener stops before the owner mTLS listener; registered service
    // objects are destroyed only after both servers have drained.
    stop_server(bridge_server);
    stop_server(server);
  }
  std::vector<std::unique_ptr<grpc::Service>> services;
  std::vector<std::unique_ptr<grpc::Service>> bridge_services;
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<grpc::Server> bridge_server;
  target::Service owner;
  bool start_attempted = false;
  int port = 0;
  int bridge_port = 0;
};
ServiceRpc::ServiceRpc(target::Service owner) : impl_(std::make_unique<Impl>(owner)) {}
ServiceRpc::~ServiceRpc() = default;
int ServiceRpc::port() const noexcept {
  return impl_->port;
}
int ServiceRpc::bridge_port() const noexcept {
  return impl_->bridge_port;
}
target::Status ServiceRpc::add_service(target::Service owner,
                                       std::unique_ptr<grpc::Service> service) {
  if (impl_->start_attempted || !service || owner != impl_->owner ||
      owner == target::Service::Control) {
    return target::Failure{target::ErrorCode::InvalidArgument,
                           target::Acceptance::NotAccepted,
                           target::Recovery::CorrectInput,
                           {},
                           {}};
  }
  impl_->services.push_back(std::move(service));
  return std::monostate{};
}
target::Status ServiceRpc::add_bridge_service(target::Service owner,
                                              std::unique_ptr<grpc::Service> service) {
  if (impl_->start_attempted || !service || owner != impl_->owner ||
      owner != target::Service::Runtime) {
    return target::Failure{target::ErrorCode::InvalidArgument,
                           target::Acceptance::NotAccepted,
                           target::Recovery::CorrectInput,
                           {},
                           {}};
  }
  impl_->bridge_services.push_back(std::move(service));
  return std::monostate{};
}
bool ServiceRpc::start(const ProcessConfig& config) {
  if (impl_->start_attempted || config.service != impl_->owner) {
    return false;
  }
  impl_->start_attempted = true;
  // Generated Service defaults explicitly return UNIMPLEMENTED. Register actual owner method
  // tables rather than an unrelated server or a generic responder that accepts every method.
  // External Adapter/Plugin bridges are never registered on the owner mTLS listener.
  if (impl_->services.empty()) {
    switch (config.service) {
    case target::Service::Identity:
      impl_->services.push_back(std::make_unique<v1::IdentityService::Service>());
      break;
    case target::Service::Runtime:
      impl_->services.push_back(std::make_unique<v1::RuntimeService::Service>());
      impl_->services.push_back(std::make_unique<v1::HostBridge::Service>());
      break;
    case target::Service::Host:
      impl_->services.push_back(std::make_unique<v1::HostService::Service>());
      break;
    case target::Service::Scheduler:
      impl_->services.push_back(std::make_unique<v1::SchedulerService::Service>());
      break;
    case target::Service::Model:
      impl_->services.push_back(std::make_unique<v1::ModelService::Service>());
      break;
    case target::Service::Control:
      return false;
    }
  }
  if (impl_->bridge_services.empty() && config.bridge_listen) {
    // Default denying stubs keep the external listener buildable before the P06 composition wire;
    // production composition registers the real AdapterBridge/PluginBridge implementations.
    impl_->bridge_services.push_back(std::make_unique<v1::AdapterBridge::Service>());
    impl_->bridge_services.push_back(std::make_unique<v1::PluginBridge::Service>());
  }
  // A bridge listener and bridge services must be configured together: never expose the external
  // services on the owner mTLS socket and never claim an omitted bridge listener during startup.
  if (impl_->bridge_services.empty() != !config.bridge_listen.has_value()) {
    return false;
  }
  grpc::SslServerCredentialsOptions options;
  options.pem_root_certs = config.tls.ca;
  options.pem_key_cert_pairs.push_back({config.tls.private_key, config.tls.certificate});
  options.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  grpc::ServerBuilder builder;
  builder.AddChannelArgument("grpc.so_reuseport", 0);
  builder.SetMaxReceiveMessageSize(1024 * 1024);
  for (const auto& service : impl_->services) {
    builder.RegisterService(service.get());
  }
  builder.AddListeningPort(config.grpc_listen, grpc::SslServerCredentials(options), &impl_->port);
  impl_->server = builder.BuildAndStart();
  if (!impl_->server || impl_->port <= 0) {
    stop_server(impl_->server);
    return false;
  }
  if (config.bridge_listen) {
    // External clients are server-verified TLS only. No client certificate is requested or
    // verified here; their authority is the bootstrap-issued ConnectionAuthorization proof.
    grpc::SslServerCredentialsOptions bridge_options;
    bridge_options.pem_root_certs = config.tls.ca;
    bridge_options.pem_key_cert_pairs.push_back({config.tls.private_key, config.tls.certificate});
    bridge_options.client_certificate_request = GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE;
    grpc::ServerBuilder bridge_builder;
    bridge_builder.AddChannelArgument("grpc.so_reuseport", 0);
    bridge_builder.SetMaxReceiveMessageSize(1024 * 1024);
    for (const auto& service : impl_->bridge_services) {
      bridge_builder.RegisterService(service.get());
    }
    bridge_builder.AddListeningPort(
        *config.bridge_listen, grpc::SslServerCredentials(bridge_options), &impl_->bridge_port);
    impl_->bridge_server = bridge_builder.BuildAndStart();
    if (!impl_->bridge_server || impl_->bridge_port <= 0) {
      stop_server(impl_->bridge_server);
      stop_server(impl_->server);
      return false;
    }
  }
  return true;
}

} // namespace kiarabot::core::platform
