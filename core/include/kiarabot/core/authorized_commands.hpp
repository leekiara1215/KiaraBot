#pragma once

#include "kiarabot/core/ports.hpp"

namespace kiarabot::core {
struct PermissionGrant {
  std::string subject;
  std::string permission;
  std::string resource;
};
// Constructed by trusted application code from authenticated identity and a
// canonical request SHA-256; never deserialize this object from a plugin payload.
struct CommandContext {
  std::string subject;
  std::string permission;
  std::string resource;
  std::string request_id;
  std::string trace_id;
  std::string idempotency_key;
  std::string fingerprint;
};

class AuthorizedCommands {
public:
  AuthorizedCommands(RuntimeStore& store, std::vector<PermissionGrant> grants, RuntimeClock clock,
                     std::size_t audit_limit = 100000)
      : store_(store), grants_(std::move(grants)), clock_(std::move(clock)),
        audit_limit_(audit_limit) {}
  // Effect must change only the supplied snapshot; external side effects belong
  // to later dispatch after commit. It must not recursively call Runtime.
  PortStatus execute(const CommandContext& request,
                     const std::function<PortStatus(RuntimeSnapshot&)>& effect) try {
    if (request.subject.empty() || request.permission.empty() || request.resource.empty() ||
        request.request_id.empty() || request.trace_id.empty() || request.idempotency_key.empty() ||
        request.subject.size() > 256 || request.permission.size() > 256 ||
        request.resource.size() > 256 || request.request_id.size() > 256 ||
        request.trace_id.size() > 256 || request.idempotency_key.size() > 256 ||
        request.fingerprint.size() != 64 ||
        !std::all_of(request.fingerprint.begin(), request.fingerprint.end(), [](char ch) {
          return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        })) {
      return error(kiarabot::v1::INVALID_ARGUMENT, request);
    }
    auto begun = store_.begin();
    if (auto* failure = std::get_if<StorageError>(&begun)) {
      return mapped(*failure, request);
    }
    auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(begun));
    if (!transaction) {
      return error(kiarabot::v1::UPSTREAM_UNAVAILABLE, request);
    }
    auto& state = transaction->snapshot();
    const bool allowed =
        std::any_of(grants_.begin(), grants_.end(), [&](const PermissionGrant& grant) {
          return grant.subject == request.subject && grant.permission == request.permission &&
                 (grant.resource == request.resource || grant.resource == "*");
        });
    const std::string key = std::to_string(request.permission.size()) + ":" + request.permission +
                            std::to_string(request.resource.size()) + ":" + request.resource +
                            request.idempotency_key;
    if (allowed) {
      const auto owner = state.commands.find(request.subject);
      if (owner != state.commands.end()) {
        const auto existing = owner->second.find(key);
        if (existing != owner->second.end()) {
          if (existing->second.fingerprint != request.fingerprint) {
            if (state.audit.size() >= audit_limit_) {
              return error(kiarabot::v1::UPSTREAM_UNAVAILABLE, request);
            }
            state.audit.push_back({request.subject, request.permission, request.resource,
                                   request.request_id, request.trace_id,
                                   clock_ ? clock_() : std::chrono::system_clock::now(), true,
                                   "CONFLICT"});
            const auto committed = transaction->commit();
            if (const auto* failure = std::get_if<StorageError>(&committed)) {
              return mapped(*failure, request);
            }
            return error(kiarabot::v1::CONFLICT, request);
          }
          return std::monostate{};
        }
      }
    }
    if (state.audit.size() >= audit_limit_) {
      return error(kiarabot::v1::UPSTREAM_UNAVAILABLE, request);
    }
    std::optional<kiarabot::v1::ErrorCode> rejected;
    if (allowed) {
      auto original = state;
      PortStatus outcome;
      try {
        outcome = effect(state);
      } catch (...) {
        outcome = error(kiarabot::v1::INTERNAL, request);
      }
      if (const auto* failure = std::get_if<kiarabot::v1::Error>(&outcome)) {
        rejected = kiarabot::v1::ErrorCode_IsValid(failure->code()) &&
                           failure->code() != kiarabot::v1::ERROR_CODE_UNSPECIFIED
                       ? failure->code()
                       : kiarabot::v1::INTERNAL;
        state = std::move(original);
      } else {
        state.commands[request.subject][key] = {request.fingerprint};
      }
    } else {
      rejected = kiarabot::v1::PERMISSION_DENIED;
    }
    state.audit.push_back({request.subject, request.permission, request.resource,
                           request.request_id, request.trace_id,
                           clock_ ? clock_() : std::chrono::system_clock::now(), allowed,
                           rejected ? kiarabot::v1::ErrorCode_Name(*rejected) : "OK"});
    const auto committed = transaction->commit();
    if (const auto* failure = std::get_if<StorageError>(&committed)) {
      return mapped(*failure, request);
    }
    return rejected ? PortStatus(error(*rejected, request)) : PortStatus(std::monostate{});
  } catch (...) {
    return error(kiarabot::v1::INTERNAL, request);
  }

private:
  static kiarabot::v1::Error error(kiarabot::v1::ErrorCode code, const CommandContext& request) {
    kiarabot::v1::Error result;
    result.set_code(code);
    result.set_message(kiarabot::v1::ErrorCode_Name(code));
    result.set_request_id(request.request_id);
    result.set_trace_id(request.trace_id);
    result.set_retryable(code == kiarabot::v1::UPSTREAM_UNAVAILABLE ||
                         code == kiarabot::v1::RATE_LIMITED || code == kiarabot::v1::TIMEOUT);
    return result;
  }
  static kiarabot::v1::Error mapped(StorageError failure, const CommandContext& request) {
    auto result = error(failure == StorageError::Conflict  ? kiarabot::v1::CONFLICT
                        : failure == StorageError::Corrupt ? kiarabot::v1::INTERNAL
                                                           : kiarabot::v1::UPSTREAM_UNAVAILABLE,
                        request);
    result.set_retryable(failure != StorageError::Corrupt);
    return result;
  }
  RuntimeStore& store_;
  const std::vector<PermissionGrant> grants_;
  RuntimeClock clock_;
  std::size_t audit_limit_;
};
} // namespace kiarabot::core
