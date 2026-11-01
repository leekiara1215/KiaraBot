#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "kiarabot/core/ports/target/service_readiness.hpp"

namespace kiarabot::core::platform {

struct ProcessConfig;
class ServiceRpc;
// Outer listener with exclusive ownership. Destructor must cancel and join within a bound.
class ServiceEndpoint {
public:
  virtual ~ServiceEndpoint() = default;
  virtual target::Status start() = 0;
  virtual target::Status check() const = 0;
};
struct ServiceComposition {
  ServiceComposition(std::unique_ptr<target::ServiceReadinessPort> application_value,
                     std::function<target::Status()> dependencies = {},
                     std::function<target::Status(ServiceRpc&)> rpc_configuration = {},
                     std::unique_ptr<ServiceEndpoint> endpoint_value = {})
      : check_dependencies(std::move(dependencies)), configure_rpc(std::move(rpc_configuration)),
        application(std::move(application_value)), endpoint(std::move(endpoint_value)) {}
  // Outer-only maintenance. Own captured resources until RPC/socket shutdown completes.
  // Called before listening and once per second on the process thread; must be bounded.
  // Failure ends this process (1); handle transient busy separately in the composition root.
  std::function<target::Status()> check_dependencies;
  // Outer adapters only; never an RPC type in an application/port. Called once before listening.
  // RPC owns registered adapters and destroys them before application/captured dependencies.
  std::function<target::Status(ServiceRpc&)> configure_rpc;
  // Destroy the application before releasing dependencies captured by the callback.
  std::unique_ptr<target::ServiceReadinessPort> application;
  // Declared last: endpoint stops before the application and captured dependencies die.
  std::unique_ptr<ServiceEndpoint> endpoint;
};
using ServiceComposer = std::function<target::Outcome<ServiceComposition>(const ProcessConfig&)>;

// Validate CLI/TLS before composing; block stop signals before any factory starts workers.
// Help/invalid input never calls the composer. Null/wrong-owner composition fails closed.
int run_service(target::Service service, const ServiceComposer& compose, int argc, char* argv[]);

// Borrows the port until return, including shutdown of all sockets/threads/RPCs.
// --help: 0 without listening. --config PATH required otherwise. CLI/config errors: 2;
// certificate/bind/runtime failure: 1; synchronous SIGINT/SIGTERM shutdown: 0.
// Configuration never overrides a false readiness result. No business state is created.
int run_service(const target::ServiceReadinessPort& application, int argc, char* argv[]);

} // namespace kiarabot::core::platform
