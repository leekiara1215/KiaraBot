// P06 defect A regression: a real AdapterBridge stream end ends durable adapter presence
// immediately instead of waiting for the 90s online-drain window. The projection write is an
// idempotent owner-repository operation: it must flip online_until to the disconnect sample and
// bump the version exactly once, restore online on the next registration (normal registration
// path), ignore absent/deleted/owner-mismatched/stale rows, and surface storage faults.
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/application/target/runtime_bridge_application.hpp"

namespace kiarabot::core::target {
namespace {

int failures = 0;

void check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "bridge presence assertion failed: %s\n", message);
    ++failures;
  }
}

constexpr std::string_view kAdapter{"22222222-2222-4222-8222-222222222222"};
constexpr std::string_view kInstance{"33333333-3333-4333-8333-333333333333"};
constexpr std::string_view kForeignInstance{"44444444-4444-4444-8444-444444444444"};

TimePoint at(std::int64_t seconds) {
  return TimePoint{std::chrono::seconds{seconds}};
}

AdapterRecord online_record(ConnectionGeneration generation, TimePoint now) {
  AdapterRecord record;
  record.adapter = ResourceId{std::string{kAdapter}};
  record.current.instance = InstanceId{std::string{kInstance}};
  record.current.runtime_epoch = RuntimeEpoch{"epoch-1"};
  record.current.connection = ConnectionId{""
                                           "connection-1"};
  record.current.connection_generation = generation;
  record.version = ResourceVersion{1};
  record.last_registered_at = now;
  record.last_heartbeat_at = now;
  record.online_until = now + std::chrono::seconds{90};
  return record;
}

class FakeAdapters final : public AdapterRegistryRepository {
public:
  std::optional<AdapterRecord> record;
  bool fail_read = false;
  bool fail_save = false;
  int pending_conflicts = 0;
  int saves = 0;

  Outcome<std::optional<AdapterRecord>> find(const ResourceId& adapter) override {
    if (fail_read) {
      return Failure{ErrorCode::UpstreamUnavailable, Acceptance::NotAccepted, Recovery::Backoff,
                     std::nullopt, std::nullopt};
    }
    if (!record || record->adapter != adapter) {
      return std::optional<AdapterRecord>{};
    }
    return std::optional<AdapterRecord>{*record};
  }
  Outcome<std::optional<AdapterRecord>> by_instance(const InstanceId& instance) override {
    if (fail_read) {
      return Failure{ErrorCode::UpstreamUnavailable, Acceptance::NotAccepted, Recovery::Backoff,
                     std::nullopt, std::nullopt};
    }
    if (!record || record->current.instance != instance) {
      return std::optional<AdapterRecord>{};
    }
    return std::optional<AdapterRecord>{*record};
  }
  Status save(const AdapterRecord& value, std::optional<ResourceVersion> expected) override {
    ++saves;
    if (fail_save) {
      return Failure{ErrorCode::UpstreamUnavailable, Acceptance::NotAccepted, Recovery::Backoff,
                     std::nullopt, std::nullopt};
    }
    if (pending_conflicts > 0) {
      --pending_conflicts;
      return Failure{ErrorCode::Conflict, Acceptance::NotAccepted, Recovery::ReadCurrent,
                     std::nullopt, std::nullopt};
    }
    if (!expected || !record || record->version.value != expected->value ||
        value.version.value != expected->value + 1) {
      return Failure{ErrorCode::Conflict, Acceptance::NotAccepted, Recovery::ReadCurrent,
                     std::nullopt, std::nullopt};
    }
    record = value;
    return std::monostate{};
  }
};

std::optional<ErrorCode> failure_code(const Outcome<bool>& outcome) {
  const auto* failure = std::get_if<Failure>(&outcome);
  if (failure == nullptr) {
    return std::nullopt;
  }
  return failure->code;
}

void online_then_offline() {
  FakeAdapters adapters;
  adapters.record = online_record(ConnectionGeneration{1}, at(1000));
  const auto online_until = adapters.record->online_until;
  const auto version = adapters.record->version;
  auto ended = end_adapter_presence(adapters, InstanceId{std::string{kInstance}},
                                    ConnectionGeneration{1}, at(1000));
  check(std::holds_alternative<bool>(ended) && std::get<bool>(ended),
        "current connection did not close presence");
  check(adapters.record->online_until == at(1000) &&
            adapters.record->online_until != online_until &&
            adapters.record->version.value == version.value + 1,
        "offline projection did not change online_until/version");
  check(adapters.saves == 1, "offline projection issued extra writes");
}

void repeated_disconnect_is_idempotent() {
  FakeAdapters adapters;
  adapters.record = online_record(ConnectionGeneration{1}, at(1000));
  check(std::get<bool>(end_adapter_presence(adapters, InstanceId{std::string{kInstance}},
                                            ConnectionGeneration{1}, at(1000))),
        "first disconnect did not close presence");
  const auto offline = *adapters.record;
  auto repeated = end_adapter_presence(adapters, InstanceId{std::string{kInstance}},
                                       ConnectionGeneration{1}, at(1000));
  check(std::holds_alternative<bool>(repeated) && !std::get<bool>(repeated),
        "repeated disconnect was not an idempotent no-op");
  check(adapters.record->online_until == offline.online_until &&
            adapters.record->version.value == offline.version.value && adapters.saves == 1,
        "repeated disconnect rewrote or re-bumped presence");
}

void stale_generation_cannot_flip_the_new_connection() {
  FakeAdapters adapters;
  adapters.record = online_record(ConnectionGeneration{2}, at(1000));
  // Replacement registration wrote generation 2; the old generation 1 teardown arrives late.
  auto stale = end_adapter_presence(adapters, InstanceId{std::string{kInstance}},
                                    ConnectionGeneration{1}, at(1000));
  check(std::holds_alternative<bool>(stale) && !std::get<bool>(stale),
        "stale generation teardown flipped presence");
  check(adapters.record->online_until > at(1000) && adapters.saves == 0,
        "stale generation teardown mutated the row");
  check(std::get<bool>(end_adapter_presence(adapters, InstanceId{std::string{kInstance}},
                                            ConnectionGeneration{2}, at(1000))),
        "current generation teardown did not close presence");
}

void absent_deleted_and_foreign_rows_are_ignored() {
  {
    FakeAdapters adapters;
    auto absent = end_adapter_presence(adapters, InstanceId{std::string{kInstance}},
                                       ConnectionGeneration{1}, at(1000));
    check(std::holds_alternative<bool>(absent) && !std::get<bool>(absent) &&
              !adapters.record.has_value() && adapters.saves == 0,
          "absent adapter row was created");
  }
  {
    FakeAdapters adapters;
    adapters.record = online_record(ConnectionGeneration{1}, at(1000));
    adapters.record->state = LifecycleState::Deleted;
    const auto before = *adapters.record;
    auto deleted = end_adapter_presence(adapters, InstanceId{std::string{kInstance}},
                                        ConnectionGeneration{1}, at(1000));
    check(std::holds_alternative<bool>(deleted) && !std::get<bool>(deleted) &&
              adapters.record->state == LifecycleState::Deleted &&
              adapters.record->online_until == before.online_until && adapters.saves == 0,
          "deleted adapter row was revived or rewritten");
  }
  {
    FakeAdapters adapters;
    adapters.record = online_record(ConnectionGeneration{1}, at(1000));
    adapters.record->current.instance = InstanceId{std::string{kForeignInstance}};
    const auto before = *adapters.record;
    auto foreign = end_adapter_presence(adapters, InstanceId{std::string{kInstance}},
                                        ConnectionGeneration{1}, at(1000));
    check(std::holds_alternative<bool>(foreign) && !std::get<bool>(foreign) &&
              adapters.record->current.instance == before.current.instance &&
              adapters.record->online_until == before.online_until && adapters.saves == 0,
          "owner-mismatched adapter row was retargeted or rewritten");
  }
}

void heartbeat_conflict_retries_the_offline_projection() {
  FakeAdapters adapters;
  adapters.record = online_record(ConnectionGeneration{1}, at(1000));
  adapters.pending_conflicts = 1; // a concurrent accepted heartbeat bumped the version
  auto ended = end_adapter_presence(adapters, InstanceId{std::string{kInstance}},
                                    ConnectionGeneration{1}, at(1000));
  check(std::holds_alternative<bool>(ended) && std::get<bool>(ended) && adapters.saves == 2,
        "presence write did not retry after a heartbeat version conflict");
  check(adapters.record->online_until == at(1000), "retried presence did not end the window");
  FakeAdapters exhausted_case;
  exhausted_case.record = online_record(ConnectionGeneration{1}, at(1000));
  exhausted_case.pending_conflicts = 3; // conflict never clears: bounded retries, then a fault
  auto exhausted = end_adapter_presence(exhausted_case, InstanceId{std::string{kInstance}},
                                        ConnectionGeneration{1}, at(1000));
  check(failure_code(exhausted) == ErrorCode::Conflict,
        "unresolved presence conflict did not fail with CONFLICT");
}

void storage_faults_are_reported() {
  FakeAdapters adapters;
  adapters.record = online_record(ConnectionGeneration{1}, at(1000));
  adapters.fail_read = true;
  auto read_fault = end_adapter_presence(adapters, InstanceId{std::string{kInstance}},
                                         ConnectionGeneration{1}, at(1000));
  check(failure_code(read_fault) == ErrorCode::UpstreamUnavailable,
        "presence read fault was not reported");
  adapters.fail_read = false;
  adapters.fail_save = true;
  auto write_fault = end_adapter_presence(adapters, InstanceId{std::string{kInstance}},
                                          ConnectionGeneration{1}, at(1000));
  check(failure_code(write_fault) == ErrorCode::UpstreamUnavailable,
        "presence write fault was not reported");
}

} // namespace
} // namespace kiarabot::core::target

int main() {
  using namespace kiarabot::core::target;
  online_then_offline();
  repeated_disconnect_is_idempotent();
  stale_generation_cannot_flip_the_new_connection();
  absent_deleted_and_foreign_rows_are_ignored();
  heartbeat_conflict_retries_the_offline_projection();
  storage_faults_are_reported();
  if (failures != 0) {
    std::fprintf(stderr, "bridge presence regression failed: %d assertion(s)\n", failures);
    return 1;
  }
  std::printf("bridge presence regression: ok\n");
  return 0;
}
