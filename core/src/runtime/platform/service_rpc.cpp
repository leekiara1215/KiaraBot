#include "kiarabot/core/runtime/platform/service_rpc.hpp"

#include <chrono>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "kiarabot/v1/host.grpc.pb.h"
#include "kiarabot/v1/identity.grpc.pb.h"
#include "kiarabot/v1/model.grpc.pb.h"
#include "kiarabot/v1/runtime.grpc.pb.h"
#include "kiarabot/v1/scheduler.grpc.pb.h"
#include "kiarabot/v1/services.grpc.pb.h"

namespace kiarabot::core::platform {

class ServiceRpc::Impl {
public:
  ~Impl() {
    if (server) {
      server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds{2});
      server->Wait();
    }
  }
  std::vector<std::unique_ptr<grpc::Service>> services;
  std::unique_ptr<grpc::Server> server;
  int port = 0;
};
ServiceRpc::ServiceRpc() : impl_(std::make_unique<Impl>()) {}
ServiceRpc::~ServiceRpc() = default;
int ServiceRpc::port() const noexcept {
  return impl_->port;
}
bool ServiceRpc::start(const ProcessConfig& config) {
  if (impl_->server || !impl_->services.empty()) {
    return false;
  }
  // Generated Service defaults explicitly return UNIMPLEMENTED. Register actual owner method
  // tables rather than an unrelated server or a generic responder that accepts every method.
  switch (config.service) {
  case target::Service::Identity:
    impl_->services.push_back(std::make_unique<v1::IdentityService::Service>());
    break;
  case target::Service::Runtime:
    impl_->services.push_back(std::make_unique<v1::RuntimeService::Service>());
    impl_->services.push_back(std::make_unique<v1::AdapterBridge::Service>());
    impl_->services.push_back(std::make_unique<v1::PluginBridge::Service>());
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
  return impl_->server && impl_->port > 0;
}

} // namespace kiarabot::core::platform
