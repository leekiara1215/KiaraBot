#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

#include "kiarabot/core/application/target/operation_observation_application.hpp"

namespace t = kiarabot::core::target;
namespace {
const std::string kId = "11111111-1111-4111-8111-111111111111";
const std::string kOther = "22222222-2222-4222-8222-222222222222";
const t::TimePoint kNow{std::chrono::seconds{10000}};
struct State {
  t::Service owner = t::Service::Runtime;
  int begins = 0;
  int queries = 0;
  int rollbacks = 0;
  std::optional<t::Failure> storage_error;
  bool bad_scope = false;
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
    throw std::logic_error("unexpected audit");
  }
  t::Repository<t::OperationId, t::Operation>& operations() override {
    throw std::logic_error("unexpected live operation");
  }
  t::CommitResult commit() override {
    throw std::logic_error("observation must not commit");
  }
  void rollback() noexcept override {
    if (!closed_) {
      ++state_.rollbacks;
      closed_ = true;
    }
  }
  t::Outcome<t::OperationObservationPage>
  query_operation_changes(const t::OperationObservationQuery& query) override {
    ++state_.queries;
    if (state_.storage_error) {
      return *state_.storage_error;
    }
    t::OperationObservationPage result;
    result.next_cursor = {state_.owner, "dataset", query.scope, 3};
    result.checked_at = kNow;
    if (state_.bad_scope) {
      result.next_cursor.scope[0].id.value = kOther;
    }
    return result;
  }

private:
  State& state_;
  bool closed_ = false;
};
class Authorization final : public t::RuntimeAuthorizationPort {
public:
  bool available() const noexcept override {
    return true;
  }
  t::Outcome<t::RuntimeAuthorizationDecision>
  check(const t::RuntimeAuthorizationRequest& request) override {
    ++checks;
    last = request;
    if (error) {
      return *error;
    }
    return t::RuntimeAuthorizationDecision{allow,
                                           request.authentication.subject,
                                           request.permission,
                                           request.scope,
                                           {1},
                                           stale ? kNow - std::chrono::seconds{5} : kNow,
                                           {}};
  }
  bool allow = true;
  bool stale = false;
  int checks = 0;
  std::optional<t::Failure> error;
  t::RuntimeAuthorizationRequest last;
};
t::OperationObservationCommand command(t::Service owner) {
  t::OperationObservationCommand value;
  value.context.request_id = kId;
  value.context.trace_id = kOther;
  const auto name = owner == t::Service::Runtime     ? "Runtime"
                    : owner == t::Service::Scheduler ? "Scheduler"
                                                     : "Model";
  value.context.method = std::string{"kiarabot.v1."} + name + "Service/ListOperationChanges";
  value.context.deadline = kNow + std::chrono::seconds{2};
  auto& authentication = value.context.authentication;
  authentication.subject = {kId};
  authentication.direct_peer = t::PlatformPeer{t::Service::Control, {kOther}};
  authentication.audience = owner;
  authentication.authorization_version = {1};
  authentication.verified_at = kNow;
  authentication.expires_at = kNow + std::chrono::seconds{30};
  authentication.proof = "opaque-proof";
  const auto kind = owner == t::Service::Model       ? t::ResourceKind::Model
                    : owner == t::Service::Scheduler ? t::ResourceKind::Task
                                                     : t::ResourceKind::Account;
  value.query.scope = {{kind, {kId}}};
  authentication.scopes = {
      {value.query.scope[0], owner == t::Service::Scheduler ? "task.read" : "resource.read"}};
  return value;
}
void expect(const t::Outcome<t::OperationObservationPage>& result, t::ErrorCode code) {
  assert(std::holds_alternative<t::Failure>(result));
  assert(std::get<t::Failure>(result).code == code);
}
void run(t::Service owner) {
  State state;
  state.owner = owner;
  Authorization authorization;
  auto built = t::make_operation_observation_application(
      {owner,
       [&]() -> t::Outcome<std::unique_ptr<t::UnitOfWork>> {
         ++state.begins;
         return std::unique_ptr<t::UnitOfWork>(std::make_unique<Work>(state));
       },
       authorization, [] { return kNow; }});
  assert(std::holds_alternative<std::unique_ptr<t::OperationObservationApplication>>(built));
  auto app = std::move(std::get<std::unique_ptr<t::OperationObservationApplication>>(built));
  auto input = command(owner);
  assert(std::holds_alternative<t::OperationObservationPage>(app->query(input)));
  assert(state.begins == 1 && state.queries == 1 && state.rollbacks == 1);
  assert(authorization.last.permission ==
         (owner == t::Service::Scheduler ? t::Permission::TaskRead : t::Permission::ResourceRead));
  assert(authorization.last.deadline == kNow + std::chrono::milliseconds{500});
  const auto before = state.begins;
  authorization.allow = false;
  expect(app->query(input), t::ErrorCode::PermissionDenied);
  assert(state.begins == before);
  authorization.allow = true;
  authorization.error = t::Failure{
      t::ErrorCode::UpstreamUnavailable, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
  expect(app->query(input), t::ErrorCode::UpstreamUnavailable);
  assert(state.begins == before);
  authorization.error.reset();
  authorization.stale = true;
  expect(app->query(input), t::ErrorCode::Internal);
  assert(state.begins == before);
  authorization.stale = false;
  auto forbidden = input;
  forbidden.context.authentication.direct_peer = t::PlatformPeer{t::Service::Runtime, {kOther}};
  const auto checks = authorization.checks;
  expect(app->query(forbidden), t::ErrorCode::PermissionDenied);
  assert(authorization.checks == checks && state.begins == before);
  forbidden = input;
  forbidden.context.authentication.scopes.clear();
  expect(app->query(forbidden), t::ErrorCode::PermissionDenied);
  assert(state.begins == before);
  auto invalid = input;
  invalid.query.scope.push_back(invalid.query.scope[0]);
  expect(app->query(invalid), t::ErrorCode::InvalidArgument);
  assert(state.begins == before);
  invalid = input;
  invalid.query.scope[0].kind = t::ResourceKind::Catalog;
  expect(app->query(invalid), t::ErrorCode::InvalidArgument);
  auto expired = input;
  expired.context.deadline = kNow;
  expect(app->query(expired), t::ErrorCode::Timeout);
  assert(state.begins == before);
  state.storage_error = t::Failure{
      t::ErrorCode::Conflict, t::Acceptance::NotAccepted, t::Recovery::ReadCurrent, {}, {}};
  expect(app->query(input), t::ErrorCode::Conflict);
  state.storage_error.reset();
  state.bad_scope = true;
  expect(app->query(input), t::ErrorCode::Internal);
  assert(state.rollbacks == state.begins);
}
} // namespace
int main() {
  run(t::Service::Runtime);
  run(t::Service::Scheduler);
  run(t::Service::Model);
}
