#include <csignal>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <stdexcept>
#include <string>

#include "kiarabot/core/runtime/platform/service_process.hpp"

namespace {
namespace target = kiarabot::core::target;
namespace platform = kiarabot::core::platform;

struct Dependency {};
class Application final : public target::ServiceReadinessPort {
public:
  Application(target::Service service, const std::shared_ptr<Dependency>& dependency)
      : service_(service), dependency_(dependency) {}
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
        auto application = std::make_unique<Application>(
            mode == "wrong-owner" ? target::Service::Control : target::Service::Identity,
            dependency);
        return platform::ServiceComposition{
            std::move(application), [dependency, mode, calls = 0]() mutable -> target::Status {
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
            }};
      },
      argc - 1, argv + 1);
}
