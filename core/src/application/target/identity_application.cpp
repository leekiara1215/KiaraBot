#include "kiarabot/core/application/target/identity_application.hpp"

namespace kiarabot::core::target {
namespace {

class IdentityApplication final : public ServiceReadinessPort {
public:
  Service service() const noexcept override {
    return Service::Identity;
  }
  ServiceReadiness readiness() const override {
    return {false,
            {{Dependency::BusinessImplementation, DependencyState::NotImplemented},
             {Dependency::DatabaseSchema, DependencyState::Unverified},
             {Dependency::Credentials, DependencyState::Unverified}}};
  }
};

} // namespace

std::unique_ptr<ServiceReadinessPort> make_identity_application() {
  return std::make_unique<IdentityApplication>();
}

} // namespace kiarabot::core::target
