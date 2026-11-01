#include "kiarabot/core/application/target/host_application.hpp"

namespace kiarabot::core::target {
namespace {

class HostApplication final : public ServiceReadinessPort {
public:
  Service service() const noexcept override {
    return Service::Host;
  }
  ServiceReadiness readiness() const override {
    return {false,
            {{Dependency::BusinessImplementation, DependencyState::NotImplemented},
             {Dependency::Identity, DependencyState::Unverified},
             {Dependency::Runtime, DependencyState::Unverified}}};
  }
};

} // namespace

std::unique_ptr<ServiceReadinessPort> make_host_application() {
  return std::make_unique<HostApplication>();
}

} // namespace kiarabot::core::target
