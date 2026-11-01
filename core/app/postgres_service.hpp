#pragma once

#include <atomic>
#include <memory>
#include <utility>

#include "kiarabot/core/runtime/platform/service_config.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"
#include "kiarabot/infrastructure/postgres/factories.hpp"

namespace kiarabot::core::composition {
namespace detail {

inline target::Failure database_unavailable() {
  return {target::ErrorCode::UpstreamUnavailable,
          target::Acceptance::NotAccepted,
          target::Recovery::Backoff,
          {},
          {}};
}

template <typename Work>
class UnconfiguredFactory final : public target::LocalUnitOfWorkFactory<Work> {
public:
  target::Outcome<std::unique_ptr<Work>> begin() override {
    return database_unavailable();
  }
};

struct DatabaseObservation {
  std::atomic<bool> available{false};
};

class ObservedApplication final : public target::ServiceReadinessPort {
public:
  ObservedApplication(std::unique_ptr<target::ServiceReadinessPort> application,
                      std::shared_ptr<DatabaseObservation> observation)
      : application_(std::move(application)), observation_(std::move(observation)) {}
  target::Service service() const noexcept override {
    return application_->service();
  }
  target::ServiceReadiness readiness() const override {
    auto result = application_->readiness();
    const bool available = observation_->available.load();
    bool found = false;
    for (auto& dependency : result.dependencies) {
      if (dependency.dependency == target::Dependency::DatabaseSchema) {
        dependency.state =
            available ? target::DependencyState::Available : target::DependencyState::Unavailable;
        found = true;
      }
    }
    if (!found) {
      result.dependencies.push_back(
          {target::Dependency::DatabaseSchema,
           available ? target::DependencyState::Available : target::DependencyState::Unavailable});
    }
    result.ready = result.ready && available;
    return result;
  }

private:
  std::unique_ptr<target::ServiceReadinessPort> application_;
  std::shared_ptr<DatabaseObservation> observation_;
};

} // namespace detail

// Composition-root glue only. Neither configuration nor PG types enter the application.
// An absent database has a rejecting port, never a memory/SQLite storage fallback.
template <typename Work, typename Builder>
target::Outcome<platform::ServiceComposition>
compose_database_service(const platform::ProcessConfig& config,
                         target::Outcome<std::unique_ptr<target::LocalUnitOfWorkFactory<Work>>> (
                             *open)(const infrastructure::postgres::DatabaseConfig&),
                         Builder build_application) {
  using Factory = target::LocalUnitOfWorkFactory<Work>;
  std::shared_ptr<Factory> factory;
  const bool configured = config.database.has_value();
  if (configured) {
    const auto& database = *config.database;
    const infrastructure::postgres::DatabaseConfig connection{
        config.service,   database.host,           database.port,
        database.name,    database.runtime_role,   database.password,
        database.ca_file, database.migration_role, database.runtime_role};
    auto opened = open(connection);
    if (const auto* failure = std::get_if<target::Failure>(&opened)) {
      return *failure;
    }
    factory = std::shared_ptr<Factory>(std::move(std::get<std::unique_ptr<Factory>>(opened)));
    if (!factory) {
      return detail::database_unavailable();
    }
  } else {
    factory = std::make_shared<detail::UnconfiguredFactory<Work>>();
  }
  auto application = build_application(*factory);
  if (!application) {
    return detail::database_unavailable();
  }
  auto observation = std::make_shared<detail::DatabaseObservation>();
  observation->available.store(configured);
  return platform::ServiceComposition{
      std::make_unique<detail::ObservedApplication>(std::move(application), observation),
      [factory, observation, configured]() -> target::Status {
        if (!configured) {
          return std::monostate{};
        }
        auto begun = factory->begin();
        if (const auto* failure = std::get_if<target::Failure>(&begun)) {
          if (failure->code == target::ErrorCode::Conflict &&
              failure->acceptance == target::Acceptance::NotAccepted) {
            return std::monostate{}; // A bounded busy writer is not a lost writer session.
          }
          observation->available.store(false);
          return *failure;
        }
        auto work = std::move(std::get<std::unique_ptr<Work>>(begun));
        if (!work) {
          observation->available.store(false);
          return detail::database_unavailable();
        }
        const auto sampled = work->current_time();
        work->rollback();
        if (const auto* failure = std::get_if<target::Failure>(&sampled)) {
          observation->available.store(false);
          return *failure;
        }
        observation->available.store(true);
        return std::monostate{};
      }};
}

} // namespace kiarabot::core::composition
