#include "kiarabot/core/application/target/runtime_application.hpp"

namespace kiarabot::core::target {
namespace {

class RuntimeApplication final : public ServiceReadinessPort {
public:
  Service service() const noexcept override {
    return Service::Runtime;
  }
  ServiceReadiness readiness() const override {
    return {false,
            {{Dependency::BusinessImplementation, DependencyState::NotImplemented},
             {Dependency::DatabaseSchema, DependencyState::Unverified},
             {Dependency::Identity, DependencyState::Unverified}}};
  }
};

} // namespace

std::unique_ptr<ServiceReadinessPort> make_runtime_application() {
  return std::make_unique<RuntimeApplication>();
}

} // namespace kiarabot::core::target
