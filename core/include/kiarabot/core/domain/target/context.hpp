#pragma once

#include <string>
#include <variant>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

// Platform service vocabulary only; also used for durable handoff sources/audiences.
struct PlatformPeer {
  Service service;
  SubjectId subject;
};
struct AdapterPeer {
  ResourceId adapter;
  SubjectId subject;
};
struct PluginPeer {
  InstanceId instance;
  SubjectId subject;
};
// Immediate authenticated peer, distinct from the represented/delegated subject below.
// Adapter/Plugin identities come from verified registration bindings, not request claims.
using DirectPeer = std::variant<PlatformPeer, AdapterPeer, PluginPeer>;
struct ResourceScope {
  ResourceRef resource;
  std::string permission;
};
// Constructed only after boundary authentication; never deserialize caller claims into this type.
// Permission strings must come from the closed target contract vocabulary, unknown values deny.
struct AuthenticationContext {
  SubjectId subject;
  DirectPeer direct_peer;
  Service audience;
  std::vector<SubjectId> actors;
  std::vector<ResourceScope> scopes;
  ResourceVersion authorization_version;
  TimePoint verified_at;
  TimePoint expires_at;
};
// Durable audit/execution evidence only; contains no reusable credentials.
struct HandoffContext {
  Service source;
  MessageId message;
  OperationRef operation;
  std::optional<MessageId> causation;
  ResourceRef resource;
  ResourceVersion version;
  SubjectId actor;
  ResourceVersion authorization_version;
  Sha256Digest payload_hash;
  TimePoint created_at;
  TimePoint valid_until;
};

} // namespace kiarabot::core::target
