#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>

#include <sys/wait.h>

extern char** environ;

#include <pqxx/pqxx>

#include "kiarabot/core/domain/target/action_rules.hpp"
#include "kiarabot/core/ports/target/business_ports.hpp"
#include "kiarabot/infrastructure/postgres/database.hpp"
#include "kiarabot/infrastructure/postgres/factories.hpp"

namespace pg = kiarabot::infrastructure::postgres;
namespace target = kiarabot::core::target;
using std::chrono_literals::operator""s;

namespace {

class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void require(bool condition, const char* message) {
  if (!condition) {
    throw TestFailure(message);
  }
}
template <typename Value>
bool failed(const target::Outcome<Value>& result, target::ErrorCode code) {
  const auto* failure = std::get_if<target::Failure>(&result);
  return failure && failure->code == code;
}
std::string quote(std::string_view value) {
  std::string output = "'";
  for (const char character : value) {
    if (character == '\'' || character == '\\') {
      output += '\\';
    }
    output += character;
  }
  return output + '\'';
}
int initialize_process(const std::filesystem::path& executable, const pg::DatabaseConfig& config,
                       const std::filesystem::path& password_file) {
  const std::array<std::string, 10> arguments{
      executable.string(),   "--initialize-empty",        "runtime",
      config.host,           std::to_string(config.port), config.database,
      config.migration_role, config.runtime_role,         config.root_certificate,
      password_file.string()};
  std::array<char*, 11> argv{};
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    argv[index] = const_cast<char*>(arguments[index].c_str());
  }
  pid_t process = 0;
  require(::posix_spawn(&process, executable.c_str(), nullptr, nullptr, argv.data(), environ) == 0,
          "initializer process spawn failed");
  int status = 0;
  require(::waitpid(process, &status, 0) == process && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "runtime database initialization failed");
  return WEXITSTATUS(status);
}
target::ActionRequest request(std::string id,
                              target::ActionKind kind = target::ActionKind::SendMessage,
                              std::uint8_t conversation_kind = 1) {
  target::ActionRequest value;
  value.owner_instance = {"instance-owner"};
  value.account_id = {"account-" + id};
  value.adapter_id = {"adapter-" + id};
  value.binding_version = {1};
  value.kind = kind;
  value.idempotency_key = "idem-" + id;
  value.request_hash.bytes[0] = std::byte{1};
  value.serialized_envelope = "envelope-" + id;
  value.target_conversation = "conversation-" + id;
  value.target_conversation_kind = conversation_kind;
  return value;
}
std::vector<target::ActionScope> scopes(const target::ActionRequest& value) {
  return target::action_quota_scopes(value.kind, value.account_id, value.target_conversation,
                                     value.target_conversation_kind);
}
target::ActionRecord admitted(const std::string& id, target::TimePoint now,
                              std::chrono::seconds deadline = 60s) {
  auto value = request(id);
  value.deadline = now + deadline;
  auto record = target::admit_action(target::ActionId{"action-" + id}, value, now);
  if (std::holds_alternative<target::Failure>(record)) {
    throw TestFailure("admit_action failed");
  }
  return std::get<target::ActionRecord>(record);
}
void run(std::uint16_t port, const std::filesystem::path& root,
         const std::filesystem::path& initializer) {
  std::ifstream admin_password_file(root / "password");
  std::string admin_password;
  std::getline(admin_password_file, admin_password);
  require(admin_password.size() == 64, "fixture administrator secret unavailable");
  const auto role_password = [&](const std::string& role) {
    std::ifstream input(root / ("password-" + role));
    std::string value;
    std::getline(input, value);
    require(value.size() == 64, "fixture role secret unavailable");
    return value;
  };
  pqxx::connection administrator(
      "host=localhost port=" + std::to_string(port) + " dbname=postgres user=p03_admin password=" +
      quote(admin_password) + " sslmode=verify-full channel_binding=require sslrootcert=" +
      quote((root / "server.pem").string()));
  {
    pqxx::nontransaction deployment(administrator);
    for (const auto& role : {"p03_runtime_migration", "p03_runtime_runtime"}) {
      deployment.exec("CREATE ROLE " + deployment.quote_name(role) +
                      " LOGIN NOINHERIT NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION "
                      "NOBYPASSRLS PASSWORD " +
                      deployment.quote(role_password(role)));
    }
    deployment.exec("CREATE DATABASE p03_runtime OWNER p03_runtime_migration");
    deployment.exec("REVOKE ALL ON DATABASE p03_runtime FROM PUBLIC");
    deployment.exec("GRANT CONNECT ON DATABASE p03_runtime TO p03_runtime_runtime");
  }
  pg::DatabaseConfig config{target::Service::Runtime,
                            "localhost",
                            port,
                            "p03_runtime",
                            "p03_runtime_runtime",
                            role_password("p03_runtime_runtime"),
                            (root / "server.pem").string(),
                            "p03_runtime_migration",
                            "p03_runtime_runtime"};
  require(initialize_process(initializer, config, root / "password-p03_runtime_migration") == 0,
          "empty Runtime database initialization failed");
  const auto open_factory = [&]() {
    auto opened = pg::open_runtime(config);
    if (std::holds_alternative<target::Failure>(opened)) {
      throw TestFailure("runtime factory open failed");
    }
    return std::move(std::get<std::unique_ptr<target::RuntimeUnitOfWorkFactory>>(opened));
  };
  auto factory = open_factory();
  const auto begin = [&]() {
    auto begun = factory->begin();
    if (std::holds_alternative<target::Failure>(begun)) {
      throw TestFailure("runtime UoW begin failed");
    }
    return std::move(std::get<std::unique_ptr<target::RuntimeUnitOfWork>>(begun));
  };
  const auto now = [&](const std::unique_ptr<target::RuntimeUnitOfWork>& work) {
    auto sample = work->current_time();
    if (std::holds_alternative<target::Failure>(sample)) {
      throw TestFailure("authoritative database time unavailable");
    }
    return std::get<target::TimePoint>(sample);
  };
  // --- insert/find/idempotency/capacity ---
  {
    auto work = begin();
    const auto sample = now(work);
    const auto record = admitted("a", sample);
    require(std::holds_alternative<target::ActionRecord>(
                work->actions().insert(record, target::ActionCapacity{})),
            "admitted action inserts");
    require(work->commit() == target::CommitResult::Committed, "action insert commits");
  }
  {
    auto work = begin();
    require(std::get<std::optional<target::ActionRecord>>(work->actions().find_by_id({"action-a"}))
                .has_value(),
            "find by id returns row");
    auto by_key = work->actions().find_by_owner_key({"instance-owner"}, "idem-a");
    require(std::holds_alternative<std::optional<target::ActionRecord>>(by_key) &&
                std::get<std::optional<target::ActionRecord>>(by_key).has_value(),
            "find by owner idempotency key");
  }
  {
    auto work = begin();
    const auto sample = now(work);
    auto duplicate = work->actions().insert(admitted("a", sample), target::ActionCapacity{});
    require(failed(duplicate, target::ErrorCode::Conflict), "unique owner/key conflict");
  }
  {
    auto work = begin();
    const auto sample = now(work);
    auto limited = work->actions().insert(admitted("b", sample), target::ActionCapacity{1, 100000});
    require(failed(limited, target::ErrorCode::RateLimited), "pending capacity rejection");
  }
  // --- quota windows and daily budget ---
  {
    auto limits = target::ActionQuotaLimits{};
    limits.account = {2, 60s};
    limits.daily_budget[0] = 100;
    auto work = begin();
    const auto sample = now(work);
    const auto first = request("q1");
    require(std::holds_alternative<target::ActionRecord>(
                work->actions().insert(admitted("q1", sample), target::ActionCapacity{})),
            "quota action inserts");
    require(std::holds_alternative<std::monostate>(work->action_quota().admit(
                {"action-q1"}, first.kind, scopes(first), limits, sample)),
            "first quota admission");
    require(work->commit() == target::CommitResult::Committed, "quota commit");
  }
  {
    auto work = begin();
    const auto sample = now(work);
    auto second = request("q2", target::ActionKind::SendMessage);
    second.account_id = {"account-q1"};
    require(std::holds_alternative<target::ActionRecord>(
                work->actions().insert(admitted("q2", sample), target::ActionCapacity{})),
            "second quota action inserts");
    auto limits = target::ActionQuotaLimits{};
    limits.account = {2, 60s};
    limits.daily_budget[0] = 100;
    require(std::holds_alternative<std::monostate>(work->action_quota().admit(
                {"action-q2"}, second.kind, scopes(second), limits, sample)),
            "second quota admission");
    require(work->commit() == target::CommitResult::Committed, "second quota commit");
  }
  {
    auto work = begin();
    const auto sample = now(work);
    auto third = request("q3");
    third.account_id = {"account-q1"};
    require(std::holds_alternative<target::ActionRecord>(
                work->actions().insert(admitted("q3", sample), target::ActionCapacity{})),
            "third quota action inserts");
    auto limits = target::ActionQuotaLimits{};
    limits.account = {2, 60s};
    limits.daily_budget[0] = 100;
    auto denied =
        work->action_quota().admit({"action-q3"}, third.kind, scopes(third), limits, sample);
    require(failed(denied, target::ErrorCode::RateLimited), "sliding window rejects over limit");
  }
  {
    auto work = begin();
    const auto sample = now(work);
    auto fourth = request("q4", target::ActionKind::ReactMessage);
    fourth.account_id = {"account-q4"};
    fourth.idempotency_key = "idem-q4-daily";
    require(std::holds_alternative<target::ActionRecord>(
                work->actions().insert(admitted("q4", sample), target::ActionCapacity{})),
            "daily action inserts");
    auto limits = target::ActionQuotaLimits{};
    limits.account = {100, 60s};
    limits.daily_budget[1] = 1;
    require(std::holds_alternative<std::monostate>(work->action_quota().admit(
                {"action-q4"}, fourth.kind, scopes(fourth), limits, sample)),
            "daily first admission");
    require(work->commit() == target::CommitResult::Committed, "daily commit");
  }
  {
    auto work = begin();
    const auto sample = now(work);
    auto fifth = request("q5", target::ActionKind::ReactMessage);
    fifth.account_id = {"account-q4"};
    fifth.idempotency_key = "idem-q5-daily";
    require(std::holds_alternative<target::ActionRecord>(
                work->actions().insert(admitted("q5", sample), target::ActionCapacity{})),
            "daily over action inserts");
    auto limits = target::ActionQuotaLimits{};
    limits.account = {100, 60s};
    limits.daily_budget[1] = 1;
    require(
        failed(work->action_quota().admit({"action-q5"}, fifth.kind, scopes(fifth), limits, sample),
               target::ErrorCode::RateLimited),
        "daily budget rejects over limit");
  }
  // --- dispatch, terminal, duplicate/conflict/late evidence ---
  {
    auto work = begin();
    const auto sample = now(work);
    require(std::holds_alternative<std::monostate>(work->actions().mark_dispatched(
                {"action-a"}, {"attempt-a"}, {"adapter-a"}, sample)),
            "first dispatch");
    require(work->commit() == target::CommitResult::Committed, "dispatch commit");
  }
  {
    auto work = begin();
    const auto sample = now(work);
    require(
        failed(work->actions().mark_dispatched({"action-a"}, {"attempt-b"}, {"adapter-a"}, sample),
               target::ErrorCode::Conflict),
        "second dispatch conflicts");
  }
  {
    auto work = begin();
    const auto sample = now(work);
    auto current = work->actions().find_by_id({"action-a"});
    require(std::holds_alternative<std::optional<target::ActionRecord>>(current) &&
                std::get<std::optional<target::ActionRecord>>(current).has_value(),
            "dispatched record found");
    const auto record = *std::get<std::optional<target::ActionRecord>>(current);
    target::ActionTerminalResult success;
    success.state = target::ActionState::Succeeded;
    success.execution_outcome = target::ExecutionOutcome::KnownSucceeded;
    success.platform_message_id = "platform-a";
    auto recorded = work->actions().record_terminal(
        record, success, std::optional<target::AttemptId>{target::AttemptId{"attempt-a"}},
        std::optional<target::InstanceId>{target::InstanceId{"adapter-a"}}, sample);
    require(std::holds_alternative<target::ActionTerminalReceipt>(recorded) &&
                std::get<target::ActionTerminalReceipt>(recorded).disposition ==
                    target::ActionReceiptDisposition::Recorded,
            "terminal recorded");
    auto duplicate = work->actions().record_terminal(
        record, success, std::optional<target::AttemptId>{target::AttemptId{"attempt-a"}},
        std::optional<target::InstanceId>{target::InstanceId{"adapter-a"}}, sample);
    require(std::holds_alternative<target::ActionTerminalReceipt>(duplicate) &&
                std::get<target::ActionTerminalReceipt>(duplicate).disposition ==
                    target::ActionReceiptDisposition::Duplicate,
            "duplicate terminal");
    target::ActionTerminalResult failure_result;
    failure_result.state = target::ActionState::Failed;
    failure_result.execution_outcome = target::ExecutionOutcome::KnownFailed;
    failure_result.error_code = target::ErrorCode::UpstreamUnavailable;
    auto conflict_result = work->actions().record_terminal(
        record, failure_result, std::optional<target::AttemptId>{target::AttemptId{"attempt-a"}},
        std::optional<target::InstanceId>{target::InstanceId{"adapter-a"}}, sample);
    require(std::holds_alternative<target::ActionTerminalReceipt>(conflict_result) &&
                std::get<target::ActionTerminalReceipt>(conflict_result).disposition ==
                    target::ActionReceiptDisposition::Conflict &&
                std::get<target::ActionTerminalReceipt>(conflict_result).evidence_appended,
            "conflicting terminal appends evidence");
    auto evidence = work->action_evidence().list({"action-a"});
    require(std::holds_alternative<std::vector<target::ActionEvidenceRecord>>(evidence) &&
                !std::get<std::vector<target::ActionEvidenceRecord>>(evidence).empty(),
            "conflict evidence listed");
    require(work->commit() == target::CommitResult::Committed, "terminal commit");
  }
  // --- cancel / expire / late evidence ---
  {
    auto work = begin();
    const auto sample = now(work);
    auto action = admitted("cancelled", sample);
    require(std::holds_alternative<target::ActionRecord>(
                work->actions().insert(action, target::ActionCapacity{})),
            "cancellable action inserts");
    require(std::holds_alternative<std::monostate>(
                work->actions().cancel({"action-cancelled"}, sample)),
            "cancel accepted action");
    require(std::holds_alternative<std::monostate>(
                work->actions().cancel({"action-cancelled"}, sample)),
            "cancel is idempotent");
    require(work->commit() == target::CommitResult::Committed, "cancel commit");
  }
  {
    auto work = begin();
    const auto sample = now(work);
    auto record = admitted("expired", sample, -1s);
    record.state = target::ActionState::Accepted;
    record.execution_outcome = target::ExecutionOutcome::Unknown;
    record.retain_until = target::TimePoint{};
    auto inserted = work->actions().insert(record, target::ActionCapacity{});
    require(std::holds_alternative<target::ActionRecord>(inserted), "expired candidate inserts");
    auto expired = work->actions().expire_due(10, sample);
    require(std::holds_alternative<std::vector<target::ActionRecord>>(expired) &&
                std::get<std::vector<target::ActionRecord>>(expired).size() == 1,
            "due expiry claims action");
    require(work->commit() == target::CommitResult::Committed, "expiry commit");
  }
  {
    auto work = begin();
    const auto sample = now(work);
    auto record = admitted("late", sample, 1s);
    require(std::holds_alternative<target::ActionRecord>(
                work->actions().insert(record, target::ActionCapacity{})),
            "late action inserts");
    require(std::holds_alternative<std::monostate>(work->actions().mark_dispatched(
                {"action-late"}, {"attempt-late"}, {"adapter-late"}, sample)),
            "late action dispatches");
    require(work->commit() == target::CommitResult::Committed, "late dispatch commit");
    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    auto later = begin();
    const auto later_now = now(later);
    auto expired = later->actions().expire_due(10, later_now);
    require(std::holds_alternative<std::vector<target::ActionRecord>>(expired),
            "late action expires");
    require(later->commit() == target::CommitResult::Committed, "late expiry commit");
    auto terminal_work = begin();
    const auto terminal_now = now(terminal_work);
    const auto current = terminal_work->actions().find_by_id({"action-late"});
    require(std::holds_alternative<std::optional<target::ActionRecord>>(current) &&
                std::get<std::optional<target::ActionRecord>>(current).has_value(),
            "expired record found");
    target::ActionTerminalResult success;
    success.state = target::ActionState::Succeeded;
    success.execution_outcome = target::ExecutionOutcome::KnownSucceeded;
    auto late = terminal_work->actions().record_terminal(
        *std::get<std::optional<target::ActionRecord>>(current), success,
        std::optional<target::AttemptId>{target::AttemptId{"attempt-late"}},
        std::optional<target::InstanceId>{target::InstanceId{"adapter-late"}}, terminal_now);
    require(std::holds_alternative<target::ActionTerminalReceipt>(late) &&
                std::get<target::ActionTerminalReceipt>(late).disposition ==
                    target::ActionReceiptDisposition::EvidenceOnly &&
                std::get<target::ActionTerminalReceipt>(late).evidence_appended,
            "late terminal is evidence only");
    require(terminal_work->commit() == target::CommitResult::Committed, "late evidence commit");
  }
  // --- list owner/control, pagination, policy, evidence, prune, restart ---
  {
    auto work = begin();
    auto owner = work->actions().list_owner({"instance-owner"}, {5, "action-"});
    require(std::holds_alternative<std::vector<target::ActionRecord>>(owner) &&
                !std::get<std::vector<target::ActionRecord>>(owner).empty(),
            "owner list");
    auto control = target::ActionControlQuery{
        {target::ResourceRef{target::ResourceKind::Account, {"account-a"}}, "action.inspect"},
        target::ActionState::Succeeded,
        target::ActionKind::SendMessage,
        {10, ""}};
    auto listed = work->actions().list_control(control);
    require(std::holds_alternative<std::vector<target::ActionRecord>>(listed) &&
                std::get<std::vector<target::ActionRecord>>(listed).size() == 1,
            "control list filters account/state/kind");
    auto missing = target::ActionControlQuery{
        {target::ResourceRef{target::ResourceKind::Account, {"account-missing"}}, "action.inspect"},
        std::nullopt,
        std::nullopt,
        {10, ""}};
    require(std::holds_alternative<std::vector<target::ActionRecord>>(
                work->actions().list_control(missing)) &&
                std::get<std::vector<target::ActionRecord>>(work->actions().list_control(missing))
                    .empty(),
            "control list empty page");
    work->rollback();
  }
  {
    auto work = begin();
    const auto sample = now(work);
    auto absent = work->action_policies().get({"account-policy"});
    require(std::holds_alternative<std::optional<target::ActionPolicy>>(absent) &&
                !std::get<std::optional<target::ActionPolicy>>(absent).has_value(),
            "policy absent");
    target::ActionPolicy policy;
    policy.account_id = {"account-policy"};
    policy.mute_enabled = true;
    policy.kick_enabled = true;
    policy.allowed_group_ids = {"group-1", "group-2"};
    policy.mute_daily_budget = 5;
    policy.kick_daily_budget = 6;
    policy.version = {1};
    policy.updated_at = sample;
    require(std::holds_alternative<std::monostate>(
                work->action_policies().upsert(policy, std::nullopt)),
            "policy create");
    policy.version = {2};
    policy.updated_at = sample;
    require(std::holds_alternative<std::monostate>(
                work->action_policies().upsert(policy, target::ResourceVersion{1})),
            "policy update");
    auto fetched = work->action_policies().get({"account-policy"});
    require(std::holds_alternative<std::optional<target::ActionPolicy>>(fetched), "policy fetched");
    const auto& stored = std::get<std::optional<target::ActionPolicy>>(fetched);
    require(stored && stored->version.value == 2 && stored->allowed_group_ids.size() == 2 &&
                stored->mute_daily_budget == 5 && stored->kick_daily_budget == 6,
            "policy round trip");
    policy.version = {3};
    require(failed(work->action_policies().upsert(policy, target::ResourceVersion{1}),
                   target::ErrorCode::Conflict),
            "stale policy update conflicts");
    work->rollback();
  }
  {
    auto work = begin();
    const auto sample = now(work);
    target::ActionEvidenceRecord evidence;
    evidence.action_id = {"action-a"};
    evidence.dispatch_attempt_id = {"attempt-a"};
    evidence.adapter_instance = {"adapter-a"};
    evidence.kind = target::ActionEvidenceKind::Unknown;
    evidence.execution_outcome = target::ExecutionOutcome::Unknown;
    evidence.observed_at = sample;
    evidence.received_at = sample;
    evidence.retain_until = sample + target::kActionEvidenceRetention;
    require(std::holds_alternative<std::monostate>(work->action_evidence().append(evidence)),
            "evidence append");
    auto listed = work->action_evidence().list({"action-a"});
    require(std::holds_alternative<std::vector<target::ActionEvidenceRecord>>(listed) &&
                !std::get<std::vector<target::ActionEvidenceRecord>>(listed).empty(),
            "evidence list");
    require(work->commit() == target::CommitResult::Committed, "evidence commit");
  }
  {
    auto work = begin();
    const auto sample = now(work);
    auto record = admitted("prune", sample);
    record.state = target::ActionState::Succeeded;
    record.execution_outcome = target::ExecutionOutcome::KnownSucceeded;
    record.completed_at = sample;
    record.retain_until = sample - 1s;
    require(std::holds_alternative<target::ActionRecord>(
                work->actions().insert(record, target::ActionCapacity{})),
            "prunable action inserts");
    auto pruned = work->actions().prune_terminal(sample, 10);
    require(std::holds_alternative<std::size_t>(pruned) && std::get<std::size_t>(pruned) >= 1,
            "terminal prune");
    require(work->commit() == target::CommitResult::Committed, "prune commit");
  }
  {
    auto work = begin();
    const auto sample = now(work);
    auto pruned = work->action_evidence().prune(sample + std::chrono::hours{200}, 10);
    require(std::holds_alternative<std::size_t>(pruned), "evidence prune");
    require(work->commit() == target::CommitResult::Committed, "evidence prune commit");
  }
  // Restart: release the writer session, then a new factory observes committed rows only.
  {
    factory.reset();
    auto second = open_factory();
    auto begun = second->begin();
    require(std::holds_alternative<std::unique_ptr<target::RuntimeUnitOfWork>>(begun),
            "restart factory begins");
    auto work = std::move(std::get<std::unique_ptr<target::RuntimeUnitOfWork>>(begun));
    auto persisted = work->actions().find_by_id({"action-a"});
    require(std::holds_alternative<std::optional<target::ActionRecord>>(persisted) &&
                std::get<std::optional<target::ActionRecord>>(persisted).has_value() &&
                std::get<std::optional<target::ActionRecord>>(persisted)->state ==
                    target::ActionState::Succeeded,
            "committed action survives restart");
    work->rollback();
  }
  std::cout << "P07 Runtime Action typed PostgreSQL repositories passed\n";
}

} // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 5, "fixture arguments required");
    const auto port = std::stoul(argv[2]);
    require(port > 0 && port <= 65535, "invalid fixture port");
    run(static_cast<std::uint16_t>(port), std::filesystem::canonical(argv[1]),
        std::filesystem::canonical(argv[3]));
    return 0;
  } catch (const TestFailure& error) {
    std::cerr << "P07 assertion: " << error.what() << '\n';
    return 1;
  } catch (const std::exception&) {
    std::cerr << "P07 Runtime Action typed storage test failed (details redacted)\n";
    return 1;
  }
}
