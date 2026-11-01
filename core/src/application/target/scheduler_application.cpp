#include "kiarabot/core/application/target/scheduler_application.hpp"

namespace kiarabot::core::target {
namespace {

class SchedulerApplication final : public ServiceReadinessPort {
public:
  Service service() const noexcept override {
    return Service::Scheduler;
  }
  ServiceReadiness readiness() const override {
    return {false,
            {{Dependency::BusinessImplementation, DependencyState::NotImplemented},
             {Dependency::DatabaseSchema, DependencyState::Unverified},
             {Dependency::Identity, DependencyState::Unverified},
             {Dependency::Runtime, DependencyState::Unverified}}};
  }
};

} // namespace

std::unique_ptr<ServiceReadinessPort> make_scheduler_application() {
  return std::make_unique<SchedulerApplication>();
}

} // namespace kiarabot::core::target
