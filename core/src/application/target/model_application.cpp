#include "kiarabot/core/application/target/model_application.hpp"

namespace kiarabot::core::target {
namespace {

class ModelApplication final : public ServiceReadinessPort {
public:
  Service service() const noexcept override {
    return Service::Model;
  }
  ServiceReadiness readiness() const override {
    return {false,
            {{Dependency::BusinessImplementation, DependencyState::NotImplemented},
             {Dependency::DatabaseSchema, DependencyState::Unverified},
             {Dependency::Identity, DependencyState::Unverified},
             {Dependency::Credentials, DependencyState::Unverified}}};
  }
};

} // namespace

std::unique_ptr<ServiceReadinessPort> make_model_application() {
  return std::make_unique<ModelApplication>();
}

} // namespace kiarabot::core::target
