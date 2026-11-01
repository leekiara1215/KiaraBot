#include <csignal>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <stdexcept>
#include <string>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/runtime/platform/service_process.hpp"
#include "kiarabot/core/runtime/platform/service_rpc.hpp"
#include "kiarabot/v1/identity.grpc.pb.h"

namespace {
namespace target = kiarabot::core::target;
namespace platform = kiarabot::core::platform;

struct Dependency {};
class Application final : public target::ServiceReadinessPort {
public:
  Application(target::Service service, const std::shared_ptr<Dependency>& dependency,
              std::shared_ptr<Dependency> lifetime)
      : service_(service), dependency_(dependency), lifetime_(std::move(lifetime)) {}
  ~Application() override {
    std::cerr << "application_destroyed dependencies_alive=" << !dependency_.expired() << '\n';
  }
  target::Service service() const noexcept override {
    return service_;
  }
  target::ServiceReadiness readiness() const override {
    return {
        false,
        {{target::Dependency::BusinessImplementation, target::DependencyState::NotImplemented}}};
  }

private:
  target::Service service_;
  std::weak_ptr<Dependency> dependency_;
  std::shared_ptr<Dependency> lifetime_;
};
class Endpoint final : public platform::ServiceEndpoint {
public:
  Endpoint(std::weak_ptr<Dependency> application, std::string mode)
      : application_(std::move(application)), mode_(std::move(mode)) {}
  ~Endpoint() override {
    std::cerr << "endpoint_destroyed application_alive=" << !application_.expired() << '\n';
  }
  target::Status start() override {
    if (mode_ == "endpoint-start-failure") {
      return failure();
    }
    return std::monostate{};
  }
  target::Status check() const override {
    return mode_ == "endpoint-lost" ? failure() : target::Status{std::monostate{}};
  }

private:
  static target::Failure failure() {
    return {target::ErrorCode::UpstreamUnavailable,
            target::Acceptance::NotAccepted,
            target::Recovery::Diagnose,
            {},
            {}};
  }
  std::weak_ptr<Dependency> application_;
  std::string mode_;
};
class RejectingIdentity final : public kiarabot::v1::IdentityService::Service {
public:
  explicit RejectingIdentity(std::weak_ptr<Dependency> application)
      : application_(std::move(application)) {}
  ~RejectingIdentity() override {
    std::cerr << "rpc_destroyed application_alive=" << !application_.expired() << '\n';
  }
  grpc::Status Login(grpc::ServerContext*, const kiarabot::v1::IdentityLoginRequest*,
                     kiarabot::v1::IdentityLoginResponse*) override {
    // Lifecycle-only fixture. A registered handler is deliberately a denial, not fake login.
    return {grpc::StatusCode::PERMISSION_DENIED, "fixture denial"};
  }

private:
  std::weak_ptr<Dependency> application_;
};
} // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    return 2;
  }
  const std::string mode = argv[1];
  return platform::run_service(
      target::Service::Identity,
      [&](const platform::ProcessConfig&) -> target::Outcome<platform::ServiceComposition> {
        std::cerr << "composer_called\n";
        sigset_t blocked{};
        if (pthread_sigmask(SIG_BLOCK, nullptr, &blocked) != 0 || !sigismember(&blocked, SIGINT) ||
            !sigismember(&blocked, SIGTERM)) {
          throw std::runtime_error("signals not blocked before composition");
        }
        if (mode == "error") {
          return target::Failure{target::ErrorCode::UpstreamUnavailable,
                                 target::Acceptance::NotAccepted,
                                 target::Recovery::Diagnose,
                                 {},
                                 {}};
        }
        if (mode == "throw") {
          throw std::runtime_error("never-print-composition-secret");
        }
        if (mode == "unknown-throw") {
          throw 7;
        }
        if (mode == "null") {
          return platform::ServiceComposition{nullptr};
        }
        auto dependency = std::make_shared<Dependency>();
        auto lifetime = std::make_shared<Dependency>();
        auto application = std::make_unique<Application>(
            mode == "wrong-owner" ? target::Service::Control : target::Service::Identity,
            dependency, lifetime);
        std::function<target::Status(platform::ServiceRpc&)> configure;
        if (mode.starts_with("rpc-")) {
          configure = [mode, observed = std::weak_ptr<Dependency>{lifetime}](
                          platform::ServiceRpc& rpc) -> target::Status {
            if (mode == "rpc-throw") {
              throw std::runtime_error("never-print-composition-secret");
            }
            if (mode == "rpc-null") {
              return rpc.add_service(target::Service::Identity, nullptr);
            }
            return rpc.add_service(mode == "rpc-wrong-owner" ? target::Service::Runtime
                                                             : target::Service::Identity,
                                   std::make_unique<RejectingIdentity>(observed));
          };
        }
        auto endpoint =
            mode.starts_with("endpoint-") ? std::make_unique<Endpoint>(lifetime, mode) : nullptr;
        return platform::ServiceComposition{
            std::move(application),
            [dependency, mode, calls = 0]() mutable -> target::Status {
              (void)
                  dependency; // Keep the composition dependency alive through application teardown.
              if (++calls >= 2 && mode == "lost") {
                return target::Failure{target::ErrorCode::UpstreamUnavailable,
                                       target::Acceptance::NotAccepted,
                                       target::Recovery::Diagnose,
                                       {},
                                       {}};
              }
              return std::monostate{};
            },
            std::move(configure), std::move(endpoint)};
      },
      argc - 1, argv + 1);
}
