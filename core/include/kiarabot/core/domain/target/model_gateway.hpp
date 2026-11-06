#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/core/domain/target/identity_permissions.hpp"
#include "kiarabot/core/domain/target/identity_types.hpp"
#include "kiarabot/core/domain/target/service_records.hpp"

namespace kiarabot::core::target {

// Frozen P11a limits. They are trusted application policy, never enlarged from request fields.
inline constexpr std::size_t kModelMaximumMessages = 128;
inline constexpr std::size_t kModelMaximumRequestBytes = 65536;
inline constexpr std::size_t kModelMaximumResultBytes = 65536;
inline constexpr std::size_t kModelMaximumProviderResponseBytes = 1024 * 1024;
inline constexpr std::size_t kModelMaximumProviderRequestId = 128;
inline constexpr std::size_t kModelMaximumAlias = 128;
inline constexpr std::size_t kModelMaximumEndpoint = 2048;
inline constexpr std::size_t kModelMaximumProviderModel = 256;
inline constexpr std::size_t kModelMaximumSecretBytes = 4096;
inline constexpr std::uint32_t kModelMinimumTimeoutMs = 1000;
inline constexpr std::uint32_t kModelMaximumTimeoutMs = 30000;
inline constexpr std::uint32_t kModelDefaultTimeoutMs = 5000;
inline constexpr std::uint32_t kModelMaximumConcurrency = 64;
inline constexpr std::uint32_t kModelDefaultConcurrency = 4;
inline constexpr std::uint32_t kModelMaximumLatencyMs = 300000;
inline constexpr auto kModelCallRetention = std::chrono::hours{24 * 7};
inline constexpr auto kModelUnknownResolutionWindow = std::chrono::hours{24 * 30};
// Closed v1 provider vocabulary. A string wire value outside this set is CAPABILITY_NOT_SUPPORTED.
inline constexpr std::string_view kModelProviderKindOpenAiChatCompletions =
    "openai_chat_completions";

// Wire replay shape for a duplicate logical_call_id (mirrors the P11 contract enum).
enum class ModelChatDisposition { Unspecified, FreshResult, ReplayInProgress, ReplayTerminal };

enum class ModelProviderKind { OpenAiChatCompletions };

inline std::optional<ModelProviderKind> model_provider_kind(std::string_view value) noexcept {
  if (value == kModelProviderKindOpenAiChatCompletions) {
    return ModelProviderKind::OpenAiChatCompletions;
  }
  return std::nullopt;
}
inline std::string_view model_provider_kind_name(ModelProviderKind kind) noexcept {
  switch (kind) {
  case ModelProviderKind::OpenAiChatCompletions:
    return kModelProviderKindOpenAiChatCompletions;
  }
  return {};
}

// Authoritative Model-local configuration row. Secret material lives in a separate relation and is
// never part of this value. `version` is the configuration CAS/watermark; a provider-secret
// rotation only advances `secret_version` and leaves this version untouched.
struct ModelConfiguration {
  ResourceId model_id;
  std::string alias;
  ModelProviderKind provider_kind = ModelProviderKind::OpenAiChatCompletions;
  std::string endpoint;
  std::string provider_model;
  std::uint32_t timeout_ms = kModelDefaultTimeoutMs;
  std::uint32_t max_concurrency = kModelDefaultConcurrency;
  LifecycleState state = LifecycleState::Disabled;
  ResourceVersion version{1};
  TimePoint created_at;
  TimePoint observed_at;
};

// Sealed provider credential version. Only ciphertext/tag, nonce and root-key version are stored;
// the plaintext never enters a record, log, audit row or public projection.
struct ProviderSecretRecord {
  ResourceId model_id;
  ResourceVersion secret_version{1};
  std::uint64_t root_key_version = 0;
  std::array<std::byte, 24> nonce{};
  std::vector<std::byte> sealed_secret;
  std::uint8_t aead = 1;
  TimePoint created_at;
};

struct ModelMessage {
  std::string role;
  std::string text;
};

// Provider-facing call shape. The secret is borrowed only for this call and must not be retained.
struct ModelProviderRequest {
  std::string endpoint;
  std::string provider_model;
  std::uint32_t timeout_ms = kModelDefaultTimeoutMs;
  std::vector<ModelMessage> messages;
  SecretView secret;
};

struct ModelProviderUsage {
  bool known = false;
  std::uint64_t input_tokens = 0;
  std::uint64_t output_tokens = 0;
};

struct ModelProviderReply {
  std::string text;
  std::string provider_request_id;
  std::uint32_t latency_ms = 0;
  ModelProviderUsage usage;
};

enum class ModelProviderOutcome { Succeeded, DefiniteFailure, Unknown };

// Result of one provider invocation. `failure` is a stable code only; provider text/details never
// cross this port. Unknown means the provider call may have happened and may have billed.
struct ModelProviderResult {
  ModelProviderOutcome outcome = ModelProviderOutcome::Unknown;
  ModelProviderReply reply;
  Failure failure;
};

// Txn2 mutation derived from a provider result. It is applied with the current call version CAS.
struct ModelCallMutation {
  ModelCallState state = ModelCallState::Unknown;
  ExecutionOutcome execution_outcome = ExecutionOutcome::Unknown;
  bool usage_known = false;
  std::uint64_t input_tokens = 0;
  std::uint64_t output_tokens = 0;
  bool may_have_billed = true;
  std::string provider_request_id;
  std::uint32_t latency_ms = 0;
  std::optional<ErrorCode> error;
  // Upstream RATE_LIMITED hint (bounded); surfaced on the durable-failure RpcFailure only, never
  // persisted as a billing fact.
  std::optional<std::chrono::milliseconds> retry_after;
};

inline bool valid_model_identifier(std::string_view value) noexcept {
  // Lowercase UUIDv4, matching docs/architecture.md section 4.1 and target_codec ResourceRef rules.
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
  return value[14] == '4' &&
         (value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b');
}

inline bool valid_model_alias(std::string_view value) noexcept {
  if (value.empty() || value.size() > kModelMaximumAlias) {
    return false;
  }
  const auto lower = [](char character) { return character >= 'a' && character <= 'z'; };
  const auto digit = [](char character) { return character >= '0' && character <= '9'; };
  if (!(lower(value.front()) || digit(value.front()))) {
    return false;
  }
  for (const char character : value) {
    if (!(lower(character) || digit(character) || character == '.' || character == '_' ||
          character == '-')) {
      return false;
    }
  }
  return true;
}

inline bool visible_ascii(std::string_view value) noexcept {
  for (const unsigned char character : value) {
    if (character < 33 || character > 126) {
      return false;
    }
  }
  return true;
}

inline bool valid_model_endpoint(std::string_view value) noexcept {
  if (value.empty() || value.size() > kModelMaximumEndpoint || !visible_ascii(value)) {
    return false;
  }
  std::string_view rest;
  std::string_view authority;
  if (value.starts_with("https://")) {
    rest = value.substr(8);
  } else if (value.starts_with("http://")) {
    rest = value.substr(7);
  } else {
    return false;
  }
  if (rest.empty() || rest.find_first_of("?#@\\") != std::string_view::npos) {
    return false;
  }
  const auto slash = rest.find('/');
  authority = rest.substr(0, slash == std::string_view::npos ? rest.size() : slash);
  if (authority.empty()) {
    return false;
  }
  if (value.starts_with("http://")) {
    // Plain HTTP is accepted only for the local loopback provider, never for a remote endpoint.
    std::string_view host = authority;
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
      host = authority.substr(0, colon);
      const auto port = authority.substr(colon + 1);
      if (port.empty() || port.size() > 5) {
        return false;
      }
      for (const char character : port) {
        if (character < '0' || character > '9') {
          return false;
        }
      }
    }
    if (host != "127.0.0.1" && host != "localhost") {
      return false;
    }
  }
  return true;
}

inline bool valid_provider_model(std::string_view value) noexcept {
  return !value.empty() && value.size() <= kModelMaximumProviderModel && visible_ascii(value);
}

inline bool valid_model_timeout(std::uint32_t value) noexcept {
  return value >= kModelMinimumTimeoutMs && value <= kModelMaximumTimeoutMs;
}

inline bool valid_model_concurrency(std::uint32_t value) noexcept {
  return value >= 1 && value <= kModelMaximumConcurrency;
}

inline bool valid_model_secret(std::span<const std::byte> value) noexcept {
  if (value.empty() || value.size() > kModelMaximumSecretBytes) {
    return false;
  }
  for (const auto byte : value) {
    const auto character = static_cast<unsigned char>(byte);
    if (character < 33 || character > 126) {
      return false;
    }
  }
  return true;
}

inline bool valid_model_message_role(std::string_view value) noexcept {
  return value == "system" || value == "user" || value == "assistant";
}

inline bool valid_provider_request_id(std::string_view value) noexcept {
  if (value.empty()) {
    return true;
  }
  if (value.size() > kModelMaximumProviderRequestId) {
    return false;
  }
  for (const unsigned char character : value) {
    if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '.' || character == '_' ||
          character == ':' || character == '-')) {
      return false;
    }
  }
  return true;
}

inline std::uint32_t model_timeout_budget(std::uint32_t configured_ms, TimePoint deadline,
                                          TimePoint now) noexcept {
  if (deadline <= now) {
    return 0;
  }
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  const auto bounded =
      remaining.count() <= 0 ? 0ULL : static_cast<std::uint64_t>(remaining.count());
  // Leave a bounded margin so the response and txn2 can still complete inside the RPC budget.
  const auto usable = bounded > 100 ? bounded - 100 : 0ULL;
  return static_cast<std::uint32_t>(
      std::min<std::uint64_t>({configured_ms, usable, kModelMaximumTimeoutMs}));
}

// Canonical byte encoding used for the durable duplicate-call fingerprint. It intentionally
// excludes transport identifiers, deadline and configuration version: a retry with identical
// semantic content must match the original intent even after a configuration or secret rotation.
inline std::vector<std::byte> model_request_canonical(const std::string& model,
                                                      const std::vector<ModelMessage>& messages) {
  const auto append_u32 = [](std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
    out.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    out.push_back(static_cast<std::byte>(value & 0xffU));
  };
  const auto append_bytes = [&](std::vector<std::byte>& out, std::string_view value) {
    append_u32(out, static_cast<std::uint32_t>(value.size()));
    for (const char character : value) {
      out.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
  };
  std::vector<std::byte> result;
  static constexpr std::string_view kDomain = "kiarabot.model.Chat.v1";
  result.reserve(kDomain.size() + model.size() + 8 + messages.size() * 24);
  for (const char character : kDomain) {
    result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  append_bytes(result, model);
  append_u32(result, static_cast<std::uint32_t>(messages.size()));
  for (const auto& message : messages) {
    append_bytes(result, message.role);
    append_bytes(result, message.text);
  }
  return result;
}

// AEAD associated data binding a sealed provider secret to its exact version and root key. The
// adapter and the application both build it with this single helper.
inline std::vector<std::byte> model_secret_aad(const ResourceId& model_id,
                                               const ResourceVersion& secret_version,
                                               std::uint64_t root_key_version) {
  std::vector<std::byte> result;
  static constexpr std::string_view kDomain = "kiarabot.model.provider-secret.v1";
  for (const char character : kDomain) {
    result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  const auto append_u64 = [&](std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      result.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
  };
  const auto append_text = [&](std::string_view value) {
    append_u64(value.size());
    for (const char character : value) {
      result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
  };
  append_text(model_id.value);
  append_u64(secret_version.value);
  append_u64(root_key_version);
  return result;
}

inline ModelCallMutation model_success_mutation(const ModelProviderReply& reply) {
  ModelCallMutation mutation;
  mutation.state = ModelCallState::Succeeded;
  mutation.execution_outcome = ExecutionOutcome::KnownSucceeded;
  mutation.usage_known = reply.usage.known;
  mutation.input_tokens = reply.usage.known ? reply.usage.input_tokens : 0;
  mutation.output_tokens = reply.usage.known ? reply.usage.output_tokens : 0;
  mutation.may_have_billed = true;
  mutation.provider_request_id = reply.provider_request_id;
  mutation.latency_ms = reply.latency_ms;
  return mutation;
}

inline ModelCallMutation model_definite_failure_mutation(ErrorCode code) {
  ModelCallMutation mutation;
  mutation.state = ModelCallState::Failed;
  mutation.execution_outcome = ExecutionOutcome::KnownFailed;
  mutation.usage_known = true;
  mutation.may_have_billed = false;
  mutation.error = code;
  return mutation;
}

inline ModelCallMutation model_unknown_mutation(ErrorCode code) {
  ModelCallMutation mutation;
  mutation.state = ModelCallState::Unknown;
  mutation.execution_outcome = ExecutionOutcome::Unknown;
  mutation.usage_known = false;
  mutation.may_have_billed = true;
  mutation.error = code;
  return mutation;
}

inline OperationState model_operation_state(ModelCallState state) noexcept {
  switch (state) {
  case ModelCallState::Started:
    return OperationState::Running;
  case ModelCallState::Succeeded:
    return OperationState::Succeeded;
  case ModelCallState::Failed:
    return OperationState::Failed;
  case ModelCallState::Unknown:
    return OperationState::Blocked;
  }
  return OperationState::Blocked;
}

inline ExecutionOutcome model_execution_outcome(ModelCallState state) noexcept {
  switch (state) {
  case ModelCallState::Started:
  case ModelCallState::Unknown:
    return ExecutionOutcome::Unknown;
  case ModelCallState::Succeeded:
    return ExecutionOutcome::KnownSucceeded;
  case ModelCallState::Failed:
    return ExecutionOutcome::KnownFailed;
  }
  return ExecutionOutcome::Unknown;
}

inline TimePoint model_retention_until(TimePoint observed_at) noexcept {
  return observed_at + kModelCallRetention;
}

inline bool model_unknown_resolution_due(const ModelIntent& call, TimePoint now) noexcept {
  return call.state == ModelCallState::Unknown &&
         call.disposition == ModelCallDisposition::Unspecified &&
         now >= call.created_at + kModelUnknownResolutionWindow;
}

// Rule-based 30-day disposition: an UNKNOWN row that cannot be resolved automatically is marked
// ABANDONED (terminal Failed, outcome stays Unknown, billing conservatively may-have-happened) and
// keeps its duplicate key until the normal retention sweep removes it.
inline ModelIntent model_abandon_unknown(const ModelIntent& call, TimePoint now,
                                         std::string_view actor) {
  ModelIntent abandoned = call;
  abandoned.state = ModelCallState::Failed;
  abandoned.may_have_billed = true;
  abandoned.disposition = ModelCallDisposition::Abandoned;
  abandoned.resolved_at = now;
  abandoned.resolved_by = std::string{actor};
  abandoned.observed_at = now;
  abandoned.version.value += 1;
  return abandoned;
}

inline bool valid_call_disposition(ModelCallDisposition disposition) noexcept {
  return disposition == ModelCallDisposition::NotExecuted ||
         disposition == ModelCallDisposition::ExecutedNotBilled ||
         disposition == ModelCallDisposition::ExecutedBilled ||
         disposition == ModelCallDisposition::Abandoned;
}

inline ModelIntent model_resolve_unknown(const ModelIntent& call, ModelCallDisposition disposition,
                                         TimePoint now, std::string_view actor) {
  ModelIntent resolved = call;
  resolved.state = ModelCallState::Failed;
  switch (disposition) {
  case ModelCallDisposition::NotExecuted:
    resolved.may_have_billed = false;
    resolved.usage_known = true;
    break;
  case ModelCallDisposition::ExecutedNotBilled:
    resolved.may_have_billed = false;
    break;
  case ModelCallDisposition::ExecutedBilled:
  case ModelCallDisposition::Abandoned:
  case ModelCallDisposition::Unspecified:
    resolved.may_have_billed = true;
    break;
  }
  resolved.disposition = disposition;
  resolved.resolved_at = now;
  resolved.resolved_by = std::string{actor};
  resolved.observed_at = now;
  resolved.version.value += 1;
  return resolved;
}

} // namespace kiarabot::core::target
