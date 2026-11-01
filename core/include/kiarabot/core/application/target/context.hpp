#pragma once

#include <string>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

enum class Service { Identity, Runtime, Host, Scheduler, Model, Control };
struct ResourceScope {
  ResourceId resource;
  std::string permission;
};
// Constructed only after boundary authentication; never deserialize caller claims into this type.
// Permission strings must come from the closed target contract vocabulary, unknown values deny.
struct AuthenticationContext {
  SubjectId subject;
  Service direct_service;
  Service audience;
  std::vector<SubjectId> actors;
  std::vector<ResourceScope> scopes;
  ResourceVersion authorization_version;
  TimePoint verified_at;
  TimePoint expires_at;
};
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
// Durable audit/execution evidence only; contains no reusable credentials.
struct HandoffContext {
  Service source;
  MessageId message;
  OperationId operation;
  std::optional<MessageId> causation;
  ResourceId resource;
  ResourceVersion version;
  SubjectId actor;
  ResourceVersion authorization_version;
  std::string payload_hash;
  TimePoint created_at;
  TimePoint valid_until;
};

} // namespace kiarabot::core::target
