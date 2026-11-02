#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <variant>
#include <vector>

#include <sys/wait.h>

extern char** environ;

#include <pqxx/pqxx>

#include "kiarabot/core/application/target/runtime_resource_application.hpp"
#include "kiarabot/core/domain/target/event_delivery.hpp"
#include "kiarabot/core/domain/target/persistence.hpp"
#include "kiarabot/core/ports/target/business_ports.hpp"
#include "kiarabot/core/ports/target/runtime_authorization.hpp"
#include "kiarabot/core/ports/target/runtime_ids.hpp"
#include "kiarabot/core/ports/target/runtime_secrets.hpp"
#include "kiarabot/infrastructure/postgres/database.hpp"
#include "kiarabot/infrastructure/postgres/factories.hpp"

namespace pg = kiarabot::infrastructure::postgres;
namespace target = kiarabot::core::target;

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

target::EventRecord event(std::string event_id, std::uint64_t sequence) {
  target::EventRecord record;
  record.key = {{"adapter"}, std::move(event_id)};
  record.fingerprint.bytes[0] = std::byte{1};
  record.conversation = {
      {"adapter"}, {"account"}, "conversation", target::ConversationKey::Kind::Group};
  record.payload = target::EventPayloadKind::MessageReceived;
  record.has_conversation = true;
  record.sequence = sequence;
  record.has_sequence = true;
  record.serialized_envelope = "envelope";
  record.occurred_at = target::TimePoint{std::chrono::seconds{1000}};
  return record;
}

target::DeliveryRecord target_for(const target::EventRecord& record, std::string instance,
                                  bool ordered = false) {
  target::DeliveryRecord delivery;
  delivery.delivery = {record.key, {std::move(instance)}};
  delivery.ordered = ordered;
  delivery.trace_id = "trace-" + record.key.event_id;
  return delivery;
}

std::string uuid_like(char seed) {
  std::string value(36, '0');
  for (const int index : {8, 13, 18, 23}) {
    value[static_cast<std::size_t>(index)] = '-';
  }
  value[0] = seed;
  value[14] = '4';
  value[19] = '8';
  return value;
}

class FakeRuntimeIds final : public target::RuntimeIdPort {
public:
  target::Outcome<target::ResourceId> account_id() override {
    return target::ResourceId{uuid_like('a')};
  }
  target::Outcome<target::InstallationId> installation_id() override {
    return target::InstallationId{uuid_like('b')};
  }
  target::Outcome<target::InstanceId> instance_id() override {
    return target::InstanceId{uuid_like('c')};
  }
  target::Outcome<target::InstanceId> adapter_instance_id() override {
    return target::InstanceId{uuid_like('d')};
  }
  target::Outcome<target::ConnectionId> connection_id() override {
    return target::ConnectionId{uuid_like('e')};
  }
  target::Outcome<target::OperationId> operation_id() override {
    return target::OperationId{uuid_like('f')};
  }
  target::Outcome<target::RuntimeEpoch> runtime_epoch() override {
    return target::RuntimeEpoch{uuid_like('1')};
  }
  target::Outcome<std::string> snapshot_id() override {
    return uuid_like('2');
  }
};

class FakeRuntimeSecrets final : public target::RuntimeSecretPort {
public:
  target::Outcome<std::string> bootstrap_credential() override {
    return std::string(64, 'a');
  }
  target::Outcome<std::array<std::byte, 32>> opaque_secret() override {
    return std::array<std::byte, 32>{};
  }
  target::Outcome<target::Sha256Digest> sha256(std::string_view value) override {
    target::Sha256Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size() && index < value.size(); ++index) {
      digest.bytes[index] = static_cast<std::byte>(value[index]);
    }
    return digest;
  }
};

class FakeRuntimeAuthorization final : public target::RuntimeAuthorizationPort {
public:
  bool available() const noexcept override {
    return true;
  }
  target::Outcome<target::RuntimeAuthorizationDecision>
  check(const target::RuntimeAuthorizationRequest& request) override {
    target::RuntimeAuthorizationDecision decision;
    decision.allowed = allow && request.permission == target::Permission::TimerAccept;
    decision.subject = request.authentication.subject;
    decision.permission = request.permission;
    decision.scope = request.scope;
    decision.authorization_version = {1};
    decision.checked_at = target::TimePoint{std::chrono::system_clock::now().time_since_epoch()};
    if (!decision.allowed) {
      decision.denial =
          target::Failure{target::ErrorCode::PermissionDenied, target::Acceptance::NotAccepted,
                          target::Recovery::ReadCurrent, std::nullopt, std::nullopt};
    }
    return decision;
  }
  bool allow = true;
};

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
  auto opened = pg::open_runtime(config);
  if (std::holds_alternative<target::Failure>(opened)) {
    throw TestFailure("runtime factory open failed");
  }
  auto factory = std::move(std::get<std::unique_ptr<target::RuntimeUnitOfWorkFactory>>(opened));
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
  const target::EventCapacityLimits limits{8, 8};

  // --- acceptance: insert, duplicate, conflict, capacity ---
  {
    auto work = begin();
    const auto record = event("event-1", 1);
    const auto targets = std::vector<target::DeliveryRecord>{target_for(record, "instance-a", true),
                                                             target_for(record, "instance-b")};
    auto accepted = work->events().accept(record, targets, limits);
    require(std::holds_alternative<target::EventAcceptanceResult>(accepted) &&
                !std::get<target::EventAcceptanceResult>(accepted).duplicate &&
                std::get<target::EventAcceptanceResult>(accepted).targets == 2,
            "new event accepted with fan-out targets");
    auto duplicate = work->events().accept(record, targets, limits);
    require(std::holds_alternative<target::EventAcceptanceResult>(duplicate) &&
                std::get<target::EventAcceptanceResult>(duplicate).duplicate,
            "same id and fingerprint is duplicate");
    require(work->commit() == target::CommitResult::Committed, "runtime UoW commits");
    // Conflict/capacity failures poison their UoW, so each runs in a fresh transaction. A UoW
    // owns the single session permit, so the poisoned work must be destroyed before the next.
    {
      auto conflict_work = begin();
      auto changed = record;
      changed.fingerprint.bytes[1] = std::byte{2};
      auto conflict = conflict_work->events().accept(changed, targets, limits);
      require(std::holds_alternative<target::Failure>(conflict) &&
                  std::get<target::Failure>(conflict).code == target::ErrorCode::Conflict,
              "same id with changed content conflicts");
    }
    auto rejected_work = begin();
    const target::EventCapacityLimits tight{1, 8};
    auto backpressure = rejected_work->events().accept(event("event-capacity", 2), {}, tight);
    require(std::holds_alternative<target::Failure>(backpressure) &&
                std::get<target::Failure>(backpressure).code == target::ErrorCode::RateLimited,
            "event capacity rejects new acceptance");
  }

  // --- sequence watermark: old sequence rejected, gap degrades ---
  {
    auto work = begin();
    const auto gap = event("event-gap", 3);
    auto accepted = work->events().accept(gap, {}, limits);
    require(std::holds_alternative<target::EventAcceptanceResult>(accepted) &&
                std::get<target::EventAcceptanceResult>(accepted).sequence.watermark.degraded,
            "sequence gap degrades the conversation");
    require(work->commit() == target::CommitResult::Committed, "watermark transaction commits");
    auto stale_work = begin();
    auto stale = stale_work->events().accept(event("event-stale", 2), {}, limits);
    require(std::holds_alternative<target::Failure>(stale) &&
                std::get<target::Failure>(stale).code == target::ErrorCode::Conflict,
            "older sequence is rejected");
  }

  // --- claim / ACK / duplicate / stale attempt ---
  target::TimePoint base;
  {
    auto work = begin();
    base = now(work);
    target::DeliveryClaimRequest request;
    request.instance = {"instance-a"};
    request.fence = {{"instance-a"}, {"epoch"}, {"execution"}, {1}, {"connection"}, {1}};
    request.limit = 1;
    request.lease = target::kDeliveryLease;
    request.now = base;
    request.attempt_ids = {{"attempt-1"}};
    auto claimed = work->events().claim(request);
    require(std::holds_alternative<std::vector<target::DeliveryClaim>>(claimed) &&
                std::get<std::vector<target::DeliveryClaim>>(claimed).size() == 1 &&
                std::get<std::vector<target::DeliveryClaim>>(claimed)[0].event.key.event_id ==
                    "event-1",
            "ordered head claim returns the earliest event");
    require(std::get<std::vector<target::DeliveryClaim>>(claimed)[0].trace_id == "trace-event-1",
            "claim carries the delivery trace id");
    target::DeliveryAcknowledgement ack;
    ack.attempt = std::get<std::vector<target::DeliveryClaim>>(claimed)[0].attempt;
    ack.accepted = true;
    auto receipt = work->events().acknowledge(ack, base);
    require(std::holds_alternative<target::DeliveryReceipt>(receipt) &&
                std::get<target::DeliveryReceipt>(receipt).disposition ==
                    target::DeliveryDisposition::Acknowledged,
            "current attempt acknowledged");
    auto duplicate = work->events().acknowledge(ack, base);
    require(std::holds_alternative<target::DeliveryReceipt>(duplicate) &&
                std::get<target::DeliveryReceipt>(duplicate).disposition ==
                    target::DeliveryDisposition::Duplicate,
            "repeat acknowledgement is duplicate");
    require(work->commit() == target::CommitResult::Committed, "claim/ack transaction commits");
  }

  // --- retry backoff, dead letter, stale ACK after a new attempt ---
  {
    auto work = begin();
    const auto record = event("event-retry", 4);
    auto accepted = work->events().accept(record, {target_for(record, "instance-retry")}, limits);
    require(std::holds_alternative<target::EventAcceptanceResult>(accepted),
            "retry event accepted");
    base = now(work);
    target::DeliveryClaimRequest request;
    request.instance = {"instance-retry"};
    request.fence = {{"instance-retry"}, {"epoch"}, {"execution"}, {1}, {"connection"}, {1}};
    request.limit = 1;
    request.lease = target::kDeliveryLease;
    request.now = base;
    request.attempt_ids = {{"retry-attempt-1"}};
    auto claimed = work->events().claim(request);
    require(std::holds_alternative<std::vector<target::DeliveryClaim>>(claimed) &&
                std::get<std::vector<target::DeliveryClaim>>(claimed).size() == 1 &&
                std::get<std::vector<target::DeliveryClaim>>(claimed)[0].event.key.event_id ==
                    "event-retry",
            "retry target claimed");
    const auto first = std::get<std::vector<target::DeliveryClaim>>(claimed)[0];
    target::DeliveryAcknowledgement reject;
    reject.attempt = first.attempt;
    reject.accepted = false;
    reject.retryable = true;
    reject.failure = target::Failure{target::ErrorCode::UpstreamUnavailable,
                                     target::Acceptance::NotAccepted,
                                     target::Recovery::Backoff,
                                     {},
                                     {}};
    auto receipt = work->events().acknowledge(reject, base);
    require(std::holds_alternative<target::DeliveryReceipt>(receipt) &&
                std::get<target::DeliveryReceipt>(receipt).disposition ==
                    target::DeliveryDisposition::Rejected,
            "rejected retryable delivery scheduled");
    auto too_early = work->events().claim(request);
    require(std::holds_alternative<std::vector<target::DeliveryClaim>>(too_early) &&
                std::get<std::vector<target::DeliveryClaim>>(too_early).empty(),
            "backoff blocks immediate reclaim");
    request.now = base + std::chrono::seconds{2};
    request.attempt_ids = {{"retry-attempt-2"}};
    auto reclaimed = work->events().claim(request);
    require(std::holds_alternative<std::vector<target::DeliveryClaim>>(reclaimed) &&
                std::get<std::vector<target::DeliveryClaim>>(reclaimed).size() == 1,
            "retry dispatch after backoff succeeds");
    const auto second = std::get<std::vector<target::DeliveryClaim>>(reclaimed)[0];
    target::DeliveryAcknowledgement stale = reject;
    stale.attempt = first.attempt;
    stale.retryable = false;
    auto stale_receipt = work->events().acknowledge(stale, request.now);
    require(std::holds_alternative<target::DeliveryReceipt>(stale_receipt) &&
                std::get<target::DeliveryReceipt>(stale_receipt).disposition ==
                    target::DeliveryDisposition::Stale,
            "old attempt ack is stale");
    auto renewed = work->events().renew(second.attempt, request.now);
    require(std::holds_alternative<target::DeliveryReceipt>(renewed) &&
                std::get<target::DeliveryReceipt>(renewed).disposition ==
                    target::DeliveryDisposition::Renewed &&
                std::get<target::DeliveryReceipt>(renewed).attempt.lease_until ==
                    request.now + std::chrono::seconds{30},
            "current attempt renews by thirty seconds");
    target::DeliveryAcknowledgement finish;
    finish.attempt = second.attempt;
    finish.accepted = true;
    auto finished = work->events().acknowledge(finish, request.now);
    require(std::holds_alternative<target::DeliveryReceipt>(finished) &&
                std::get<target::DeliveryReceipt>(finished).disposition ==
                    target::DeliveryDisposition::Acknowledged,
            "renewed attempt acknowledged");
    require(work->commit() == target::CommitResult::Committed, "retry transaction commits");
  }

  // --- dispatcher timeout sweep, restart recovery and dead letters ---
  {
    auto work = begin();
    const auto record = event("event-dead", 5);
    auto accepted = work->events().accept(record, {target_for(record, "instance-c")}, limits);
    require(std::holds_alternative<target::EventAcceptanceResult>(accepted), "dead event accepted");
    base = now(work);
    target::DeliveryClaimRequest request;
    request.instance = {"instance-c"};
    request.fence = {{"instance-c"}, {"epoch"}, {"execution"}, {1}, {"connection"}, {1}};
    request.limit = 1;
    request.lease = target::kDeliveryLease;
    request.now = base;
    request.attempt_ids = {{"dead-attempt-1"}};
    auto claimed = work->events().claim(request);
    require(std::holds_alternative<std::vector<target::DeliveryClaim>>(claimed),
            "dead target claimed");
    auto swept = work->events().sweep(base + std::chrono::seconds{31}, target::RetryPolicy{}, 0.5);
    require(std::holds_alternative<target::DeliverySweep>(swept) &&
                std::get<target::DeliverySweep>(swept).expired_attempts == 1,
            "expired dispatcher attempt swept");
    auto recovered =
        work->events().recover(base + std::chrono::seconds{32}, target::RetryPolicy{}, 0.5);
    require(std::holds_alternative<std::size_t>(recovered) && std::get<std::size_t>(recovered) == 0,
            "restart recovery ignores non-dispatched rows");
    require(work->commit() == target::CommitResult::Committed, "sweep transaction commits");
  }

  // --- P10b AcceptTimerTrigger: durable/idempotent TimerTriggered acceptance and owner fan-out ---
  {
    FakeRuntimeIds ids;
    FakeRuntimeSecrets secrets;
    FakeRuntimeAuthorization authorization;
    auto built =
        target::make_runtime_resource_application({*factory, ids, secrets, authorization, {}});
    require(!std::holds_alternative<target::Failure>(built), "runtime application built");
    auto application =
        std::move(std::get<std::unique_ptr<target::RuntimeResourceApplication>>(built));
    auto time_work = begin();
    const auto base = now(time_work);
    time_work->rollback();
    const auto occurrence = uuid_like('9');
    const auto owner = uuid_like('8');
    const auto subject = uuid_like('7');
    target::AcceptTimerTriggerRequest request;
    request.context.request_id = uuid_like('3');
    request.context.trace_id = uuid_like('4');
    request.context.method = "kiarabot.v1.RuntimeService/AcceptTimerTrigger";
    request.context.deadline = base + std::chrono::seconds{5};
    request.context.authentication.subject = target::SubjectId{subject};
    request.context.authentication.direct_peer =
        target::PlatformPeer{target::Service::Scheduler, target::SubjectId{subject}};
    request.context.authentication.audience = target::Service::Runtime;
    request.context.authentication.authorization_version = {1};
    request.context.authentication.verified_at = base - std::chrono::seconds{1};
    request.context.authentication.expires_at = base + std::chrono::seconds{60};
    request.occurrence_id = target::MessageId{occurrence};
    request.task_id = "task-1";
    request.owner_instance_id = target::InstanceId{owner};
    request.schedule_version = {1};
    request.conversation_id = "conversation";
    request.conversation_kind = target::ConversationKey::Kind::Private;
    request.serialized_envelope = "timer-envelope";
    request.handoff.source = target::Service::Scheduler;
    request.handoff.message = target::MessageId{occurrence};
    request.handoff.operation =
        target::OperationRef{target::Service::Scheduler, target::OperationId{uuid_like('6')},
                             target::ResourceRef{target::ResourceKind::Task, "task-1"}};
    request.handoff.resource = target::ResourceRef{target::ResourceKind::Task, "task-1"};
    request.handoff.version = {1};
    request.handoff.actor = target::SubjectId{owner};
    request.handoff.authorization_version = {1};
    // SHA-256 of the empty Scheduler payload (the request carries no payload in this fixture).
    constexpr std::array<unsigned char, 32> kEmptySha256{
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
        0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
        0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
    for (std::size_t index = 0; index < kEmptySha256.size(); ++index) {
      request.handoff.payload_hash.bytes[index] = static_cast<std::byte>(kEmptySha256[index]);
    }
    request.handoff.created_at = base - std::chrono::seconds{1};
    request.handoff.valid_until = base + std::chrono::hours{1};
    auto accepted = application->accept_timer_trigger(request);
    require(std::holds_alternative<target::TimerTriggerAcceptance>(accepted) &&
                !std::get<target::TimerTriggerAcceptance>(accepted).duplicate,
            "timer trigger durably accepted");
    auto duplicate = application->accept_timer_trigger(request);
    require(std::holds_alternative<target::TimerTriggerAcceptance>(duplicate) &&
                std::get<target::TimerTriggerAcceptance>(duplicate).duplicate,
            "same occurrence/event id is duplicate without a second enqueue");
    // Transport-only envelope differences do not change the handoff idempotency identity.
    auto same_content = request;
    same_content.serialized_envelope = "different-envelope";
    auto duplicate_envelope = application->accept_timer_trigger(same_content);
    require(std::holds_alternative<target::TimerTriggerAcceptance>(duplicate_envelope) &&
                std::get<target::TimerTriggerAcceptance>(duplicate_envelope).duplicate,
            "envelope-only change remains a duplicate");
    auto changed = request;
    changed.task_id = "task-2";
    changed.handoff.operation.resource = {target::ResourceKind::Task, {"task-2"}};
    changed.handoff.resource = {target::ResourceKind::Task, {"task-2"}};
    auto conflict = application->accept_timer_trigger(changed);
    require(std::holds_alternative<target::Failure>(conflict) &&
                std::get<target::Failure>(conflict).code == target::ErrorCode::Conflict,
            "changed handoff content for the same event id is Conflict");
    auto expired = request;
    expired.occurrence_id = target::MessageId{uuid_like('5')};
    expired.handoff.message = expired.occurrence_id;
    expired.handoff.valid_until = base - std::chrono::seconds{1};
    auto timed_out = application->accept_timer_trigger(expired);
    require(std::holds_alternative<target::Failure>(timed_out) &&
                std::get<target::Failure>(timed_out).code == target::ErrorCode::Timeout,
            "handoff past valid_until is Timeout");
    auto mismatch = request;
    mismatch.handoff.payload_hash.bytes[0] = std::byte{0};
    auto mismatched = application->accept_timer_trigger(mismatch);
    require(std::holds_alternative<target::Failure>(mismatched) &&
                std::get<target::Failure>(mismatched).code == target::ErrorCode::Conflict,
            "payload hash mismatch is Conflict");
    auto wrong_source = request;
    wrong_source.handoff.source = target::Service::Runtime;
    auto rejected = application->accept_timer_trigger(wrong_source);
    require(std::holds_alternative<target::Failure>(rejected) &&
                std::get<target::Failure>(rejected).code == target::ErrorCode::InvalidArgument,
            "non-Scheduler handoff is InvalidArgument");
    // The accepted event has exactly the owning instance as fan-out target.
    {
      auto work = begin();
      target::DeliveryClaimRequest claim;
      claim.instance = {owner};
      claim.fence = {{owner}, {"epoch"}, {"execution"}, {1}, {"connection"}, {1}};
      claim.limit = 1;
      claim.lease = target::kDeliveryLease;
      claim.now = base + std::chrono::seconds{1};
      claim.attempt_ids = {{uuid_like('0')}};
      auto claimed = work->events().claim(claim);
      require(std::holds_alternative<std::vector<target::DeliveryClaim>>(claimed) &&
                  std::get<std::vector<target::DeliveryClaim>>(claimed).size() == 1 &&
                  std::get<std::vector<target::DeliveryClaim>>(claimed)[0].event.key.event_id ==
                      occurrence &&
                  std::get<std::vector<target::DeliveryClaim>>(claimed)[0]
                          .attempt.delivery.instance.value == owner,
              "timer event fan-out targets the owner instance exactly once");
      work->rollback();
    }
  }

  std::cout << "P06 Runtime event inbox, watermark, delivery and lease passed\n";
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
    std::cerr << "P06 assertion: " << error.what() << '\n';
    return 1;
  } catch (const std::exception&) {
    std::cerr << "P06 Runtime event storage test failed (details redacted)\n";
    return 1;
  }
}
