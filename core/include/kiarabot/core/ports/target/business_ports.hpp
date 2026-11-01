#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

struct EventKey {
  ResourceId source;
  std::string event_id;
};
struct DeliveryKey {
  EventKey event;
  InstanceId instance;
};
struct DeliveryAttempt {
  DeliveryKey delivery;
  AttemptId attempt;
  Fence fence;
  TimePoint started_at;
  TimePoint lease_until;
};
struct EventAcceptance {
  EventKey event;
  std::string fingerprint;
  ConversationKey conversation;
  std::optional<std::uint64_t> sequence;
  std::vector<InstanceId> targets;
};
// All methods participate in the caller's Runtime UoW, never open/commit another transaction.
class EventRepository {
public:
  virtual ~EventRepository() = default;
  virtual Outcome<bool> accept(const EventAcceptance& event) = 0; // true = same-content duplicate
  virtual Outcome<std::optional<DeliveryAttempt>> claim(const InstanceId& instance,
                                                        TimePoint now) = 0;
  virtual Status acknowledge(const DeliveryAttempt& attempt, TimePoint now) = 0;
  virtual Outcome<TimePoint> renew(const DeliveryAttempt& attempt, TimePoint now) = 0;
};
enum class ActionKind {
  SendMessage,
  ReactMessage,
  DeleteMessage,
  MuteMember,
  KickMember,
  ApproveRequest
};
struct QuotaReservation {
  std::string action_id;
  InstanceId owner;
  ActionKind kind;
  ConversationKey conversation;
  TimePoint accepted_at;
};
class ActionQuotaRepository {
public:
  virtual ~ActionQuotaRepository() = default;
  // Action + receipt + quota + budget + audit must commit together; duplicates never charge twice.
  virtual Status reserve(const QuotaReservation& reservation) = 0;
};
struct PrivateNamespace {
  InstanceId instance;
};
struct SharedNamespace {
  InstallationId installation;
  ResourceId shared_namespace;
};
using NamespaceScope = std::variant<PrivateNamespace, SharedNamespace>;
struct ExpectedAbsent {};
using CompareCondition = std::variant<ExpectedAbsent, ResourceVersion>;
struct NamespaceValue {
  std::vector<std::byte> bytes;
  ResourceVersion version;
  std::optional<TimePoint> expires_at;
};
class NamespaceRepository {
public:
  virtual ~NamespaceRepository() = default;
  virtual Outcome<std::optional<NamespaceValue>> get(const NamespaceScope& scope,
                                                     const std::string& key, TimePoint now) = 0;
  // Scope is server-resolved after authorization; CAS/membership/quota/receipt share the UoW.
  virtual Outcome<ResourceVersion> compare_exchange(const NamespaceScope& scope,
                                                    const std::string& key,
                                                    const CompareCondition& expected,
                                                    const NamespaceValue& value) = 0;
};
class OccurrenceRepository {
public:
  virtual ~OccurrenceRepository() = default;
  virtual Status prepare(const Occurrence& occurrence) = 0;
  // Only persistent Runtime acceptance permits Scheduler completion.
  virtual Status confirm_accepted(const MessageId& occurrence) = 0;
};
class ModelIntentRepository {
public:
  virtual ~ModelIntentRepository() = default;
  virtual Outcome<std::optional<ModelIntent>> find(const InstanceId& owner,
                                                   const std::string& call_id) = 0;
  // Persist Started before Provider invocation; repeats never authorize another invocation.
  virtual Status insert(const ModelIntent& intent) = 0;
  virtual Status record_outcome(const InstanceId& owner, const std::string& call_id,
                                ModelCallState outcome) = 0;
};
// Future service-local UoWs expose the relevant repository group alongside receipts/journal.
// These contracts do not authorize cross-service database access or remote calls in a UoW.

} // namespace kiarabot::core::target
