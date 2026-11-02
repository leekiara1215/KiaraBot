#include "kiarabot/control/application/target/application.hpp"

namespace kiarabot::control::target {
namespace {

class ControlApplication final : public core::target::ServiceReadinessPort {
public:
  explicit ControlApplication(std::shared_ptr<ControlReadiness> readiness)
      : readiness_(std::move(readiness)) {}

  core::target::Service service() const noexcept override {
    return core::target::Service::Control;
  }

  core::target::ServiceReadiness readiness() const override {
    using core::target::Dependency;
    using core::target::DependencyState;
    if (!readiness_) {
      return {false,
              {{Dependency::BusinessImplementation, DependencyState::NotImplemented},
               {Dependency::Identity, DependencyState::Unverified},
               {Dependency::Credentials, DependencyState::Unverified}}};
    }
    const bool business = readiness_->business.load(std::memory_order_acquire);
    const bool credentials = readiness_->credentials.load(std::memory_order_acquire);
    const bool identity = readiness_->identity.load(std::memory_order_acquire);
    return {business && credentials && identity,
            {{Dependency::BusinessImplementation,
              business ? DependencyState::Available : DependencyState::Unverified},
             {Dependency::Identity,
              identity ? DependencyState::Available : DependencyState::Unverified},
             {Dependency::Credentials,
              credentials ? DependencyState::Available : DependencyState::Unverified}}};
  }

private:
  std::shared_ptr<ControlReadiness> readiness_;
};

} // namespace

std::unique_ptr<core::target::ServiceReadinessPort>
make_application(std::shared_ptr<ControlReadiness> readiness) {
  return std::make_unique<ControlApplication>(std::move(readiness));
}

} // namespace kiarabot::control::target
