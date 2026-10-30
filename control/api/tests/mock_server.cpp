#include <iostream>
#include <pthread.h>
#include <signal.h>

#include "kiarabot/control/http_server.hpp"
#include "kiarabot/core/application/control_service.hpp"

namespace {
namespace core = kiarabot::core;
namespace wire = kiarabot::v1;
// Deliberately has no store, session service, or production Core implementation.
class ContractMock final : public core::IControlApplication {
public:
  core::PortResult<core::ControlReply> invoke(const core::ControlRequest& request) override {
    if (const auto invalid =
            core::validate_control_request(request, std::chrono::system_clock::now())) {
      return *invalid;
    }
    auto reject = [](wire::ErrorCode code) -> core::PortResult<core::ControlReply> {
      wire::Error result;
      result.set_code(code);
      return result;
    };
    if (const auto* login = std::get_if<core::ControlLogin>(&request.operation)) {
      return core::ControlReply(core::ControlTokenPair{
          std::string(64, login->username == "viewer" ? 'v' : 'a'), std::string(64, 'r'), 900});
    }
    const auto& token = request.metadata.access_token;
    if (token != std::string(64, 'a') && token != std::string(64, 'v')) {
      return reject(wire::UNAUTHENTICATED);
    }
    const auto role = token.front() == 'a' ? "admin" : "viewer";
    if (!core::control_role_allows(role, core::control_policy(request.operation).permission)) {
      return reject(wire::PERMISSION_DENIED);
    }
    if (std::holds_alternative<core::ControlPrincipalQuery>(request.operation)) {
      return core::ControlReply(
          core::ControlPrincipal{role, "mock-session", {role}, core::control_permissions(role)});
    }
    if (const auto* list = std::get_if<core::ControlList>(&request.operation)) {
      if (!list->page_token.empty()) {
        return reject(wire::INVALID_ARGUMENT);
      }
      return core::ControlReply(
          core::ControlPage{list->resource, {}, list->page_size, std::nullopt});
    }
    if (std::holds_alternative<core::ControlHealth>(request.operation)) {
      return core::ControlReply(
          core::ControlHealthResult{wire::HEALTHY, {{"mock", wire::HEALTHY, ""}}});
    }
    if (std::holds_alternative<core::ControlReadPluginConfig>(request.operation)) {
      return core::ControlReply(core::ControlPluginConfig{"0", {}});
    }
    return reject(wire::NOT_FOUND);
  }
};
} // namespace
int main() {
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  ContractMock application;
  kiarabot::control::HttpServer server(application);
  if (!server.start("127.0.0.1:0")) {
    return 1;
  }
  std::cout << "operation=control_start port=" << server.port() << '\n' << std::flush;
  int signal = 0;
  sigwait(&signals, &signal);
  server.stop();
}
