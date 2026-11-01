#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "kiarabot/core/ports/target/service_readiness.hpp"

namespace kiarabot::core::platform {

struct ProcessConfig;
struct ServiceComposition {
  ServiceComposition(std::unique_ptr<target::ServiceReadinessPort> application_value,
                     std::function<target::Status()> dependencies = {})
      : check_dependencies(std::move(dependencies)), application(std::move(application_value)) {}
  // Outer-only maintenance. Own captured resources until RPC/socket shutdown completes.
  // Called before listening and once per second on the process thread; must be bounded.
  // Failure ends this process (1); handle transient busy separately in the composition root.
  std::function<target::Status()> check_dependencies;
  // Destroy the application before releasing dependencies captured by the callback.
  std::unique_ptr<target::ServiceReadinessPort> application;
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
