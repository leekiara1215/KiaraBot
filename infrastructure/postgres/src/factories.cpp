#include "kiarabot/infrastructure/postgres/factories.hpp"

#include <exception>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "identity_security.hpp"
#include "kiarabot/infrastructure/postgres/security_factory.hpp"
#include "record_access.hpp"

namespace kiarabot::infrastructure::postgres {
namespace {
namespace target = core::target;

target::Failure failure(target::ErrorCode code) {
  return {code, target::Acceptance::NotAccepted, target::Recovery::Diagnose, {}, {}};
}
template <typename Key> std::vector<std::string> key_parts(const Key& key) {
  if constexpr (std::is_same_v<Key, target::TaskKey>) {
    return {key.owner.value, key.task_id};
  } else if constexpr (std::is_same_v<Key, target::ModelCallKey>) {
    return {key.owner.value, key.logical_call_id};
  } else {
    return {key.value};
  }
}
template <typename Key, typename Value>
class LiveRepository final : public target::Repository<Key, Value> {
public:
  LiveRepository(target::UnitOfWork& work, detail::RecordKind kind) : work_(work), kind_(kind) {}
  target::Outcome<std::optional<Value>> find(const Key& key) override {
    auto found = detail::find_record(work_, kind_, key_parts(key));
    if (const auto* rejected = std::get_if<target::Failure>(&found)) {
      return *rejected;
    }
    auto record = std::move(std::get<std::optional<target::ReplayResult>>(found));
    if (!record) {
      return std::optional<Value>{};
    }
    if (auto* value = std::get_if<Value>(&*record)) {
      return std::optional<Value>(std::move(*value));
    }
    return failure(target::ErrorCode::Internal);
  }
  target::Status save(const Value& value,
                      std::optional<target::ResourceVersion> expected) override {
    return detail::save_record(work_, kind_, target::ReplayResult(value), expected);
  }

private:
  target::UnitOfWork& work_;
  detail::RecordKind kind_;
};
class AuthorizationRepository final
    : public target::Repository<target::SubjectId, target::AuthorizationState> {
public:
  explicit AuthorizationRepository(target::UnitOfWork& work) : work_(work) {}
  target::Outcome<std::optional<target::AuthorizationState>>
  find(const target::SubjectId& key) override {
    return detail::find_authorization(work_, key);
  }
  target::Status save(const target::AuthorizationState& value,
                      std::optional<target::ResourceVersion> expected) override {
    return detail::save_authorization(work_, value, expected);
  }

private:
  target::UnitOfWork& work_;
};
template <typename Work> class WorkDelegate : public Work {
public:
  explicit WorkDelegate(std::unique_ptr<target::UnitOfWork> common) : common_(std::move(common)) {}
  target::Service owner() const noexcept override {
    return common_->owner();
  }
  target::Outcome<target::TimePoint> current_time() override {
    return common_->current_time();
  }
  target::ReceiptRepository& receipts() override {
    return common_->receipts();
  }
  target::JournalRepository& journal() override {
    return common_->journal();
  }
  target::Repository<target::OperationId, target::Operation>& operations() override {
    return common_->operations();
  }
  target::CommitResult commit() override {
    return common_->commit();
  }
  void rollback() noexcept override {
    common_->rollback();
  }

protected:
  target::UnitOfWork& work() {
    return *common_;
  }

private:
  std::unique_ptr<target::UnitOfWork> common_;
};
class IdentityWork final : public WorkDelegate<target::IdentityUnitOfWork> {
public:
  explicit IdentityWork(std::unique_ptr<target::UnitOfWork> common)
      : WorkDelegate(std::move(common)), principals_(work(), detail::RecordKind::Principal),
        sessions_(work(), detail::RecordKind::Session), grants_(work(), detail::RecordKind::Grant),
        authorizations_(work()) {}
  target::Repository<target::SubjectId, target::Principal>& principals() override {
    return principals_;
  }
  target::Repository<target::SessionId, target::LoginSession>& sessions() override {
    return sessions_;
  }
  target::Repository<target::AssignmentId, target::ResourceGrant>& grants() override {
    return grants_;
  }
  target::Repository<target::SubjectId, target::AuthorizationState>& authorizations() override {
    return authorizations_;
  }

private:
  LiveRepository<target::SubjectId, target::Principal> principals_;
  LiveRepository<target::SessionId, target::LoginSession> sessions_;
  LiveRepository<target::AssignmentId, target::ResourceGrant> grants_;
  AuthorizationRepository authorizations_;
};
class RuntimeWork final : public WorkDelegate<target::RuntimeUnitOfWork> {
public:
  explicit RuntimeWork(std::unique_ptr<target::UnitOfWork> common)
      : WorkDelegate(std::move(common)), accounts_(work(), detail::RecordKind::Account),
        installations_(work(), detail::RecordKind::Installation),
        instances_(work(), detail::RecordKind::Instance) {}
  target::Repository<target::ResourceId, target::BotAccount>& accounts() override {
    return accounts_;
  }
  target::Repository<target::InstallationId, target::PluginInstallation>& installations() override {
    return installations_;
  }
  target::Repository<target::InstanceId, target::PluginInstance>& instances() override {
    return instances_;
  }

private:
  LiveRepository<target::ResourceId, target::BotAccount> accounts_;
  LiveRepository<target::InstallationId, target::PluginInstallation> installations_;
  LiveRepository<target::InstanceId, target::PluginInstance> instances_;
};
class SchedulerWork final : public WorkDelegate<target::SchedulerUnitOfWork> {
public:
  explicit SchedulerWork(std::unique_ptr<target::UnitOfWork> common)
      : WorkDelegate(std::move(common)), tasks_(work(), detail::RecordKind::Task),
        occurrences_(work(), detail::RecordKind::Occurrence) {}
  target::Repository<target::TaskKey, target::ScheduledTask>& tasks() override {
    return tasks_;
  }
  target::Repository<target::MessageId, target::Occurrence>& occurrences() override {
    return occurrences_;
  }

private:
  LiveRepository<target::TaskKey, target::ScheduledTask> tasks_;
  LiveRepository<target::MessageId, target::Occurrence> occurrences_;
};
class ModelWork final : public WorkDelegate<target::ModelUnitOfWork> {
public:
  explicit ModelWork(std::unique_ptr<target::UnitOfWork> common)
      : WorkDelegate(std::move(common)), intents_(work(), detail::RecordKind::ModelIntent) {}
  target::Repository<target::ModelCallKey, target::ModelIntent>& model_intents() override {
    return intents_;
  }

private:
  LiveRepository<target::ModelCallKey, target::ModelIntent> intents_;
};
class IdentitySecurityWork final : public target::IdentitySecurityUnitOfWork {
public:
  explicit IdentitySecurityWork(detail::CommonWorkAccess common)
      : common_(std::move(common)), principals_(work(), detail::RecordKind::Principal),
        sessions_(work(), detail::RecordKind::Session), grants_(work(), detail::RecordKind::Grant),
        authorizations_(work()), credentials_(detail::credential_repository(*common_.sql)),
        security_(detail::session_security_repository(*common_.sql)),
        assignments_(detail::exact_assignment_repository(*common_.sql)),
        secrets_(detail::secret_receipt_repository(*common_.sql)),
        bootstrap_(detail::bootstrap_repository(*common_.sql)) {}
  target::Outcome<target::TimePoint> current_time() override {
    return work().current_time();
  }
  target::ReceiptRepository& receipts() override {
    return work().receipts();
  }
  target::JournalRepository& journal() override {
    return work().journal();
  }
  target::Repository<target::OperationId, target::Operation>& operations() override {
    return work().operations();
  }
  target::CommitResult commit() override {
    return work().commit();
  }
  void rollback() noexcept override {
    work().rollback();
  }
  target::Repository<target::SubjectId, target::Principal>& principals() override {
    return principals_;
  }
  target::Repository<target::SessionId, target::LoginSession>& sessions() override {
    return sessions_;
  }
  target::Repository<target::AssignmentId, target::ResourceGrant>& grants() override {
    return grants_;
  }
  target::Repository<target::SubjectId, target::AuthorizationState>& authorizations() override {
    return authorizations_;
  }
  target::IdentityCredentialRepository& credentials() override {
    return *credentials_;
  }
  target::IdentitySessionSecurityRepository& session_security() override {
    return *security_;
  }
  target::IdentityExactAssignmentRepository& exact_assignments() override {
    return *assignments_;
  }
  target::IdentitySecretReceiptRepository& secret_receipts() override {
    return *secrets_;
  }
  target::IdentityBootstrapRepository& bootstrap() override {
    return *bootstrap_;
  }

private:
  target::UnitOfWork& work() {
    return *common_.work;
  }
  detail::CommonWorkAccess common_;
  LiveRepository<target::SubjectId, target::Principal> principals_;
  LiveRepository<target::SessionId, target::LoginSession> sessions_;
  LiveRepository<target::AssignmentId, target::ResourceGrant> grants_;
  AuthorizationRepository authorizations_;
  std::unique_ptr<target::IdentityCredentialRepository> credentials_;
  std::unique_ptr<target::IdentitySessionSecurityRepository> security_;
  std::unique_ptr<target::IdentityExactAssignmentRepository> assignments_;
  std::unique_ptr<target::IdentitySecretReceiptRepository> secrets_;
  std::unique_ptr<target::IdentityBootstrapRepository> bootstrap_;
};
class IdentitySecurityFactory final : public target::IdentitySecurityUnitOfWorkFactory {
public:
  explicit IdentitySecurityFactory(std::unique_ptr<Database> database)
      : database_(std::move(database)) {}
  target::Outcome<std::unique_ptr<target::IdentitySecurityUnitOfWork>> begin() override {
    auto begun = detail::DatabaseAccess::begin(*database_);
    if (const auto* rejected = std::get_if<target::Failure>(&begun)) {
      return *rejected;
    }
    try {
      return std::unique_ptr<target::IdentitySecurityUnitOfWork>(
          std::make_unique<IdentitySecurityWork>(
              std::move(std::get<detail::CommonWorkAccess>(begun))));
    } catch (...) {
      return failure(target::ErrorCode::Internal);
    }
  }

private:
  std::unique_ptr<Database> database_;
};

template <typename Work, typename Contract>
class Factory final : public target::LocalUnitOfWorkFactory<Contract> {
public:
  explicit Factory(std::unique_ptr<Database> database) : database_(std::move(database)) {}
  target::Outcome<std::unique_ptr<Contract>> begin() override {
    auto begun = database_->begin();
    if (const auto* rejected = std::get_if<target::Failure>(&begun)) {
      return *rejected;
    }
    try {
      return std::unique_ptr<Contract>(
          std::make_unique<Work>(std::move(std::get<std::unique_ptr<target::UnitOfWork>>(begun))));
    } catch (...) {
      return failure(target::ErrorCode::Internal);
    }
  }

private:
  std::unique_ptr<Database> database_;
};
template <typename Work, typename Contract>
target::Outcome<std::unique_ptr<target::LocalUnitOfWorkFactory<Contract>>>
open_factory(const DatabaseConfig& config, target::Service owner) {
  if (config.service != owner) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  auto opened = Database::open(config);
  if (const auto* rejected = std::get_if<target::Failure>(&opened)) {
    return *rejected;
  }
  try {
    return std::unique_ptr<target::LocalUnitOfWorkFactory<Contract>>(
        std::make_unique<Factory<Work, Contract>>(
            std::move(std::get<std::unique_ptr<Database>>(opened))));
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}
} // namespace

target::Outcome<std::unique_ptr<target::IdentitySecurityUnitOfWorkFactory>>
open_identity_security(const DatabaseConfig& config) {
  if (config.service != target::Service::Identity) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  auto opened = Database::open(config);
  if (const auto* rejected = std::get_if<target::Failure>(&opened)) {
    return *rejected;
  }
  try {
    return std::unique_ptr<target::IdentitySecurityUnitOfWorkFactory>(
        std::make_unique<IdentitySecurityFactory>(
            std::move(std::get<std::unique_ptr<Database>>(opened))));
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}

target::Outcome<std::unique_ptr<target::IdentityUnitOfWorkFactory>>
open_identity(const DatabaseConfig& config) {
  return open_factory<IdentityWork, target::IdentityUnitOfWork>(config, target::Service::Identity);
}
target::Outcome<std::unique_ptr<target::RuntimeUnitOfWorkFactory>>
open_runtime(const DatabaseConfig& config) {
  return open_factory<RuntimeWork, target::RuntimeUnitOfWork>(config, target::Service::Runtime);
}
target::Outcome<std::unique_ptr<target::SchedulerUnitOfWorkFactory>>
open_scheduler(const DatabaseConfig& config) {
  return open_factory<SchedulerWork, target::SchedulerUnitOfWork>(config,
                                                                  target::Service::Scheduler);
}
target::Outcome<std::unique_ptr<target::ModelUnitOfWorkFactory>>
open_model(const DatabaseConfig& config) {
  return open_factory<ModelWork, target::ModelUnitOfWork>(config, target::Service::Model);
}
} // namespace kiarabot::infrastructure::postgres
