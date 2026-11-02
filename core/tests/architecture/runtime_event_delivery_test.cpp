#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/event_delivery.hpp"

namespace target = kiarabot::core::target;
using std::chrono_literals::operator""s;

void check(bool passed, const char* scenario) {
  if (!passed) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}

target::DeliveryRecord delivery(std::string event_id = "event", std::string instance = "instance",
                                std::uint32_t attempts = 0) {
  target::DeliveryRecord record;
  record.delivery.event.source.value = "adapter";
  record.delivery.event.event_id = std::move(event_id);
  record.delivery.instance.value = "instance-" + instance;
  record.trace_id = "trace";
  record.attempts = attempts;
  return record;
}

target::DeliveryAttempt attempt(const target::DeliveryRecord& record, std::string id,
                                target::TimePoint started, std::chrono::seconds lease = 30s) {
  return target::DeliveryAttempt{record.delivery,
                                 {std::move(id)},
                                 {{"instance"}, {"epoch"}, {"execution"}, {1}, {"connection"}, {1}},
                                 started,
                                 started + lease};
}

int main() {
  // --- conversation key: length-prefixed encoding cannot alias ---
  target::ConversationKey first{target::ResourceId{"a"}, target::ResourceId{"bc"}, "conversation",
                                target::ConversationKey::Kind::Group};
  target::ConversationKey second{target::ResourceId{"ab"}, target::ResourceId{"c"}, "conversation",
                                 target::ConversationKey::Kind::Group};
  check(target::conversation_key_text(first) != target::conversation_key_text(second),
        "conversation key parts cannot alias");
  const auto parsed = target::conversation_key_from_text(target::conversation_key_text(first));
  check(parsed.has_value() && *parsed == first, "conversation key text round-trips");
  check(!target::conversation_key_from_text("4:abc").has_value(),
        "truncated conversation key rejected");
  check(!target::conversation_key_from_text("1:a1:b4:conv1:9").has_value(),
        "unknown conversation kind rejected");

  // --- sequence watermark ---
  auto decision = target::observe_sequence(std::nullopt, std::uint64_t{1});
  check(decision.verdict == target::SequenceVerdict::Accepted &&
            decision.watermark.last_sequence == 1 && !decision.watermark.degraded,
        "first sequence 1 accepted contiguously");
  decision = target::observe_sequence(std::nullopt, std::uint64_t{3});
  check(decision.verdict == target::SequenceVerdict::Accepted && decision.watermark.degraded,
        "first sequence jump degrades");
  target::SequenceWatermark watermark{{2}, false};
  decision = target::observe_sequence(watermark, std::uint64_t{2});
  check(decision.verdict == target::SequenceVerdict::OutOfOrder,
        "non-advancing sequence rejected as out of order");
  decision = target::observe_sequence(watermark, std::uint64_t{1});
  check(decision.verdict == target::SequenceVerdict::OutOfOrder,
        "lower sequence rejected as out of order");
  decision = target::observe_sequence(watermark, std::uint64_t{4});
  check(decision.verdict == target::SequenceVerdict::Accepted &&
            decision.watermark.last_sequence == 4 && decision.watermark.degraded,
        "sequence gap advances and degrades");
  decision = target::observe_sequence(watermark, std::nullopt);
  check(decision.verdict == target::SequenceVerdict::Accepted && decision.watermark.degraded &&
            decision.watermark.last_sequence == 2,
        "missing sequence degrades without advancing");
  decision = target::observe_sequence(target::SequenceWatermark{{5}, true}, std::uint64_t{6});
  check(decision.watermark.degraded, "degraded watermark is sticky");

  // --- retry schedule: 1/2/4/8/16s +/-20%, first delivery plus five retries ---
  const auto exact = [](std::uint32_t retry_number) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               target::retry_delay(retry_number, 0.5))
        .count();
  };
  check(exact(1) == 1000 && exact(2) == 2000 && exact(3) == 4000 && exact(4) == 8000 &&
            exact(5) == 16000,
        "retry delays are 1/2/4/8/16 seconds");
  check(
      std::chrono::duration_cast<std::chrono::milliseconds>(target::retry_delay(1, 0.0)).count() ==
          800,
      "retry delay lower jitter bound is -20%");
  check(
      std::chrono::duration_cast<std::chrono::milliseconds>(target::retry_delay(2, 1.0)).count() ==
          2400,
      "retry delay upper jitter bound is +20%");
  check(target::clamp_jitter(-1.0) == 0.0 && target::clamp_jitter(2.0) == 1.0,
        "jitter sample is clamped to [0,1]");

  const target::TimePoint now{std::chrono::seconds{100}};
  auto failed = target::fail_delivery(delivery("event", "instance", 1), true,
                                      target::ErrorCode::Timeout, now, 0.5);
  check(failed.state == target::DeliveryState::Retrying && failed.next_attempt_at.has_value() &&
            *failed.next_attempt_at == now + 1s,
        "first retryable failure backs off one second");
  failed = target::fail_delivery(delivery("event", "instance", 5), true, target::ErrorCode::Timeout,
                                 now, 0.5);
  check(failed.state == target::DeliveryState::Retrying && *failed.next_attempt_at == now + 16s,
        "fifth retryable failure still retries with 16s delay");
  failed = target::fail_delivery(delivery("event", "instance", 6), true, target::ErrorCode::Timeout,
                                 now, 0.5);
  check(failed.state == target::DeliveryState::DeadLetter && !failed.next_attempt_at &&
            failed.dead_letter_reason == target::ErrorCode::Timeout,
        "exhausted retries dead-letter after five retries");
  failed = target::fail_delivery(delivery("event", "instance", 1), false,
                                 target::ErrorCode::PermissionDenied, now, 0.5);
  check(failed.state == target::DeliveryState::DeadLetter &&
            failed.dead_letter_reason == target::ErrorCode::PermissionDenied,
        "non-retryable failure dead-letters immediately");

  // --- claim eligibility ---
  target::StoredDelivery stored{delivery(), target::DeliveryState::Persisted, now, std::nullopt,
                                std::nullopt};
  check(target::delivery_due(stored, now), "persisted delivery is due at next_attempt_at");
  stored.next_attempt_at = now + 1s;
  check(!target::delivery_due(stored, now), "retrying delivery waits for backoff");
  stored.state = target::DeliveryState::Dispatched;
  stored.next_attempt_at = now;
  check(!target::delivery_due(stored, now), "dispatched delivery is not re-claimed");

  // --- ACK/Renew classification ---
  const auto record = delivery();
  const auto current = attempt(record, "attempt-1", now - 1s);
  target::DeliveryAttempt stale = current;
  stale.attempt = {"attempt-0"};
  check(target::classify_acknowledgement(target::DeliveryState::Acknowledged, current, current,
                                         now) == target::DeliveryDisposition::Duplicate,
        "same attempt after acknowledgement is duplicate");
  check(target::classify_acknowledgement(target::DeliveryState::Dispatched, current, current,
                                         now) == target::DeliveryDisposition::Acknowledged,
        "current active attempt ACKs");
  check(target::classify_acknowledgement(target::DeliveryState::Dispatched, current, stale, now) ==
            target::DeliveryDisposition::Stale,
        "old attempt cannot confirm the new attempt");
  check(target::classify_acknowledgement(target::DeliveryState::Dispatched, current, current,
                                         now + 30s) == target::DeliveryDisposition::Stale,
        "expired lease ACK is stale");
  auto wrong_fence = current;
  wrong_fence.fence.connection_generation.value = 2;
  check(target::classify_acknowledgement(target::DeliveryState::Dispatched, current, wrong_fence,
                                         now) == target::DeliveryDisposition::Stale,
        "stale fence ACK is stale");
  check(target::classify_renewal(current, current, now) == target::DeliveryDisposition::Renewed,
        "current active attempt renews");
  check(target::classify_renewal(current, stale, now) == target::DeliveryDisposition::Stale,
        "old attempt cannot renew the new attempt");
  const auto extended = target::renewed_lease_until(current, now);
  check(extended == now + 30s, "renew extends by at most 30 seconds");
  const auto old = attempt(record, "attempt-1", now - 290s);
  check(target::renewed_lease_until(old, now) == old.started_at + 300s,
        "renew caps at started_at+300 seconds");
  check(target::classify_renewal(old, old, old.started_at + 300s) ==
            target::DeliveryDisposition::Stale,
        "renew at lifetime cap is stale");

  // --- target selection and capacity ---
  const auto active = [](std::string id, std::uint64_t desired, std::uint64_t applied,
                         bool registered) {
    target::PluginInstance instance{
        {"instance-" + id},
        {"installation"},
        {"plugin"},
        {desired},
        {applied},
        registered ? std::optional<target::Fence>(target::Fence{
                         {"instance-" + id}, {"epoch"}, {"execution"}, {1}, {"connection"}, {1}})
                   : std::nullopt,
        {1},
        target::LifecycleState::Active,
        std::nullopt};
    return instance;
  };
  std::vector<target::PluginInstance> instances{active("b", 1, 1, true), active("a", 0, 0, true),
                                                active("c", 2, 1, true), active("d", 1, 1, false)};
  std::vector<std::string> subscriptions{"event.message.received", "event.message.received"};
  const auto targets = target::delivery_targets(instances, "event.message.received", subscriptions);
  check(targets.size() == 2 && targets[0].value == "instance-a" && targets[1].value == "instance-b",
        "fan-out targets are eligible, unique and sorted");
  check(target::delivery_targets(instances, "event.unknown", subscriptions).empty(),
        "unknown subscription has no targets");
  target::EventCapacity capacity{1023, 1023, false, false};
  check(target::capacity_available(capacity), "capacity below limits accepts");
  capacity.events_saturated = true;
  check(!target::capacity_available(capacity), "event capacity saturation rejects");

  // --- restart recovery and dead-letter replay ---
  target::StoredDelivery dispatched{delivery("event", "instance", 2),
                                    target::DeliveryState::Dispatched, now, std::nullopt,
                                    std::nullopt};
  const auto recovered = target::recover_dispatched(dispatched, now, 0.5);
  check(recovered.state == target::DeliveryState::Retrying &&
            *recovered.next_attempt_at == now + 2s,
        "dispatched attempt recovered as retryable failure");
  target::StoredDelivery dead{delivery(), target::DeliveryState::DeadLetter, now,
                              target::ErrorCode::Timeout, std::nullopt};
  check(std::holds_alternative<std::monostate>(target::replay_dead_letter(dead)),
        "dead-letter delivery is replayable");
  check(std::holds_alternative<target::Failure>(target::replay_dead_letter(dispatched)),
        "non-dead-letter delivery rejects replay");

  std::cout << "P06 event/delivery rules passed\n";
  return 0;
}
