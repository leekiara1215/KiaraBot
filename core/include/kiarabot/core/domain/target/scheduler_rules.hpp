#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/core/domain/target/service_records.hpp"

namespace kiarabot::core::target {

// Frozen P10 one-shot task limits.
inline constexpr auto kTaskHorizon = std::chrono::hours{24 * 366};
inline constexpr std::size_t kInstanceTaskCapacity = 1024;
inline constexpr std::size_t kInstallationTaskCapacity = 8192;
inline constexpr std::uint32_t kMaxTriggerAttempts = 100;
inline constexpr auto kTriggerValidFor = std::chrono::hours{24 * 7};
inline constexpr std::size_t kMaximumPayloadBytes = 65536;

// Civil time exactly as it arrives from the boundary; never normalized locally before the IANA
// resolver decides whether it is valid/unique in the named zone.
struct LocalCivilTime {
  std::int32_t year = 0;
  std::int32_t month = 0;
  std::int32_t day = 0;
  std::int32_t hour = 0;
  std::int32_t minute = 0;
  std::int32_t second = 0;
  std::string time_zone;
};

struct TaskCreationInput {
  std::string task_id;
  ConversationKey target;
  std::vector<std::byte> payload;
  std::optional<TimePoint> run_at;
  std::optional<LocalCivilTime> local_time;
};

Failure scheduler_invalid(Recovery recovery = Recovery::CorrectInput);
Failure scheduler_conflict();
Failure scheduler_not_found();
Failure scheduler_unavailable(Recovery recovery = Recovery::Backoff);
Failure scheduler_limited();

bool canonical_id(std::string_view value);
bool task_id_text(std::string_view value);
bool task_terminal(TaskState state);
bool occurrence_terminal(OperationState state);

// Exactly one of run_at/local_time, strictly future and within the horizon. local_time is resolved
// only through the injected IANA port: nonexistent/ambiguous civil times and invalid zones are
// INVALID_ARGUMENT, unavailable zone data is UPSTREAM_UNAVAILABLE (never a guessed UTC offset).
Outcome<TimePoint> resolve_run_at(const TaskCreationInput& input, class SchedulerTimeZonePort& zones,
                                  TimePoint now, std::chrono::hours horizon = kTaskHorizon);

// Deterministic owner-bound fingerprint of creation fields (excluding transport ids/deadline).
Sha256Digest creation_fingerprint(const TaskCreationInput& input);

} // namespace kiarabot::core::target
