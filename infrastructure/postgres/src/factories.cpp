#include "kiarabot/infrastructure/postgres/factories.hpp"

#include <exception>
#include <functional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "identity_security.hpp"
#include "kiarabot/core/domain/target/scheduler_rules.hpp"
#include "kiarabot/core/ports/target/business_ports.hpp"
#include "kiarabot/infrastructure/postgres/security_factory.hpp"
#include "model_store.hpp"
#include "record_access.hpp"
#include "runtime_events.hpp"

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
// Bounded owner-ordered list projection used by Runtime typed List queries. Casts each loaded
// record to the exact expected type; a mismatch is an internal storage fault, never an empty page.
template <typename Value>
target::Outcome<std::vector<Value>> listed(target::UnitOfWork& work, detail::RecordKind kind,
                                           std::size_t limit, const std::string& after) {
  auto records = detail::list_records(work, kind, limit, after);
  if (const auto* rejected = std::get_if<target::Failure>(&records)) {
    return *rejected;
  }
  std::vector<Value> result;
  auto& values = std::get<std::vector<target::ReplayResult>>(records);
  result.reserve(values.size());
  for (auto& record : values) {
    auto* value = std::get_if<Value>(&record);
    if (!value) {
      return failure(target::ErrorCode::Internal);
    }
    result.push_back(std::move(*value));
  }
  return result;
}
template <typename Value>
target::Outcome<std::vector<Value>>
load_records(target::UnitOfWork& work, detail::RecordKind kind,
             const std::vector<std::vector<std::string>>& keys) {
  std::vector<Value> result;
  result.reserve(keys.size());
  for (const auto& key : keys) {
    auto found = detail::find_record(work, kind, key);
    if (const auto* rejected = std::get_if<target::Failure>(&found)) {
      return *rejected;
    }
    const auto& optional = std::get<std::optional<target::ReplayResult>>(found);
    if (!optional) {
      return failure(target::ErrorCode::Internal);
    }
    const auto* value = std::get_if<Value>(&*optional);
    if (!value) {
      return failure(target::ErrorCode::Internal);
    }
    result.push_back(*value);
  }
  return result;
}

// Adapter registry has two reads: stable adapter resource id and the assigned adapter instance
// identity carried by registration/heartbeat frames. Both borrow the same owner UoW.
class AdapterRegistry final : public target::AdapterRegistryRepository {
public:
  explicit AdapterRegistry(target::UnitOfWork& work) : work_(work) {}
  target::Outcome<std::optional<target::AdapterRecord>>
  find(const target::ResourceId& adapter) override {
    const std::string key = adapter.value;
    auto found = detail::find_record(work_, detail::RecordKind::Adapter,
                                     std::span<const std::string>(&key, 1));
    if (const auto* rejected = std::get_if<target::Failure>(&found)) {
      return *rejected;
    }
    auto record = std::move(std::get<std::optional<target::ReplayResult>>(found));
    if (!record) {
      return std::optional<target::AdapterRecord>{};
    }
    if (auto* value = std::get_if<target::AdapterRecord>(&*record)) {
      return std::optional<target::AdapterRecord>(std::move(*value));
    }
    return failure(target::ErrorCode::Internal);
  }
  target::Outcome<std::optional<target::AdapterRecord>>
  by_instance(const target::InstanceId& instance) override {
    auto resolved = detail::find_adapter_id(work_, instance);
    if (const auto* rejected = std::get_if<target::Failure>(&resolved)) {
      return *rejected;
    }
    const auto& id = std::get<std::optional<target::ResourceId>>(resolved);
    if (!id) {
      return std::optional<target::AdapterRecord>{};
    }
    return find(*id);
  }
  target::Status save(const target::AdapterRecord& value,
                      std::optional<target::ResourceVersion> expected) override {
    return detail::save_record(work_, detail::RecordKind::Adapter, target::ReplayResult(value),
                               expected);
  }

private:
  target::UnitOfWork& work_;
};
// One-time external-bridge bootstrap credential persistence; plaintext is never stored.
class BridgeCredentials final : public target::BridgeCredentialRepository {
public:
  explicit BridgeCredentials(target::UnitOfWork& work) : work_(work) {}
  target::Outcome<bool> active(const std::string& resource_id) override {
    return detail::bridge_credential_active(work_, resource_id);
  }
  target::Outcome<std::optional<target::BridgeCredentialRecord>>
  by_digest(const target::Sha256Digest& digest) override {
    return detail::bridge_credential_by_digest(work_, digest);
  }
  target::Status insert(const target::BridgeCredentialRecord& record) override {
    return detail::insert_bridge_credential(work_, record);
  }
  target::Status revoke(const std::string& resource_id, target::TimePoint now) override {
    return detail::revoke_bridge_credentials(work_, resource_id, now);
  }

private:
  target::UnitOfWork& work_;
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
// P07a-A draft: the Action relations land in a follow-up commit once the shared audit columns
// and P06a-2 Runtime schema are merged and measured on real PG16.4. Until then every Action port
// fails closed with UPSTREAM_UNAVAILABLE; the Runtime application never treats this as absence.
#define KIARABOT_P07A_STUB_FAILURE return failure(target::ErrorCode::UpstreamUnavailable)
class ActionRepositoryStub final : public target::ActionRepository {
public:
  target::Outcome<std::optional<target::ActionRecord>>
  find_by_id(const target::ActionId&) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
  target::Outcome<std::optional<target::ActionRecord>>
  find_by_owner_key(const target::InstanceId&, std::string_view) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
  target::Outcome<target::ActionRecord> insert(const target::ActionRecord&,
                                               target::ActionCapacity) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
  target::Status mark_dispatched(const target::ActionId&, const target::AttemptId&,
                                 const target::InstanceId&, target::TimePoint) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
  target::Outcome<target::ActionTerminalReceipt>
  record_terminal(const target::ActionRecord&, const target::ActionTerminalResult&,
                  std::optional<target::AttemptId>, std::optional<target::InstanceId>,
                  target::TimePoint) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
  target::Status cancel(const target::ActionId&, target::TimePoint) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
  target::Outcome<std::vector<target::ActionRecord>> expire_due(std::size_t,
                                                                target::TimePoint) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
  target::Outcome<std::vector<target::ActionRecord>>
  list_owner(const target::InstanceId&, const target::ActionPageRequest&) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
  target::Outcome<std::vector<target::ActionRecord>>
  list_control(const target::ActionControlQuery&) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
  target::Outcome<std::size_t> prune_terminal(target::TimePoint, std::size_t) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
};
class ActionQuotaRepositoryStub final : public target::ActionQuotaRepository {
public:
  target::Status admit(const target::ActionId&, target::ActionKind,
                       std::span<const target::ActionScope>, const target::ActionQuotaLimits&,
                       target::TimePoint) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
  target::Outcome<std::size_t> prune_windows(target::TimePoint, std::size_t) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
};
class ActionPolicyRepositoryStub final : public target::ActionPolicyRepository {
public:
  target::Outcome<std::optional<target::ActionPolicy>> get(const target::ResourceId&) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
  target::Status upsert(const target::ActionPolicy&,
                        std::optional<target::ResourceVersion>) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
};
class ActionEvidenceRepositoryStub final : public target::ActionEvidenceRepository {
public:
  target::Status append(const target::ActionEvidenceRecord&) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
  target::Outcome<std::vector<target::ActionEvidenceRecord>>
  list(const target::ActionId&) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
  target::Outcome<std::size_t> prune(target::TimePoint, std::size_t) override {
    KIARABOT_P07A_STUB_FAILURE;
  }
};
#undef KIARABOT_P07A_STUB_FAILURE

class RuntimeWork final : public WorkDelegate<target::RuntimeUnitOfWork> {
public:
  explicit RuntimeWork(detail::CommonWorkAccess common)
      : WorkDelegate(std::move(common.work)), accounts_(work(), detail::RecordKind::Account),
        bindings_(work(), detail::RecordKind::AdapterBinding), adapters_(work()),
        credentials_(work()), hosts_(work(), detail::RecordKind::Host),
        installations_(work(), detail::RecordKind::Installation),
        instances_(work(), detail::RecordKind::Instance),
        instance_configs_(work(), detail::RecordKind::InstanceConfig), events_(*common.sql) {}
  target::Repository<target::ResourceId, target::BotAccount>& accounts() override {
    return accounts_;
  }
  target::Repository<target::ResourceId, target::AdapterBindingRecord>&
  adapter_bindings() override {
    return bindings_;
  }
  target::AdapterRegistryRepository& adapters() override {
    return adapters_;
  }
  target::BridgeCredentialRepository& bridge_credentials() override {
    return credentials_;
  }
  target::Outcome<std::size_t>
  active_bindings(const target::InstanceId& adapter_instance) override {
    return detail::active_binding_count(work(), adapter_instance.value);
  }
  target::Repository<target::ResourceId, target::HostRegistration>& hosts() override {
    return hosts_;
  }
  target::Repository<target::InstallationId, target::PluginInstallation>& installations() override {
    return installations_;
  }
  target::Repository<target::InstanceId, target::PluginInstance>& instances() override {
    return instances_;
  }
  target::Repository<target::InstanceId, target::InstanceConfiguration>&
  instance_configs() override {
    return instance_configs_;
  }
  target::Outcome<std::vector<target::BotAccount>>
  list_accounts(std::size_t limit, const std::string& after) override {
    return listed<target::BotAccount>(work(), detail::RecordKind::Account, limit, after);
  }
  target::Outcome<std::vector<target::AdapterRecord>>
  list_adapters(std::size_t limit, const std::string& after) override {
    return listed<target::AdapterRecord>(work(), detail::RecordKind::Adapter, limit, after);
  }
  target::Outcome<std::vector<target::HostRegistration>>
  list_hosts(std::size_t limit, const std::string& after) override {
    return listed<target::HostRegistration>(work(), detail::RecordKind::Host, limit, after);
  }
  target::Outcome<std::vector<target::PluginInstallation>>
  list_installations(std::size_t limit, const std::string& after) override {
    return listed<target::PluginInstallation>(work(), detail::RecordKind::Installation, limit,
                                              after);
  }
  target::Outcome<std::vector<target::PluginInstance>>
  list_instances(std::size_t limit, const std::string& after) override {
    return listed<target::PluginInstance>(work(), detail::RecordKind::Instance, limit, after);
  }
  target::EventRepository& events() override {
    return events_;
  }
  target::ActionRepository& actions() override {
    return actions_;
  }
  target::ActionQuotaRepository& action_quota() override {
    return action_quota_;
  }
  target::ActionPolicyRepository& action_policies() override {
    return action_policies_;
  }
  target::ActionEvidenceRepository& action_evidence() override {
    return action_evidence_;
  }

private:
  LiveRepository<target::ResourceId, target::BotAccount> accounts_;
  LiveRepository<target::ResourceId, target::AdapterBindingRecord> bindings_;
  AdapterRegistry adapters_;
  BridgeCredentials credentials_;
  LiveRepository<target::ResourceId, target::HostRegistration> hosts_;
  LiveRepository<target::InstallationId, target::PluginInstallation> installations_;
  LiveRepository<target::InstanceId, target::PluginInstance> instances_;
  LiveRepository<target::InstanceId, target::InstanceConfiguration> instance_configs_;
  detail::RuntimeEvents events_;
  ActionRepositoryStub actions_;
  ActionQuotaRepositoryStub action_quota_;
  ActionPolicyRepositoryStub action_policies_;
  ActionEvidenceRepositoryStub action_evidence_;
};
class RuntimeFactory final : public target::RuntimeUnitOfWorkFactory {
public:
  explicit RuntimeFactory(std::unique_ptr<Database> database) : database_(std::move(database)) {}
  target::Outcome<std::unique_ptr<target::RuntimeUnitOfWork>> begin() override {
    auto begun = detail::DatabaseAccess::begin(*database_);
    if (const auto* rejected = std::get_if<target::Failure>(&begun)) {
      return *rejected;
    }
    try {
      return std::unique_ptr<target::RuntimeUnitOfWork>(
          std::make_unique<RuntimeWork>(std::move(std::get<detail::CommonWorkAccess>(begun))));
    } catch (...) {
      return failure(target::ErrorCode::Internal);
    }
  }

private:
  std::unique_ptr<Database> database_;
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
  // Owner/due projections are real SQL scans over the private scheduler relations; the closed
  // record kind is reloaded through the same live-record dispatch as Find/Save. Authorization is
  // never decided here.
  target::Outcome<std::vector<target::ScheduledTask>>
  list_tasks(const target::InstanceId& owner, std::size_t limit,
             const std::string& after) override {
    if (limit == 0 || limit > 1000) {
      return failure(target::ErrorCode::InvalidArgument);
    }
    std::vector<std::vector<std::string>> keys;
    const auto status = run_sql([&](pqxx::transaction_base& tx) -> target::Status {
      const auto rows =
          tx.exec("SELECT owner,task_id FROM kiarabot.tasks WHERE owner=$1 AND "
                  "state IN (0,1,2) AND ($2='' OR task_id>$2) ORDER BY task_id LIMIT $3",
                  pqxx::params{owner.value, after, static_cast<int>(limit)});
      for (const auto& row : rows) {
        keys.push_back({row[0].as<std::string>(), row[1].as<std::string>()});
      }
      return std::monostate{};
    });
    if (const auto* rejected = std::get_if<target::Failure>(&status)) {
      return *rejected;
    }
    return load_records<target::ScheduledTask>(work(), detail::RecordKind::Task, keys);
  }
  target::Outcome<std::pair<std::size_t, std::size_t>>
  count_tasks(const target::InstanceId& owner,
              const target::InstallationId& installation) override {
    std::size_t instance = 0;
    std::size_t aggregate = 0;
    const auto status = run_sql([&](pqxx::transaction_base& tx) -> target::Status {
      const auto row =
          tx.exec(
                "SELECT count(*) FILTER (WHERE owner=$1 AND state IN (0,1,2)),"
                " count(*) FILTER (WHERE installation=$2 AND state IN (0,1,2)) FROM kiarabot.tasks",
                pqxx::params{owner.value, installation.value})
              .one_row();
      instance = static_cast<std::size_t>(row[0].as<long long>());
      aggregate = static_cast<std::size_t>(row[1].as<long long>());
      return std::monostate{};
    });
    if (const auto* rejected = std::get_if<target::Failure>(&status)) {
      return *rejected;
    }
    return std::pair<std::size_t, std::size_t>{instance, aggregate};
  }
  target::Outcome<std::vector<target::ScheduledTask>> due_tasks(target::TimePoint run_at,
                                                                std::size_t limit) override {
    if (limit == 0 || limit > 1000) {
      return failure(target::ErrorCode::InvalidArgument);
    }
    std::vector<std::vector<std::string>> keys;
    const auto status = run_sql([&](pqxx::transaction_base& tx) -> target::Status {
      const auto rows =
          tx.exec("SELECT owner,task_id FROM kiarabot.tasks WHERE state=0 AND run_at<=$1 "
                  "ORDER BY run_at LIMIT $2",
                  pqxx::params{run_at.time_since_epoch().count(), static_cast<int>(limit)});
      for (const auto& row : rows) {
        keys.push_back({row[0].as<std::string>(), row[1].as<std::string>()});
      }
      return std::monostate{};
    });
    if (const auto* rejected = std::get_if<target::Failure>(&status)) {
      return *rejected;
    }
    return load_records<target::ScheduledTask>(work(), detail::RecordKind::Task, keys);
  }
  target::Outcome<std::vector<target::Occurrence>>
  list_occurrences(const target::InstanceId& owner, std::size_t limit,
                   const std::string& after) override {
    if (limit == 0 || limit > 1000) {
      return failure(target::ErrorCode::InvalidArgument);
    }
    std::vector<std::vector<std::string>> keys;
    const auto status = run_sql([&](pqxx::transaction_base& tx) -> target::Status {
      const auto rows = tx.exec("SELECT occurrence_id FROM kiarabot.occurrences WHERE owner=$1 AND "
                                "($2='' OR occurrence_id>$2) ORDER BY occurrence_id LIMIT $3",
                                pqxx::params{owner.value, after, static_cast<int>(limit)});
      for (const auto& row : rows) {
        keys.push_back({row[0].as<std::string>()});
      }
      return std::monostate{};
    });
    if (const auto* rejected = std::get_if<target::Failure>(&status)) {
      return *rejected;
    }
    return load_records<target::Occurrence>(work(), detail::RecordKind::Occurrence, keys);
  }
  target::Outcome<std::vector<target::Occurrence>>
  inflight_occurrences(std::size_t limit) override {
    if (limit == 0 || limit > 256) {
      return failure(target::ErrorCode::InvalidArgument);
    }
    std::vector<std::vector<std::string>> keys;
    const auto status = run_sql([&](pqxx::transaction_base& tx) -> target::Status {
      const auto rows = tx.exec("SELECT occurrence_id FROM kiarabot.occurrences WHERE state=$1 "
                                "ORDER BY due_at LIMIT $2",
                                pqxx::params{static_cast<int>(target::OperationState::Accepted),
                                             static_cast<int>(limit)});
      for (const auto& row : rows) {
        keys.push_back({row[0].as<std::string>()});
      }
      return std::monostate{};
    });
    if (const auto* rejected = std::get_if<target::Failure>(&status)) {
      return *rejected;
    }
    return load_records<target::Occurrence>(work(), detail::RecordKind::Occurrence, keys);
  }

private:
  target::Status run_sql(const std::function<target::Status(pqxx::transaction_base&)>& function) {
    auto* sql = dynamic_cast<detail::SqlWork*>(&work());
    if (!sql) {
      return failure(target::ErrorCode::Internal);
    }
    return sql->run(function);
  }
  LiveRepository<target::TaskKey, target::ScheduledTask> tasks_;
  LiveRepository<target::MessageId, target::Occurrence> occurrences_;
};
// P11 Model UoW borrows one physical transaction for its dedicated configuration/secret/call
// repositories and the P03 declaration-level generic relation.
class ModelWork final : public target::ModelUnitOfWork {
public:
  explicit ModelWork(detail::CommonWorkAccess common)
      : common_(std::move(common)), intents_(work(), detail::RecordKind::ModelIntent),
        configs_(detail::model_configuration_repository(*common_.sql)),
        secrets_(detail::provider_secret_repository(*common_.sql)),
        calls_(detail::model_call_repository(*common_.sql)) {}
  target::Service owner() const noexcept override {
    return common_.work->owner();
  }
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
  target::Repository<target::ModelCallKey, target::ModelIntent>& model_intents() override {
    return intents_;
  }
  target::ModelConfigurationRepository& model_configs() override {
    return *configs_;
  }
  target::ProviderSecretRepository& provider_secrets() override {
    return *secrets_;
  }
  target::ModelCallRepository& model_calls() override {
    return *calls_;
  }

private:
  target::UnitOfWork& work() {
    return *common_.work;
  }
  detail::CommonWorkAccess common_;
  LiveRepository<target::ModelCallKey, target::ModelIntent> intents_;
  std::unique_ptr<target::ModelConfigurationRepository> configs_;
  std::unique_ptr<target::ProviderSecretRepository> secrets_;
  std::unique_ptr<target::ModelCallRepository> calls_;
};
class ModelFactory final : public target::ModelUnitOfWorkFactory {
public:
  explicit ModelFactory(std::unique_ptr<Database> database) : database_(std::move(database)) {}
  target::Outcome<std::unique_ptr<target::ModelUnitOfWork>> begin() override {
    auto begun = detail::DatabaseAccess::begin(*database_);
    if (const auto* rejected = std::get_if<target::Failure>(&begun)) {
      return *rejected;
    }
    try {
      return std::unique_ptr<target::ModelUnitOfWork>(
          std::make_unique<ModelWork>(std::move(std::get<detail::CommonWorkAccess>(begun))));
    } catch (...) {
      return failure(target::ErrorCode::Internal);
    }
  }

private:
  std::unique_ptr<Database> database_;
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
        bootstrap_(detail::bootstrap_repository(*common_.sql)),
        invalidations_(detail::invalidation_repository(*common_.sql)) {}
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
  target::IdentityInvalidationRepository* invalidations() noexcept override {
    return invalidations_.get();
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
  std::unique_ptr<target::IdentityInvalidationRepository> invalidations_;
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
  if (config.service != target::Service::Runtime) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  auto opened = Database::open(config);
  if (const auto* rejected = std::get_if<target::Failure>(&opened)) {
    return *rejected;
  }
  try {
    return std::unique_ptr<target::RuntimeUnitOfWorkFactory>(
        std::make_unique<RuntimeFactory>(std::move(std::get<std::unique_ptr<Database>>(opened))));
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}
target::Outcome<std::unique_ptr<target::SchedulerUnitOfWorkFactory>>
open_scheduler(const DatabaseConfig& config) {
  return open_factory<SchedulerWork, target::SchedulerUnitOfWork>(config,
                                                                  target::Service::Scheduler);
}
target::Outcome<std::unique_ptr<target::ModelUnitOfWorkFactory>>
open_model(const DatabaseConfig& config) {
  if (config.service != target::Service::Model) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  auto opened = Database::open(config);
  if (const auto* rejected = std::get_if<target::Failure>(&opened)) {
    return *rejected;
  }
  try {
    return std::unique_ptr<target::ModelUnitOfWorkFactory>(
        std::make_unique<ModelFactory>(std::move(std::get<std::unique_ptr<Database>>(opened))));
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}
} // namespace kiarabot::infrastructure::postgres
