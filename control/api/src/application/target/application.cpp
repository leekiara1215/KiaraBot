#include "kiarabot/control/application/target/application.hpp"

namespace kiarabot::control::target {
namespace {

class ControlApplication final : public core::target::ServiceReadinessPort {
public:
  core::target::Service service() const noexcept override {
    return core::target::Service::Control;
  }

  core::target::ServiceReadiness readiness() const override {
    using core::target::Dependency;
    using core::target::DependencyState;
    return {false,
            {{Dependency::BusinessImplementation, DependencyState::NotImplemented},
             {Dependency::Identity, DependencyState::Unverified},
             {Dependency::Credentials, DependencyState::Unverified}}};
  }
};

} // namespace

std::unique_ptr<core::target::ServiceReadinessPort> make_application() {
  return std::make_unique<ControlApplication>();
}

} // namespace kiarabot::control::target
