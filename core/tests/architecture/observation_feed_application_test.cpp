// WS-12 observation feed application/RPC boundary evidence: exact-scope current authorization,
// owner-local read-only UoW handling, typed gap passthrough, page revalidation and wire encoding.
// No network, PG or owner process is involved.
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "kiarabot/core/application/target/observation_feed_application.hpp"
#include "kiarabot/core/runtime/platform/observation_feed_rpc.hpp"

namespace t = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace v1 = kiarabot::v1;

namespace {
const std::string kSubject = "11111111-1111-4111-8111-111111111111";
const std::string kOtherSubject = "22222222-2222-4222-8222-222222222222";
const std::string kTask = "33333333-3333-4333-8333-333333333333";
const std::string kEntry = "entry-1";
const std::string kMessage = "44444444-4444-4444-8444-444444444444";
const t::TimePoint kNow{std::chrono::seconds{20000}};

void require(bool condition, const char* reason) {
  if (!condition) {
    throw std::runtime_error(std::string{"WS12 application assertion: "} + reason);
  }
}
struct State {
  t::Service owner = t::Service::Runtime;
  int begins = 0;
  int rollbacks = 0;
  int queries = 0;
  int checks = 0;
  bool allowed = true;
  bool skip_scope_binding = false;
  std::optional<t::Failure> storage_error;
  bool bad_page = false;
  std::vector<t::ResourceRef> response_scope;
};
class Work final : public t::UnitOfWork {
public:
  explicit Work(State& state) : state_(state) {}
  ~Work() override {
    rollback();
  }
  t::Service owner() const noexcept override {
    return state_.owner;
  }
  t::Outcome<t::TimePoint> current_time() override {
    return kNow;
  }
  t::ReceiptRepository& receipts() override {
    throw std::logic_error("unexpected receipt");
  }
  t::JournalRepository& journal() override {
    throw std::logic_error("unexpected audit journal");
  }
  t::Repository<t::OperationId, t::Operation>& operations() override {
    throw std::logic_error("unexpected live operation");
  }
  t::CommitResult commit() override {
    throw std::logic_error("feed query must not commit");
  }
  void rollback() noexcept override {
    if (!closed_) {
      ++state_.rollbacks;
      closed_ = true;
    }
  }

private:
  State& state_;
  bool closed_ = false;
};
class Authorization final : public t::RuntimeAuthorizationPort {
public:
  explicit Authorization(State& state) : state_(state) {}
  bool available() const noexcept override {
    return true;
  }
  t::Outcome<t::RuntimeAuthorizationDecision>
  check(const t::RuntimeAuthorizationRequest& request) override {
    ++state_.checks;
    if (!state_.allowed) {
      return t::RuntimeAuthorizationDecision{false,
                                             request.authentication.subject,
                                             request.permission,
                                             request.scope,
                                             t::ResourceVersion{7},
                                             kNow,
                                             t::Failure{t::ErrorCode::PermissionDenied,
                                                        t::Acceptance::NotAccepted,
                                                        t::Recovery::ReadCurrent,
                                                        {},
                                                        {}}};
    }
    return t::RuntimeAuthorizationDecision{
        true,          request.authentication.subject, request.permission,
        request.scope, t::ResourceVersion{7},          kNow,
        std::nullopt};
  }

private:
  State& state_;
};
t::ResourceRef catalog() {
  return {t::ResourceKind::Catalog, t::ResourceId{"control"}};
}
t::ResourceRef task_resource() {
  return {t::ResourceKind::Task, t::ResourceId{kTask}};
}
t::ResourceRef account() {
  return {t::ResourceKind::Account, t::ResourceId{kSubject}};
}
t::AuthenticationContext authentication(t::Service owner, const std::vector<t::ResourceRef>& scope,
                                        const std::string& permission) {
  t::AuthenticationContext result;
  result.subject = t::SubjectId{kSubject};
  result.direct_peer = t::PlatformPeer{t::Service::Control, t::SubjectId{kSubject}};
  result.audience = owner;
  result.authorization_version = t::ResourceVersion{7};
  result.verified_at = kNow - std::chrono::seconds{1};
  result.expires_at = kNow + std::chrono::seconds{30};
  result.proof = "delegation-proof";
  for (const auto& resource : scope) {
    result.scopes.push_back(t::ResourceScope{resource, permission});
  }
  return result;
}
t::ObservationFeedCommand command(t::Service owner, t::ObservationFamily family,
                                  const std::vector<t::ResourceRef>& scope) {
  t::ObservationFeedCommand result;
  result.context.authentication =
      authentication(owner, scope, std::string{t::observation_feed_permission_text(owner, family)});
  result.context.request_id = kSubject;
  result.context.trace_id = kSubject;
  result.context.method = std::string{t::observation_feed_method(owner, family)};
  result.context.deadline = kNow + std::chrono::seconds{2};
  result.query.family = family;
  result.query.scope = scope;
  result.query.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  return result;
}
t::ObservationFeedPage page(const t::ObservationFeedCommand& input, std::string epoch) {
  t::ObservationFeedPage result;
  result.next_cursor = {input.context.authentication.audience, input.query.family, std::move(epoch),
                        input.query.scope, 2};
  result.checked_at = kNow;
  if (input.query.cursor) {
    t::ObservationFeedChange change;
    change.cursor = {input.context.authentication.audience, input.query.family,
                     result.next_cursor.epoch, input.query.scope, 2};
    if (input.query.family == t::ObservationFamily::Logs) {
      t::LogObservation log;
      log.owner = t::Service::Runtime;
      log.entry_id = kEntry;
      log.severity = "INFO";
      log.component = "runtime";
      log.method = "ListAccounts";
      log.event = "request.recorded";
      log.resource = account();
      log.request_id = "request-1";
      log.trace_id = "trace-1";
      log.occurred_at = kNow;
      change.payload = log;
    } else if (input.query.family == t::ObservationFamily::Tasks) {
      t::TaskObservation task;
      task.task_id = kTask;
      task.owner_instance_id = kOtherSubject;
      task.state = t::TaskFeedState::Completed;
      task.version = 3;
      task.updated_at = kNow;
      change.payload = task;
    } else {
      t::MessageObservation message;
      message.message_id = kMessage;
      message.conversation_id = kOtherSubject;
      message.account_id = kSubject;
      message.received_at = kNow;
      change.payload = message;
    }
    result.changes.push_back(std::move(change));
  }
  return result;
}
struct Fixture {
  State state;
  Authorization authorization{state};
  std::unique_ptr<t::ObservationFeedApplication> application;
  explicit Fixture(t::Service owner = t::Service::Runtime,
                   t::ObservationFamily family = t::ObservationFamily::Logs)
      : state{[&] {
          State value;
          value.owner = owner;
          return value;
        }()},
        authorization(state) {
    t::ObservationFeedDependencies dependencies{
        owner,
        family,
        [this] {
          ++state.begins;
          return t::Outcome<std::unique_ptr<t::UnitOfWork>>{std::make_unique<Work>(state)};
        },
        [this](t::UnitOfWork&, const t::ObservationFeedQuery& query) {
          ++state.queries;
          if (state.storage_error) {
            return t::Outcome<t::ObservationFeedPage>{*state.storage_error};
          }
          t::ObservationFeedCommand shape;
          shape.query = query;
          shape.context.authentication.audience = state.owner;
          auto result = page(shape, "epoch-1");
          if (state.bad_page) {
            result.next_cursor.owner = t::Service::Identity;
          }
          return t::Outcome<t::ObservationFeedPage>{std::move(result)};
        },
        authorization,
        [] { return kNow; }};
    auto created = t::make_observation_feed_application(std::move(dependencies));
    require(std::holds_alternative<std::unique_ptr<t::ObservationFeedApplication>>(created),
            "application factory");
    application = std::move(std::get<std::unique_ptr<t::ObservationFeedApplication>>(created));
  }
};

void allowed_page() {
  Fixture fixture;
  auto input = command(t::Service::Runtime, t::ObservationFamily::Logs, {catalog()});
  input.query.cursor = t::ObservationFeedCursor{
      t::Service::Runtime, t::ObservationFamily::Logs, "epoch-1", {catalog()}, 1};
  auto result = fixture.application->query(input);
  require(std::holds_alternative<t::ObservationFeedPage>(result), "allowed page");
  const auto& page = std::get<t::ObservationFeedPage>(result);
  require(page.changes.size() == 1 && fixture.state.begins == 1 && fixture.state.rollbacks == 1 &&
              fixture.state.checks == 1,
          "read-only UoW usage");
  require(page.next_cursor.sequence == 2, "cursor advances");
}
void missing_scope_binding() {
  Fixture fixture;
  auto input = command(t::Service::Runtime, t::ObservationFamily::Logs, {catalog()});
  input.context.authentication.scopes.clear();
  const auto result = fixture.application->query(input);
  require(std::holds_alternative<t::Failure>(result) &&
              std::get<t::Failure>(result).code == t::ErrorCode::PermissionDenied,
          "missing current scope binding denies");
  require(fixture.state.begins == 0, "no UoW before authorization");
}
void denied() {
  Fixture fixture;
  fixture.state.allowed = false;
  const auto result = fixture.application->query(
      command(t::Service::Runtime, t::ObservationFamily::Logs, {catalog()}));
  require(std::holds_alternative<t::Failure>(result) &&
              std::get<t::Failure>(result).code == t::ErrorCode::PermissionDenied,
          "denied authorization");
}
void typed_gap_passthrough() {
  Fixture fixture;
  fixture.state.storage_error = t::Failure{
      t::ErrorCode::Conflict, t::Acceptance::NotAccepted, t::Recovery::ReadCurrent, {}, {}};
  const auto result = fixture.application->query(
      command(t::Service::Runtime, t::ObservationFamily::Logs, {catalog()}));
  require(std::holds_alternative<t::Failure>(result) &&
              std::get<t::Failure>(result).code == t::ErrorCode::Conflict,
          "typed gap is preserved, never an empty page");
}
void invalid_page_is_internal() {
  Fixture fixture;
  fixture.state.bad_page = true;
  auto input = command(t::Service::Runtime, t::ObservationFamily::Logs, {catalog()});
  const auto result = fixture.application->query(input);
  require(std::holds_alternative<t::Failure>(result) &&
              std::get<t::Failure>(result).code == t::ErrorCode::Internal,
          "foreign page boundary is rejected");
}
void task_scope_and_family() {
  Fixture fixture(t::Service::Scheduler, t::ObservationFamily::Tasks);
  auto input = command(t::Service::Scheduler, t::ObservationFamily::Tasks, {task_resource()});
  auto result = fixture.application->query(input);
  require(std::holds_alternative<t::ObservationFeedPage>(result), "scheduler task page");
  auto wrong_scope = command(t::Service::Scheduler, t::ObservationFamily::Tasks, {account()});
  const auto rejected = fixture.application->query(wrong_scope);
  require(std::holds_alternative<t::Failure>(rejected) &&
              std::get<t::Failure>(rejected).code == t::ErrorCode::InvalidArgument,
          "wrong family scope kinds rejected");
  auto wrong_method =
      command(t::Service::Scheduler, t::ObservationFamily::Tasks, {task_resource()});
  wrong_method.context.method = "kiarabot.v1.SchedulerService/ListOperationChanges";
  const auto method_rejected = fixture.application->query(wrong_method);
  require(std::holds_alternative<t::Failure>(method_rejected) &&
              std::get<t::Failure>(method_rejected).code == t::ErrorCode::InvalidArgument,
          "method binding enforced");
}
void encoding() {
  Fixture fixture(t::Service::Scheduler, t::ObservationFamily::Tasks);
  auto input = command(t::Service::Scheduler, t::ObservationFamily::Tasks, {task_resource()});
  input.query.cursor = t::ObservationFeedCursor{
      t::Service::Scheduler, t::ObservationFamily::Tasks, "epoch-1", {task_resource()}, 1};
  auto page = std::get<t::ObservationFeedPage>(fixture.application->query(input));
  v1::RequestContext request;
  request.set_request_id(kSubject);
  request.set_trace_id(kSubject);
  v1::TaskChangesResponse tasks;
  require(std::holds_alternative<std::monostate>(
              platform::encode_task_changes_page(page, request, tasks)),
          "task encode");
  require(tasks.changes_size() == 1 && tasks.changes(0).task().task_id() == kTask &&
              tasks.changes(0).task().state() == v1::SCHEDULED_TASK_COMPLETED &&
              tasks.changes(0).cursor().source_service() == "scheduler",
          "task wire projection");
  v1::LogChangesResponse logs;
  auto log_page = page;
  log_page.next_cursor.family = t::ObservationFamily::Logs;
  log_page.changes.clear();
  require(std::holds_alternative<std::monostate>(
              platform::encode_log_changes_page(log_page, request, logs)),
          "empty log encode");
  v1::MessageMetadataChangesResponse messages;
  auto message_page = page;
  message_page.next_cursor.family = t::ObservationFamily::Messages;
  message_page.changes.clear();
  require(std::holds_alternative<std::monostate>(
              platform::encode_message_metadata_changes_page(message_page, request, messages)),
          "empty message encode");
}
} // namespace

int main() {
  try {
    allowed_page();
    missing_scope_binding();
    denied();
    typed_gap_passthrough();
    invalid_page_is_internal();
    task_scope_and_family();
    encoding();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "observation feed application: ok\n";
  return 0;
}
