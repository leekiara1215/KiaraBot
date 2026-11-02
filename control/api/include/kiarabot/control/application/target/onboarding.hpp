#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::control::target::onboarding {

// Identifiers are opaque catalog keys, not executable names or command lines.
struct Selection {
  std::string channel;
  std::string protocol;
  std::string implementation;
  std::string adapter;
  enum class Mode { Managed, Existing } mode;
};

// Runtime remains the sole owner of these business identities and their relationships.
struct AccountBinding {
  core::target::ResourceId account;
  std::string platform_account_id;
  core::target::ResourceId adapter;
};

enum class Step { Detect, Install, Configure, Start, Login, CheckConnection, Stop };
enum class InteractionKind { QrCode, DeviceCode, AuthorizationUrl, ExistingToken };
enum class FieldKind { Text, Boolean, SecretReference };
struct SecretReference {
  std::string id;
};
using FieldValue = std::variant<std::string, bool, SecretReference>;
struct ConfigField {
  std::string key;
  FieldKind kind;
  bool required;
};
struct ConfigValue {
  std::string key;
  FieldValue value;
};
struct Capabilities {
  Selection selection;
  std::vector<Step> steps;
  std::vector<InteractionKind> interactions;
  std::vector<ConfigField> fields;
};

enum class ObservationKind {
  Installed,
  ProcessRunning,
  PlatformLoggedIn,
  ProtocolConnected,
  AdapterRegistered,
  MessageVerified
};
enum class ObservationState { Unknown, No, Yes };
struct Observation {
  ObservationKind kind;
  ObservationState state;
  core::target::TimePoint observed_at;
};
// Ephemeral challenge content must not be logged or stored in persistent operation records.
struct Challenge {
  InteractionKind kind;
  std::string content;
  core::target::TimePoint expires_at;
};
struct RequestContext {
  core::target::OperationId operation_id;
  core::target::TimePoint deadline;
  // Borrowed for the duration of a call; the caller owns and may set it concurrently.
  const std::atomic_bool& cancelled;
};

// Validates catalog selection and configuration before dispatch; does not authorize a caller.
core::target::Status validate(const Capabilities& capabilities, const Selection& selection,
                              Step step, const std::vector<ConfigValue>& config);

// Implementations return stable Failure codes and observe the deadline/cancellation. A timeout
// or cancellation after a side effect must report Acceptance::Unknown when the result is unknown.
// Calling code owns authorization, durable operations, idempotency and recovery. Implementations
// must not remove user data or manage an existing service's process. No shell or executable input.
class Driver {
public:
  virtual ~Driver() = default;
  virtual Capabilities capabilities() const = 0;
  virtual core::target::Status execute(const RequestContext& context, Step step,
                                       const std::vector<ConfigValue>& config) = 0;
  virtual core::target::Outcome<std::vector<Observation>>
  observe(const RequestContext& context) = 0;
  virtual core::target::Outcome<std::optional<Challenge>>
  challenge(const RequestContext& context) = 0;
};

} // namespace kiarabot::control::target::onboarding
