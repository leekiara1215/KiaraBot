#include "kiarabot/core/domain/target/scheduler_rules.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

#include <openssl/sha.h>

#include "kiarabot/core/ports/target/scheduler_ports.hpp"

namespace kiarabot::core::target {
namespace {

void append(std::vector<std::byte>& out, std::string_view value) {
  const auto length = static_cast<std::uint32_t>(value.size());
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::byte>((length >> shift) & 0xffU));
  }
  for (const char character : value) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
}
void append_u64(std::vector<std::byte>& out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xffULL));
  }
}
void append_i32(std::vector<std::byte>& out, std::int32_t value) {
  append_u64(out, static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)));
}
bool printable(std::string_view value) {
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char character) { return character >= 32 && character < 127; });
}

} // namespace

Failure scheduler_invalid(Recovery recovery) {
  return {ErrorCode::InvalidArgument, Acceptance::NotAccepted, recovery, std::nullopt,
          std::nullopt};
}
Failure scheduler_conflict() {
  return {ErrorCode::Conflict, Acceptance::NotAccepted, Recovery::ReadCurrent, std::nullopt,
          std::nullopt};
}
Failure scheduler_not_found() {
  return {ErrorCode::NotFound, Acceptance::NotAccepted, Recovery::ReadCurrent, std::nullopt,
          std::nullopt};
}
Failure scheduler_unavailable(Recovery recovery) {
  return {ErrorCode::UpstreamUnavailable, Acceptance::NotAccepted, recovery, std::nullopt,
          std::nullopt};
}
Failure scheduler_limited() {
  return {ErrorCode::RateLimited, Acceptance::NotAccepted, Recovery::Backoff, std::nullopt,
          std::nullopt};
}

bool canonical_id(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
      continue;
    }
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool task_id_text(std::string_view value) {
  return !value.empty() && value.size() <= 128 && printable(value);
}

bool task_terminal(TaskState state) {
  return state == TaskState::Completed || state == TaskState::Failed ||
         state == TaskState::Cancelled;
}

bool occurrence_terminal(OperationState state) {
  return state == OperationState::Succeeded || state == OperationState::Failed ||
         state == OperationState::Cancelled || state == OperationState::Expired;
}

Outcome<TimePoint> resolve_run_at(const TaskCreationInput& input, SchedulerTimeZonePort& zones,
                                  TimePoint now, std::chrono::hours horizon) {
  if (!task_id_text(input.task_id) || (input.run_at.has_value() == input.local_time.has_value()) ||
      input.payload.size() > kMaximumPayloadBytes || input.target.conversation_id.empty() ||
      input.target.conversation_id.size() > 128 || !printable(input.target.conversation_id)) {
    return scheduler_invalid();
  }
  TimePoint run_at;
  if (input.run_at) {
    run_at = *input.run_at;
  } else {
    if (!zones.available()) {
      return scheduler_unavailable(Recovery::Backoff);
    }
    auto resolved = zones.resolve(*input.local_time);
    if (const auto* failure = std::get_if<Failure>(&resolved)) {
      return *failure;
    }
    run_at = std::get<TimePoint>(resolved);
  }
  if (run_at <= now || run_at > now + horizon) {
    return scheduler_invalid();
  }
  return run_at;
}

Sha256Digest creation_fingerprint(const TaskCreationInput& input) {
  std::vector<std::byte> canonical;
  append(canonical, input.task_id);
  append(canonical, input.target.adapter.value);
  append(canonical, input.target.account.value);
  append(canonical, input.target.conversation_id);
  append_u64(canonical, static_cast<std::uint64_t>(input.target.kind));
  append_u64(canonical, input.payload.size());
  canonical.insert(canonical.end(), input.payload.begin(), input.payload.end());
  append_i32(canonical, input.run_at ? 1 : 0);
  if (input.run_at) {
    append_u64(canonical, static_cast<std::uint64_t>(input.run_at->time_since_epoch().count()));
  } else {
    append_i32(canonical, input.local_time->year);
    append_i32(canonical, input.local_time->month);
    append_i32(canonical, input.local_time->day);
    append_i32(canonical, input.local_time->hour);
    append_i32(canonical, input.local_time->minute);
    append_i32(canonical, input.local_time->second);
    append(canonical, input.local_time->time_zone);
  }
  Sha256Digest digest;
  SHA256(reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size(),
         reinterpret_cast<unsigned char*>(digest.bytes.data()));
  return digest;
}

} // namespace kiarabot::core::target
