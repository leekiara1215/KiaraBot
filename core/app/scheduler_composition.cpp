#include "scheduler_composition.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/rand.h>

#include "kiarabot/core/application/target/scheduler_service.hpp"
#include "kiarabot/core/runtime/platform/scheduler_rpc.hpp"
#include "kiarabot/core/runtime/platform/scheduler_runtime_client.hpp"
#include "kiarabot/core/runtime/platform/service_rpc.hpp"
#include "kiarabot/infrastructure/postgres/factories.hpp"
#include "postgres_service.hpp"

namespace kiarabot::core::composition {
namespace {
namespace target = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace postgres = kiarabot::infrastructure::postgres;
namespace v1 = kiarabot::v1;

target::Failure failure(target::ErrorCode code,
                        target::Recovery recovery = target::Recovery::Diagnose) {
  return {code, target::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}

class SchedulerIds final : public target::SchedulerIdPort {
public:
  target::Outcome<target::MessageId> occurrence_id() override {
    auto text = uuid();
    if (const auto* rejected = std::get_if<target::Failure>(&text)) {
      return *rejected;
    }
    return target::MessageId{std::get<std::string>(text)};
  }
  target::Outcome<target::OperationId> operation_id() override {
    auto text = uuid();
    if (const auto* rejected = std::get_if<target::Failure>(&text)) {
      return *rejected;
    }
    return target::OperationId{std::get<std::string>(text)};
  }

private:
  target::Outcome<std::string> uuid() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
      return failure(target::ErrorCode::UpstreamUnavailable, target::Recovery::Backoff);
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      if (index == 4 || index == 6 || index == 8 || index == 10) {
        result += '-';
      }
      result += kDigits[bytes[index] >> 4U];
      result += kDigits[bytes[index] & 0x0fU];
    }
    return result;
  }
};

// Production IANA adapter over the POSIX zoneinfo database. The reference
// infrastructure::TimeZoneResolver links a second generated protobuf copy, which cannot be mixed
// into the owner-service binary without duplicate-symbol/ODR faults; this adapter keeps the same
// frozen error split without touching Abseil or reference protobuf types.
class SchedulerZones final : public target::SchedulerTimeZonePort {
public:
  bool available() const override {
    static const bool present = [] {
      std::error_code error;
      return std::filesystem::is_directory("/usr/share/zoneinfo", error);
    }();
    return present;
  }
  target::Outcome<target::TimePoint> resolve(const target::LocalCivilTime& local) override {
    if (local.year < 1970 || local.year > 2100) {
      return failure(target::ErrorCode::InvalidArgument, target::Recovery::CorrectInput);
    }
    if (!available()) {
      return failure(target::ErrorCode::UpstreamUnavailable, target::Recovery::Backoff);
    }
    if (!zone_path(local.time_zone)) {
      return failure(target::ErrorCode::InvalidArgument, target::Recovery::CorrectInput);
    }
    std::lock_guard lock(mutex_);
    if (::setenv("TZ", local.time_zone.c_str(), 1) != 0) {
      return failure(target::ErrorCode::UpstreamUnavailable, target::Recovery::Backoff);
    }
    ::tzset();
    struct tm requested {};
    requested.tm_year = local.year - 1900;
    requested.tm_mon = local.month - 1;
    requested.tm_mday = local.day;
    requested.tm_hour = local.hour;
    requested.tm_min = local.minute;
    requested.tm_sec = local.second;
    requested.tm_isdst = -1;
    struct tm primary = requested;
    errno = 0;
    const std::time_t first = ::mktime(&primary);
    if (first == static_cast<std::time_t>(-1)) {
      return failure(target::ErrorCode::InvalidArgument, target::Recovery::CorrectInput);
    }
    struct tm normalized {};
    if (::localtime_r(&first, &normalized) == nullptr || !same_civil(normalized, local)) {
      // mktime normalized a nonexistent civil time through a DST gap.
      return failure(target::ErrorCode::InvalidArgument, target::Recovery::CorrectInput);
    }
    // A fold yields two distinct instants for the same civil time: probe the opposite DST intent.
    struct tm alternate = requested;
    alternate.tm_isdst = primary.tm_isdst == 0 ? 1 : 0;
    errno = 0;
    const std::time_t second = ::mktime(&alternate);
    if (second != static_cast<std::time_t>(-1) && second != first) {
      struct tm alternate_local {};
      if (::localtime_r(&second, &alternate_local) != nullptr &&
          same_civil(alternate_local, local)) {
        return failure(target::ErrorCode::InvalidArgument, target::Recovery::CorrectInput);
      }
    }
    return target::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::seconds{static_cast<std::int64_t>(first)})};
  }

private:
  static bool same_civil(const struct tm& value, const target::LocalCivilTime& local) {
    return value.tm_year == local.year - 1900 && value.tm_mon == local.month - 1 &&
           value.tm_mday == local.day && value.tm_hour == local.hour &&
           value.tm_min == local.minute && value.tm_sec == local.second;
  }
  static bool zone_path(const std::string& zone) {
    if (zone.empty() || zone.size() > 128 || zone.find("..") != std::string::npos ||
        zone.front() == '/') {
      return false;
    }
    std::error_code error;
    const auto path = std::filesystem::canonical("/usr/share/zoneinfo/" + zone, error);
    if (error || !std::filesystem::is_regular_file(path, error)) {
      return false;
    }
    const auto base = std::filesystem::canonical("/usr/share/zoneinfo", error);
    return !error && path.string().starts_with(base.string() + "/");
  }

  mutable std::mutex mutex_;
};

class ApplicationHolder final : public target::ServiceReadinessPort {
public:
  explicit ApplicationHolder(std::shared_ptr<target::SchedulerService> service)
      : service_(std::move(service)) {}
  target::Service service() const noexcept override {
    return target::Service::Scheduler;
  }
  target::ServiceReadiness readiness() const override {
    return service_->readiness();
  }

private:
  std::shared_ptr<target::SchedulerService> service_;
};

class FailClosedApplication final : public target::ServiceReadinessPort {
public:
  explicit FailClosedApplication(std::unique_ptr<target::ServiceReadinessPort> inner)
      : inner_(std::move(inner)) {}
  target::Service service() const noexcept override {
    return inner_->service();
  }
  target::ServiceReadiness readiness() const override {
    auto result = inner_->readiness();
    for (auto& dependency : result.dependencies) {
      if (dependency.dependency == target::Dependency::Identity) {
        dependency.state = target::DependencyState::Unavailable;
      }
    }
    result.ready = false;
    return result;
  }

private:
  std::unique_ptr<target::ServiceReadinessPort> inner_;
};

target::Outcome<platform::ServiceComposition> compose(const platform::ProcessConfig& config) {
  std::shared_ptr<target::SchedulerUnitOfWorkFactory> factory;
  const bool configured = config.database.has_value();
  if (configured) {
    const auto& database = *config.database;
    const postgres::DatabaseConfig connection{
        config.service,   database.host,           database.port,
        database.name,    database.runtime_role,   database.password,
        database.ca_file, database.migration_role, database.runtime_role};
    auto opened = postgres::open_scheduler(connection);
    if (const auto* rejected = std::get_if<target::Failure>(&opened)) {
      return *rejected;
    }
    factory = std::shared_ptr<target::SchedulerUnitOfWorkFactory>(
        std::move(std::get<std::unique_ptr<target::SchedulerUnitOfWorkFactory>>(opened)));
  } else {
    factory = std::make_shared<detail::UnconfiguredFactory<target::SchedulerUnitOfWork>>();
  }
  auto zones = std::make_shared<SchedulerZones>();
  auto ids = std::make_shared<SchedulerIds>();
  std::shared_ptr<platform::SchedulerRuntimeClient> runtime_client;
  if (configured && config.runtime_endpoint) {
    grpc::SslCredentialsOptions ssl;
    ssl.pem_root_certs = config.tls.ca;
    ssl.pem_cert_chain = config.tls.certificate;
    ssl.pem_private_key = config.tls.private_key;
    auto client = platform::SchedulerRuntimeClient::create(
        {*config.runtime_endpoint, grpc::SslCredentials(ssl), std::chrono::milliseconds{5000}});
    if (const auto* rejected = std::get_if<target::Failure>(&client)) {
      return *rejected;
    }
    runtime_client = std::move(std::get<std::unique_ptr<platform::SchedulerRuntimeClient>>(client));
  }
  struct KeepAlive {
    std::shared_ptr<target::SchedulerUnitOfWorkFactory> factory;
    std::shared_ptr<SchedulerZones> zones;
    std::shared_ptr<SchedulerIds> ids;
    std::shared_ptr<platform::SchedulerRuntimeClient> runtime_client;
  };
  auto keep_alive = std::make_shared<KeepAlive>(KeepAlive{factory, zones, ids, runtime_client});
  auto service = std::make_shared<target::SchedulerService>(target::SchedulerServiceDependencies{
      *factory, *ids, *zones, target::kTaskHorizon, target::kInstanceTaskCapacity,
      target::kInstallationTaskCapacity, runtime_client.get(), keep_alive});

  platform::SchedulerRpcOptions rpc_options;
  rpc_options.authorize =
      [](grpc::ServerContext&, const v1::RequestContext&,
         const v1::ResourceScope*) -> target::Outcome<target::SchedulerCommandContext> {
    // Fail closed until Identity delegation/current-authorization is wired for Scheduler.
    return failure(target::ErrorCode::Unauthenticated, target::Recovery::Authenticate);
  };
  auto rpc = platform::make_scheduler_rpc(*service, std::move(rpc_options));
  if (const auto* rejected = std::get_if<target::Failure>(&rpc)) {
    return *rejected;
  }
  auto database_observation = std::make_shared<detail::DatabaseObservation>();
  database_observation->available.store(configured);

  auto maintain = [factory, service, database_observation, runtime_client]() -> target::Status {
    if (!database_observation->available.load()) {
      return std::monostate{};
    }
    auto work = factory->begin();
    if (const auto* rejected = std::get_if<target::Failure>(&work)) {
      if (rejected->code == target::ErrorCode::Conflict) {
        return std::monostate{};
      }
      return *rejected;
    }
    auto uow = std::move(std::get<std::unique_ptr<target::SchedulerUnitOfWork>>(work));
    auto sample = uow->current_time();
    if (const auto* rejected = std::get_if<target::Failure>(&sample)) {
      uow->rollback();
      if (rejected->code == target::ErrorCode::Conflict) {
        return std::monostate{};
      }
      return *rejected;
    }
    const auto now = std::get<target::TimePoint>(sample);
    (void)service->due_sweep(now, 10);
    (void)service->expire_stale_triggers(now, 10);
    (void)uow->receipts().prune_expired(now, 100);
    (void)uow->journal().prune_received(now, 100);
    if (uow->commit() != target::CommitResult::Committed) {
      return failure(target::ErrorCode::UpstreamUnavailable, target::Recovery::Backoff);
    }
    if (runtime_client) {
      (void)service->deliver_triggers(now, 5);
    }
    return std::monostate{};
  };
  auto check = [maintain]() -> target::Status { return maintain(); };
  std::unique_ptr<target::ServiceReadinessPort> application =
      std::make_unique<FailClosedApplication>(std::make_unique<detail::ObservedApplication>(
          std::make_unique<ApplicationHolder>(service), database_observation));
  auto rpc_holder = std::make_shared<std::unique_ptr<v1::SchedulerService::Service>>(
      std::move(std::get<std::unique_ptr<v1::SchedulerService::Service>>(rpc)));
  return platform::ServiceComposition{std::move(application),
                                      std::move(check),
                                      [rpc_holder](platform::ServiceRpc& server) -> target::Status {
                                        return server.add_service(target::Service::Scheduler,
                                                                  std::move(*rpc_holder));
                                      },
                                      {},
                                      {}};
}
} // namespace

target::Outcome<platform::ServiceComposition>
compose_scheduler_service(const platform::ProcessConfig& config) {
  return compose(config);
}

} // namespace kiarabot::core::composition
