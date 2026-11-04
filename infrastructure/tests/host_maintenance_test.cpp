#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/wait.h>

#include "kiarabot/core/application/target/host_maintenance.hpp"
#include "kiarabot/core/application/target/host_service.hpp"
#include "kiarabot/core/runtime/platform/host_process.hpp"
#include "kiarabot/infrastructure/host/host_journal.hpp"
#include "kiarabot/infrastructure/host/host_maintenance_sqlite.hpp"

namespace {
namespace t = kiarabot::core::target;
namespace h = kiarabot::infrastructure::host;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
constexpr std::string_view kHost = "host-1";
constexpr std::string_view kInstance = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view kInstallation = "22222222-2222-4222-8222-222222222222";

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string{message});
  }
}
template <typename V> V take(t::Outcome<V> result) {
  require(std::holds_alternative<V>(result), "expected successful Host maintenance result");
  return std::move(std::get<V>(result));
}
template <typename V> void rejects(const t::Outcome<V>& result, t::ErrorCode code) {
  require(std::holds_alternative<t::Failure>(result) && std::get<t::Failure>(result).code == code,
          "expected stable Host maintenance refusal");
}
struct Temporary {
  fs::path root;
  Temporary() {
    auto pattern =
        (fs::canonical(fs::temp_directory_path()) / "kiarabot-maintenance-XXXXXX").string();
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    require(::mkdtemp(bytes.data()) != nullptr, "temporary Host database directory");
    root = bytes.data();
  }
  ~Temporary() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
};
struct Clock final : t::HostClock {
  t::TimePoint current{std::chrono::seconds{10000}};
  t::TimePoint now() const override {
    return current;
  }
};
struct Authorization final : t::HostMaintenanceAuthorizationPort {
  bool allowed = true;
  std::optional<t::ErrorCode> fault;
  int calls = 0;
  t::Permission last = t::Permission::Unspecified;
  t::Outcome<t::ResourceVersion> check(const t::AuthenticationContext&, t::Permission permission,
                                       const t::ResourceRef& scope, t::TimePoint) override {
    ++calls;
    last = permission;
    require(scope == t::ResourceRef{t::ResourceKind::Host, {std::string{kHost}}},
            "exact own Host authorization only");
    if (fault) {
      return t::Failure{*fault, t::Acceptance::NotAccepted, t::Recovery::ReadCurrent, {}, {}};
    }
    if (!allowed) {
      return t::Failure{t::ErrorCode::PermissionDenied,
                        t::Acceptance::NotAccepted,
                        t::Recovery::ReadCurrent,
                        {},
                        {}};
    }
    return t::ResourceVersion{7};
  }
};
// Only a local typed model. It intentionally does not assert mTLS/Identity/Runtime backend
// coverage.
struct Gate final : t::RuntimeAdmissionGatePort {
  std::uint64_t version = 0;
  int requests = 0;
  int effects = 0;
  bool lose_ack_once = false;
  bool unavailable = false;
  std::map<std::string, t::HostAdmissionGateReceipt> receipts;
  t::Outcome<t::HostAdmissionGateReceipt>
  set_admission(const t::HostAdmissionGateRequest& request) override {
    ++requests;
    if (unavailable) {
      return t::Failure{t::ErrorCode::UpstreamUnavailable,
                        t::Acceptance::NotAccepted,
                        t::Recovery::ReadCurrent,
                        {},
                        {}};
    }
    const auto key = request.operation.id.value;
    const auto old = receipts.find(key);
    if (old != receipts.end()) {
      if (old->second.maintenance_version != request.maintenance_version ||
          old->second.admission_open != request.admission_open) {
        return t::Failure{
            t::ErrorCode::Conflict, t::Acceptance::NotAccepted, t::Recovery::ReadCurrent, {}, {}};
      }
      return old->second;
    }
    if (request.expected_gate_version != version) {
      return t::Failure{
          t::ErrorCode::Conflict, t::Acceptance::NotAccepted, t::Recovery::ReadCurrent, {}, {}};
    }
    ++effects;
    t::HostAdmissionGateReceipt receipt{request.operation, request.maintenance_version, ++version,
                                        request.admission_open};
    receipts.emplace(key, receipt);
    if (lose_ack_once) {
      lose_ack_once = false;
      return t::Failure{t::ErrorCode::UpstreamUnavailable,
                        t::Acceptance::Unknown,
                        t::Recovery::QuerySameIdentity,
                        {},
                        {}};
    }
    return receipt;
  }
};
t::ManagedExecution execution() {
  t::ManagedExecution value;
  value.host_id = kHost;
  value.instance_id = kInstance;
  value.installation_id = kInstallation;
  value.execution_generation = 1;
  value.desired_version = 1;
  value.pid = 123;
  value.process_marker = "fixed-process-marker";
  value.state = t::ManagedExecutionState::Running;
  return value;
}
struct Executions final : t::HostMaintenanceExecutionPort {
  std::vector<t::ManagedExecution> values{execution()};
  int stops = 0;
  int terminations = 0;
  bool unknown = false;
  t::Outcome<std::vector<t::ManagedExecution>> maintenance_executions() override {
    std::vector<t::ManagedExecution> active;
    for (const auto& value : values) {
      if (value.state != t::ManagedExecutionState::Exited ||
          value.outcome == t::ExecutionOutcome::Unknown) {
        active.push_back(value);
      }
    }
    return active;
  }
  t::Outcome<t::ManagedExecution> stop_for_maintenance(const t::ManagedExecution& target,
                                                       std::uint32_t grace,
                                                       std::string_view) override {
    ++stops;
    require(grace <= 10000, "bounded maintenance grace");
    for (auto& value : values) {
      if (value.instance_id != target.instance_id ||
          value.execution_generation != target.execution_generation ||
          value.process_marker != target.process_marker) {
        continue;
      }
      if (value.state != t::ManagedExecutionState::Exited) {
        ++terminations;
      }
      value.state = t::ManagedExecutionState::Exited;
      value.outcome = unknown ? t::ExecutionOutcome::Unknown : t::ExecutionOutcome::KnownSucceeded;
      return value;
    }
    return t::Failure{
        t::ErrorCode::Conflict, t::Acceptance::Unknown, t::Recovery::QuerySameIdentity, {}, {}};
  }
};
t::AuthenticationContext authentication(Clock& clock) {
  return {{"operator-1"},
          t::PlatformPeer{t::Service::Control, {"control-1"}},
          t::Service::Host,
          {},
          {},
          {1},
          clock.now() - 1s,
          clock.now() + 60s,
          "request-local-proof-never-persisted"};
}
t::HostMaintenanceRequest command(Clock& clock, std::string id = "operation-1",
                                  std::uint64_t expected = 1) {
  return {authentication(clock), std::move(id), "trace-1", "maintenance-idempotency-1",
          std::string{kHost},    expected,      10,        clock.now() + 5s};
}
std::unique_ptr<t::HostMaintenanceStore>
store(const fs::path& path, Clock& clock, h::HostMaintenanceStorageLimits limits = {},
      h::HostMaintenanceCommitCheckpoint checkpoint = nullptr) {
  return take(h::make_sqlite_host_maintenance(path.string(), {std::string{kHost}}, "initial-epoch",
                                              clock, limits, checkpoint));
}
t::HostMaintenanceApplication application(t::HostMaintenanceStore& store, Executions& executions,
                                          Gate& gate, Authorization& auth, Clock& clock) {
  return {{std::string{kHost}}, store, executions, gate, auth, clock};
}
t::OperationObservationQuery query() {
  return {{{t::ResourceKind::Host, {std::string{kHost}}}}, {}, 0};
}
std::uint64_t head(t::HostMaintenanceStore& store) {
  return take(store.query_operation_changes(query())).next_cursor.sequence;
}
t::HostMaintenanceOperation finish(t::HostMaintenanceApplication& app, const t::OperationId& id) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    auto value = take(app.advance(id));
    if (value.operation.state == t::OperationState::Succeeded ||
        value.operation.state == t::OperationState::Blocked) {
      return value;
    }
  }
  throw std::runtime_error("Host operation failed to converge in bounded fixture");
}
void sql(const fs::path& path, const char* statement) {
  sqlite3* database = nullptr;
  require(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "open fault fixture connection");
  const auto result = sqlite3_exec(database, statement, nullptr, nullptr, nullptr);
  sqlite3_close(database);
  require(result == SQLITE_OK, "execute fixed SQL fault fixture");
}

void happy_cancel_and_replay() {
  Temporary temporary;
  Clock clock;
  Authorization auth;
  Executions executions;
  Gate gate;
  auto db = store(temporary.root / "host.sqlite", clock);
  auto app = application(*db, executions, gate, auth, clock);
  auto request = command(clock);
  const auto first = take(app.begin(request));
  const auto accepted_head = head(*db);
  auto retry = request;
  retry.request_id = "other-request";
  require(take(app.begin(retry)).operation.ref.id == first.operation.ref.id &&
              head(*db) == accepted_head,
          "same-key replay retains operation identity and creates no fake change");
  retry.grace_ms = 11;
  rejects(app.begin(retry), t::ErrorCode::Conflict);
  auto cancel = command(clock, "cancel-request");
  cancel.idempotency_key = "cancel-idempotency-1";
  const auto cancelled = take(app.cancel({cancel, first.operation.ref.id}));
  require(cancelled.operation.state == t::OperationState::Cancelled && take(db->allows_start()),
          "pre-effect cancellation leaves admission open");
  const auto cancelled_head = head(*db);
  require(take(app.cancel({cancel, first.operation.ref.id})).operation.version ==
                  cancelled.operation.version &&
              head(*db) == cancelled_head,
          "cancel replay preserves receipt without another change");
  request = command(clock, "operation-2", take(db->maintenance_state()).version.value);
  request.idempotency_key = "maintenance-idempotency-2";
  auto accepted = take(app.begin(request));
  const auto closed = take(app.advance(accepted.operation.ref.id));
  require(!take(db->allows_start()) && gate.requests == 0 &&
              closed.phase == t::HostMaintenancePhase::Closing,
          "local gate closes durably before Runtime work");
  cancel.expected_version = closed.operation.version.value;
  cancel.idempotency_key = "cancel-idempotency-2";
  rejects(app.cancel({cancel, accepted.operation.ref.id}), t::ErrorCode::Conflict);
  require(finish(app, accepted.operation.ref.id).operation.state == t::OperationState::Succeeded &&
              executions.terminations == 1,
          "known finite execution completion succeeds once");
  auto resume = command(clock, "resume-1", take(db->maintenance_state()).version.value);
  resume.idempotency_key = "resume-idempotency-1";
  auto resumed = take(app.resume(resume));
  require(finish(app, resumed.operation.ref.id).operation.state == t::OperationState::Succeeded &&
              take(db->allows_start()),
          "explicit resume waits for Runtime then opens local admission");
  const auto resume_head = head(*db);
  require(take(app.resume(resume)).operation.state == t::OperationState::Succeeded &&
              head(*db) == resume_head,
          "resume replay remains valid after its original effect");
  auth.allowed = false;
  rejects(app.begin(request), t::ErrorCode::PermissionDenied);
  rejects(app.operation(authentication(clock), clock.now() + 1s, accepted.operation.ref.id),
          t::ErrorCode::PermissionDenied);
  require(head(*db) == resume_head, "revoked caller cannot mutate through replay");
  auth.allowed = true;
  auth.fault = t::ErrorCode::UpstreamUnavailable;
  rejects(app.state(authentication(clock), clock.now() + 1s), t::ErrorCode::UpstreamUnavailable);
  auto wrong = authentication(clock);
  wrong.audience = t::Service::Runtime;
  rejects(app.state(wrong, clock.now() + 1s), t::ErrorCode::Unauthenticated);
}

void unknown_gate_and_process() {
  Temporary temporary;
  Clock clock;
  Authorization auth;
  Executions executions;
  Gate gate;
  const auto path = temporary.root / "host.sqlite";
  auto db = store(path, clock);
  auto app = application(*db, executions, gate, auth, clock);
  auto accepted = take(app.begin(command(clock)));
  (void)take(app.advance(accepted.operation.ref.id));
  gate.lose_ack_once = true;
  const auto unknown = take(app.advance(accepted.operation.ref.id));
  require(unknown.operation.state == t::OperationState::Blocked && gate.effects == 1 &&
              executions.stops == 0 && !take(db->allows_start()),
          "lost gate ACK never starts process drain or opens admission");
  // Runtime-side typed fixture survives a recreated Host store/application, as a remote owner
  // would.
  auto reopened = store(path, clock);
  auto after_restart = application(*reopened, executions, gate, auth, clock);
  require(finish(after_restart, accepted.operation.ref.id).operation.state ==
                  t::OperationState::Succeeded &&
              gate.effects == 1,
          "original gate identity recovers without a second Runtime mutation");
  auto resume = command(clock, "resume-unknown", take(reopened->maintenance_state()).version.value);
  resume.idempotency_key = "resume-unknown-key";
  auto unknown_execution = execution();
  unknown_execution.state = t::ManagedExecutionState::Exited;
  unknown_execution.outcome = t::ExecutionOutcome::Unknown;
  executions.values = {unknown_execution};
  auto pending = take(after_restart.resume(resume));
  const auto blocked = take(after_restart.advance(pending.operation.ref.id));
  require(blocked.operation.state == t::OperationState::Blocked && !take(reopened->allows_start()),
          "unknown exited execution prevents resume");
}

void changes_atomicity_and_bounds() {
  Temporary temporary;
  Clock clock;
  Authorization auth;
  Executions executions;
  Gate gate;
  auto db = store(temporary.root / "host.sqlite", clock, {3, 65536});
  auto app = application(*db, executions, gate, auth, clock);
  auto initial = take(db->query_operation_changes(query()));
  require(initial.changes.empty() && initial.next_cursor.sequence == 0,
          "head capture is not historical replay");
  auto accepted = take(app.begin(command(clock)));
  auto request = query();
  request.cursor = initial.next_cursor;
  request.page_size = 1;
  const auto page = take(db->query_operation_changes(request));
  require(page.changes.size() == 1 &&
              page.changes.front().operation.ref.owner == t::Service::Host &&
              page.changes.front().cursor.sequence == 1,
          "committed Host changes use shared owner cursor");
  const auto before = head(*db);
  sql(temporary.root / "host.sqlite",
      "CREATE TRIGGER reject_host_progress BEFORE UPDATE ON host_maintenance_operations BEGIN "
      "SELECT RAISE(ABORT,'fixed fault'); END;");
  const auto rejected = app.advance(accepted.operation.ref.id);
  rejects(rejected, t::ErrorCode::Internal);
  require(head(*db) == before && take(db->allows_start()),
          "failed progress transaction rolls back admission and change append together");
  sql(temporary.root / "host.sqlite", "DROP TRIGGER reject_host_progress;");
  require(finish(app, accepted.operation.ref.id).operation.state == t::OperationState::Succeeded,
          "retry after local rollback converges");
  rejects(db->query_operation_changes(request), t::ErrorCode::Conflict);
  request = query();
  request.cursor = take(db->query_operation_changes(query())).next_cursor;
  auto reopen = take(h::make_sqlite_host_maintenance(
      (temporary.root / "host.sqlite").string(), {std::string{kHost}},
      "different-process-epoch-seed", clock, {3, 65536}));
  require(take(reopen->query_operation_changes(request)).changes.empty(),
          "ordinary restart retains dataset epoch and cursor");
  auto invalid_scope = query();
  invalid_scope.scope = {{t::ResourceKind::Instance, {"other"}}};
  rejects(reopen->query_operation_changes(invalid_scope), t::ErrorCode::InvalidArgument);
  auto future = request;
  ++future.cursor->sequence;
  rejects(reopen->query_operation_changes(future), t::ErrorCode::Conflict);
  clock.current += 601s;
  auto too_old = request;
  too_old.cursor->sequence = request.cursor->sequence - 1;
  rejects(reopen->query_operation_changes(too_old), t::ErrorCode::Conflict);
  require(take(reopen->prune_operation_changes(1000)) > 0,
          "bounded retention advances persistent low-water");
  (void)take(reopen->rotate_observation_epoch("rebuilt-epoch"));
  rejects(reopen->query_operation_changes(request), t::ErrorCode::Conflict);
  require(take(reopen->query_operation_changes(query())).next_cursor.sequence == 0,
          "trusted rebuild uses a new dataset epoch");
}

void crash_after_commit() {
  ::_exit(95);
}
void commit_response_loss() {
  Temporary temporary;
  Clock clock;
  const auto path = temporary.root / "host.sqlite";
  const auto child = ::fork();
  require(child >= 0, "fork Host commit crash fixture");
  if (child == 0) {
    auto db = store(path, clock, {}, crash_after_commit);
    Authorization auth;
    Executions executions;
    Gate gate;
    auto app = application(*db, executions, gate, auth, clock);
    (void)app.begin(command(clock));
    ::_exit(111);
  }
  int status = 0;
  require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 95,
          "actual exit after Host local COMMIT before response");
  auto db = store(path, clock);
  Authorization auth;
  Executions executions;
  Gate gate;
  auto app = application(*db, executions, gate, auth, clock);
  require(head(*db) == 1, "accepted operation and change survive crash together");
  auto replay = command(clock, "request-after-crash");
  auto value = take(app.begin(replay));
  require(value.operation.ref.id.value == "operation-1" && head(*db) == 1,
          "commit ambiguity recovers original identity without duplicate change");
  require(finish(app, value.operation.ref.id).operation.state == t::OperationState::Succeeded,
          "restart continues original accepted intent");
}

struct Bridge final : t::HostBridgeTransport {
  bool active = false;
  t::Status start(t::HostBridgeHandler&) override {
    active = true;
    return std::monostate{};
  }
  void stop() override {
    active = false;
  }
  bool registered() const override {
    return active;
  }
  t::Status send_observation(const t::ExecutionObservation&) override {
    return std::monostate{};
  }
  t::Status send_receipt(const t::HostReceipt&) override {
    return std::monostate{};
  }
  std::uint64_t delivered_sequence() const override {
    return 0;
  }
};
void real_managed_process(const char* executable) {
  Temporary temporary;
  Clock clock;
  Authorization auth;
  Gate gate;
  Bridge bridge;
  const auto path = temporary.root / "host.sqlite";
  auto maintenance = store(path, clock);
  auto journal = h::make_sqlite_host_journal(path.string());
  struct ObservedSupervisor final : t::ProcessSupervisor {
    std::unique_ptr<t::ProcessSupervisor> delegate = t::make_posix_process_supervisor();
    t::Outcome<SpawnResult> spawn(const t::ManagedProcessSpec& process) override {
      return delegate->spawn(process);
    }
    bool alive(std::int64_t pid, std::string_view marker) override {
      return delegate->alive(pid, marker);
    }
    t::Outcome<bool> terminate(std::int64_t pid, std::string_view marker,
                               std::chrono::milliseconds grace) override {
      const bool owned_group = ::getpgid(static_cast<pid_t>(pid)) == pid;
      const bool before = delegate->alive(pid, marker);
      errno = 0;
      const int group_probe = ::kill(-static_cast<pid_t>(pid), 0);
      const int group_errno = group_probe == 0 ? 0 : errno;
      const auto started = std::chrono::steady_clock::now();
      auto result = delegate->terminate(pid, marker, grace);
      const int terminate_errno = errno;
      if (const auto* error = std::get_if<t::Failure>(&result)) {
        std::cerr << "fixture terminate failure code=" << static_cast<int>(error->code)
                  << " own_group=" << owned_group << " alive_before=" << before
                  << " group_probe_errno=" << group_errno << " errno=" << terminate_errno
                  << " elapsed_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started)
                         .count()
                  << '\n';
      }
      return result;
    }
  };
  auto supervisor = std::make_unique<ObservedSupervisor>();
  t::HostIdentity identity{
      std::string{kHost},
      {{"fixture", temporary.root.string(), std::string(64, 'a'), "fixed-fixture", {}}},
      {}};
  const auto ready_file = temporary.root / "fixed-child.ready";
  t::LaunchProfile profile{
      "fixed-fixture", executable, {executable, "--managed-fixture", ready_file.string()}, {}, {}};
  t::HostServiceDependencies dependencies{*journal, *supervisor, bridge, clock, identity, {profile},
                                          {},       {},          100ms,  8,     {},       nullptr};
  dependencies.admission = maintenance.get();
  t::HostService service{dependencies};
  (void)take(service.start());
  t::HostProcessSpec process{std::string{kInstance},
                             std::string{kInstallation},
                             1,
                             1,
                             t::DesiredState::Running,
                             std::string(64, 'a'),
                             "fixed-fixture",
                             {},
                             {}};
  const auto applied =
      take(service.apply_desired_process(process, "fixed-apply", std::string(64, 'b')));
  struct OwnedProcessCleanup {
    t::ProcessSupervisor& supervisor;
    std::optional<t::ManagedExecution> execution;
    ~OwnedProcessCleanup() {
      if (execution && supervisor.alive(execution->pid, execution->process_marker)) {
        (void)supervisor.terminate(execution->pid, execution->process_marker, 100ms);
      }
    }
  } cleanup{*supervisor, applied.execution};
  require(applied.execution &&
              supervisor->alive(applied.execution->pid, applied.execution->process_marker),
          "real allow-listed fixture process starts");
  const auto ready_deadline = std::chrono::steady_clock::now() + 2s;
  while (!fs::exists(ready_file) && std::chrono::steady_clock::now() < ready_deadline) {
    require(supervisor->alive(applied.execution->pid, applied.execution->process_marker),
            "fixed child stays at its original marker before readiness");
    std::this_thread::sleep_for(1ms);
  }
  require(fs::exists(ready_file) &&
              supervisor->alive(applied.execution->pid, applied.execution->process_marker) &&
              ::getpgid(static_cast<pid_t>(applied.execution->pid)) == applied.execution->pid,
          "fixed child reached main and owns the recorded process group before stop");
  t::HostMaintenanceApplication app{{std::string{kHost}}, *maintenance, service, gate, auth, clock};
  const auto accepted = take(app.begin(command(clock)));
  (void)take(app.advance(accepted.operation.ref.id));
  auto another = process;
  another.instance_id = "33333333-3333-4333-8333-333333333333";
  rejects(service.apply_desired_process(another, "forbidden-during-drain", std::string(64, 'c')),
          t::ErrorCode::Conflict);
  (void)take(app.advance(accepted.operation.ref.id));
  (void)take(app.advance(accepted.operation.ref.id));
  // Stop is a real effect. Make the later progress transaction fail and verify recovery uses
  // the journal's known stopped fact, not another spawn or an invented success receipt.
  sql(path, "CREATE TRIGGER reject_after_stop BEFORE UPDATE ON host_maintenance_operations BEGIN "
            "SELECT RAISE(ABORT,'fixed fault'); END;");
  const auto rejected = app.advance(accepted.operation.ref.id);
  rejects(rejected, t::ErrorCode::Internal);
  require(std::get<t::Failure>(rejected).acceptance == t::Acceptance::Unknown &&
              !supervisor->alive(applied.execution->pid, applied.execution->process_marker),
          "known process stopped but uncommitted operation stays queryable unknown");
  sql(path, "DROP TRIGGER reject_after_stop;");
  const auto known = take(journal->find_execution({process.instance_id}, {1}));
  std::cerr << "journal after rollback present=" << known.has_value()
            << " state=" << (known ? static_cast<int>(known->state) : -1)
            << " outcome=" << (known ? static_cast<int>(known->outcome) : -1) << '\n';
  require(known && known->state == t::ManagedExecutionState::Exited &&
              (known->outcome == t::ExecutionOutcome::KnownSucceeded ||
               known->outcome == t::ExecutionOutcome::KnownFailed),
          "confirmed stop survives failed maintenance progress write in journal");
  const auto recovered = finish(app, accepted.operation.ref.id);
  if (recovered.operation.state != t::OperationState::Succeeded) {
    std::cerr << "recovery phase=" << static_cast<int>(recovered.phase)
              << " state=" << static_cast<int>(recovered.operation.state) << " reason="
              << (recovered.operation.blocked_reason
                      ? static_cast<int>(*recovered.operation.blocked_reason)
                      : -1)
              << " targets=" << recovered.targets.size() << '\n';
  }
  require(recovered.operation.state == t::OperationState::Succeeded,
          "known journal evidence recovers operation after progress rollback");
  require(take(journal->active_executions()).empty(),
          "no unresolved managed execution after confirmed stop");
  // An Exited+UNKNOWN record must remain visible; offline/gone PID never permits resume.
  auto unknown = *applied.execution;
  unknown.state = t::ManagedExecutionState::Exited;
  unknown.outcome = t::ExecutionOutcome::Unknown;
  (void)take(journal->save_execution(unknown));
  require(take(journal->active_executions()).size() == 1,
          "real SQLite retains Exited+UNKNOWN maintenance evidence");
  auto resume =
      command(clock, "resume-with-unknown", take(maintenance->maintenance_state()).version.value);
  resume.idempotency_key = "resume-with-unknown-key";
  const auto pending = take(app.resume(resume));
  require(take(app.advance(pending.operation.ref.id)).operation.state ==
                  t::OperationState::Blocked &&
              !take(maintenance->allows_start()),
          "real unknown journal evidence prevents reopening");
  service.stop();
}
} // namespace

int main(int argc, char* argv[]) {
  if (argc == 3 && std::string_view{argv[1]} == "--managed-fixture") {
    // Fixed owned test process, no input, shell, external service or user container.
    const int ready = ::open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (ready < 0) {
      return 112;
    }
    const char marker = '1';
    const bool written = ::write(ready, &marker, 1) == 1;
    ::close(ready);
    if (!written) {
      return 113;
    }
    for (;;) {
      ::pause();
    }
  }
  try {
    happy_cancel_and_replay();
    unknown_gate_and_process();
    changes_atomicity_and_bounds();
    commit_response_loss();
    real_managed_process(fs::canonical(argv[0]).c_str());
    std::cout << "Host maintenance SQLite/application/local-process tests passed; Runtime gate is "
                 "a typed fixture, not backend evidence\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
