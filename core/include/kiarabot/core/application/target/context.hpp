#pragma once

#include "kiarabot/core/domain/target/context.hpp"

namespace kiarabot::core::target {

struct CommandContext {
  AuthenticationContext authentication;
  std::string request_id;
  std::string trace_id;
  std::string method;
  ResourceId resource;
  std::string idempotency_key;
  std::string fingerprint;
  TimePoint deadline;
  std::optional<ResourceVersion> expected_version;
};

} // namespace kiarabot::core::target
