#include "kiarabot/core/host_dispatcher.hpp"

#include "kiarabot/core/host_validation.hpp"
#include "kiarabot/core/request_digest.hpp"

namespace kiarabot::core {
namespace {
namespace wire = kiarabot::v1;
wire::HostResponse rejected(const wire::HostRequest& request, wire::ErrorCode code,
                            bool retryable = false) {
  wire::HostResponse response;
  response.set_request_id(request.request_id());
  auto* error = response.mutable_error();
  error->set_code(code);
  error->set_message(wire::ErrorCode_Name(code));
  error->set_request_id(request.request_id());
  error->set_trace_id(request.trace_id());
  error->set_retryable(retryable);
  return response;
}
wire::HostResponse mapped(const wire::HostRequest& request, StorageError failure) {
  return rejected(request,
                  failure == StorageError::Corrupt    ? wire::INTERNAL
                  : failure == StorageError::Conflict ? wire::CONFLICT
                                                      : wire::UPSTREAM_UNAVAILABLE,
                  failure != StorageError::Corrupt);
}
bool expired(const NamespaceEntry& entry, RuntimeTimePoint now) {
  return entry.expires_at != RuntimeTimePoint{} && entry.expires_at <= now;
}
} // namespace
HostDispatcher::HostDispatcher(RuntimeStore& store, RuntimeClock clock, NamespacePolicy policy,
                               std::shared_ptr<IModelProvider> model,
                               std::shared_ptr<TaskScheduler> scheduler)
    : model_(std::move(model)), scheduler_(std::move(scheduler)), store_(store),
      clock_(std::move(clock)), policy_(policy) {}

wire::HostResponse HostDispatcher::model_chat(const PluginSession& session,
                                              const wire::HostRequest& request) {
  if (!model_) {
    return rejected(request, wire::CAPABILITY_NOT_SUPPORTED);
  }
  const bool allowed = std::find(session.permissions.begin(), session.permissions.end(),
                                 "model.chat") != session.permissions.end();
  const auto audit = [&](const std::string& outcome) -> std::optional<wire::HostResponse> {
    std::lock_guard lock(mutex_);
    auto begun = store_.begin();
    if (const auto* error = std::get_if<StorageError>(&begun)) {
      return mapped(request, *error);
    }
    auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(begun));
    if (!transaction || transaction->snapshot().audit.size() >= policy_.audit_records) {
      return rejected(request, wire::UPSTREAM_UNAVAILABLE);
    }
    transaction->snapshot().audit.push_back(
        {"plugin/" + session.identity, "model.chat", session.identity, request.request_id(),
         request.trace_id(), clock_ ? clock_() : std::chrono::system_clock::now(), allowed,
         outcome});
    const auto committed = transaction->commit();
    if (const auto* error = std::get_if<StorageError>(&committed)) {
      return mapped(request, *error);
    }
    return std::nullopt;
  };
  if (const auto error = audit(allowed ? "STARTED" : "PERMISSION_DENIED")) {
    return *error;
  }
  if (!allowed) {
    return rejected(request, wire::PERMISSION_DENIED);
  }
  const auto now = clock_ ? clock_() : std::chrono::system_clock::now();
  const auto deadline = RuntimeTimePoint(std::chrono::duration_cast<RuntimeTimePoint::duration>(
      std::chrono::seconds(request.deadline().seconds()) +
      std::chrono::nanoseconds(request.deadline().nanos())));
  auto response = rejected(request, wire::TIMEOUT);
  if (deadline > now) {
    const auto result =
        model_->chat(request.model_chat(),
                     std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
    if (const auto* error = std::get_if<wire::Error>(&result)) {
      // Never pass provider messages/details (possibly credentials) across the boundary.
      const auto code = error->code();
      response = rejected(request,
                          wire::ErrorCode_IsValid(code) && code != wire::ERROR_CODE_UNSPECIFIED
                              ? code
                              : wire::INTERNAL,
                          code == wire::RATE_LIMITED && error->retryable());
    } else {
      response.Clear();
      response.set_request_id(request.request_id());
      *response.mutable_model_chat() = std::get<wire::ModelChatResponse>(result);
    }
  }
  if ((clock_ ? clock_() : std::chrono::system_clock::now()) >= deadline) {
    response = rejected(request, wire::TIMEOUT);
  }
  if (auto error =
          audit(response.has_error() ? wire::ErrorCode_Name(response.error().code()) : "OK")) {
    error->mutable_error()->set_retryable(false); // External call may have completed.
    return *error;
  }
  return response;
}
wire::HostResponse HostDispatcher::dispatch(const PluginSession& session,
                                            const wire::HostRequest& request) try {
  const auto now = clock_ ? clock_() : std::chrono::system_clock::now();
  if (const auto failure = validate_host_request(request, now)) {
    return rejected(request, failure->code(), failure->code() == wire::TIMEOUT);
  }
  if (request.has_model_chat()) {
    return model_chat(session, request);
  }
  if (request.has_task_create() || request.has_task_get() || request.has_task_cancel()) {
    return scheduler_ ? scheduler_->dispatch(session, request)
                      : rejected(request, wire::CAPABILITY_NOT_SUPPORTED);
  }
  std::lock_guard lock(mutex_);
  const bool writing = request.has_storage_set() || request.has_storage_delete();
  if (!writing && !request.has_storage_get()) {
    return rejected(request, wire::CAPABILITY_NOT_SUPPORTED);
  }
  auto begun = store_.begin();
  if (const auto* failure = std::get_if<StorageError>(&begun)) {
    return mapped(request, *failure);
  }
  auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(begun));
  if (!transaction) {
    return rejected(request, wire::UPSTREAM_UNAVAILABLE, true);
  }
  auto& snapshot = transaction->snapshot();
  if (snapshot.audit.size() >= policy_.audit_records) {
    return rejected(request, wire::UPSTREAM_UNAVAILABLE, true);
  }
  const bool allowed = std::find(session.permissions.begin(), session.permissions.end(),
                                 "storage.namespace") != session.permissions.end();
  wire::HostResponse response;
  response.set_request_id(request.request_id());
  if (!allowed) {
    response = rejected(request, wire::PERMISSION_DENIED);
  } else {
    const auto original = snapshot;
    auto canonical = request;
    canonical.clear_request_id();
    canonical.clear_trace_id();
    canonical.clear_deadline();
    const auto fingerprint = request_digest(canonical);
    auto& receipts = snapshot.host_receipts[session.identity];
    if (writing) {
      const auto previous = receipts.find(request.idempotency_key());
      if (previous != receipts.end() && previous->second.expires_at > now) {
        if (previous->second.fingerprint != fingerprint) {
          response = rejected(request, wire::CONFLICT);
        } else {
          if (!response.ParseFromString(previous->second.serialized_response) ||
              response.has_error() ||
              response.result_case() == wire::HostResponse::RESULT_NOT_SET) {
            return rejected(request, wire::INTERNAL);
          }
          response.set_request_id(request.request_id());
          return response; // Already committed; no repeated side effect or audit.
        }
      }
    }
    auto& values = snapshot.namespaces[session.identity];
    std::erase_if(values, [&](const auto& item) { return expired(item.second, now); });
    if (!response.has_error()) {
      if (request.has_storage_get()) {
        const auto found = values.find(request.storage_get().key());
        if (found == values.end()) {
          response = rejected(request, wire::NOT_FOUND);
        } else {
          response.mutable_storage_get()->set_value(found->second.value);
        }
      } else if (request.has_storage_delete()) {
        values.erase(request.storage_delete().key());
        response.mutable_storage_delete();
      } else {
        const auto& operation = request.storage_set();
        RuntimeTimePoint expiration{};
        if (operation.has_expires_at()) {
          const auto& date = operation.expires_at();
          const auto epoch_seconds =
              std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
          if (date.seconds() < epoch_seconds || date.seconds() > epoch_seconds + 30 * 86400LL ||
              date.nanos() < 0 || date.nanos() >= 1000000000) {
            response = rejected(request, wire::INVALID_ARGUMENT);
          } else {
            expiration = RuntimeTimePoint(std::chrono::duration_cast<RuntimeTimePoint::duration>(
                std::chrono::seconds(date.seconds()) + std::chrono::nanoseconds(date.nanos())));
            if (expiration <= now || expiration > now + std::chrono::hours(720)) {
              response = rejected(request, wire::INVALID_ARGUMENT);
            }
          }
        }
        std::size_t bytes = operation.value().size();
        for (const auto& [name, entry] : values) {
          if (name != operation.key()) {
            bytes += entry.value.size();
          }
        }
        if (!response.has_error() &&
            (operation.value().size() > policy_.value_bytes || bytes > policy_.namespace_bytes ||
             (!values.contains(operation.key()) && values.size() >= policy_.keys))) {
          response = rejected(request, wire::RATE_LIMITED, true);
        }
        if (!response.has_error()) {
          values[operation.key()] = {operation.value(), expiration};
          response.mutable_storage_set();
        }
      }
    }
    if (!response.has_error() && writing) {
      std::erase_if(receipts, [&](const auto& item) { return item.second.expires_at <= now; });
      if (receipts.size() >= policy_.receipts) {
        response = rejected(request, wire::RATE_LIMITED, true);
      } else {
        receipts[request.idempotency_key()] = {fingerprint, response.SerializeAsString(),
                                               now + std::chrono::hours(168)};
      }
    }
    if (response.has_error()) {
      snapshot = original;
    }
  }
  snapshot.audit.push_back(
      {"plugin/" + session.identity, "storage.namespace", session.identity, request.request_id(),
       request.trace_id(), now, allowed,
       response.has_error() ? wire::ErrorCode_Name(response.error().code()) : "OK"});
  const auto commit_time = clock_ ? clock_() : std::chrono::system_clock::now();
  if (const auto overdue = validate_host_request(request, commit_time);
      overdue && overdue->code() == wire::TIMEOUT) {
    return rejected(request, wire::TIMEOUT, true);
  }
  const auto committed = transaction->commit();
  if (const auto* failure = std::get_if<StorageError>(&committed)) {
    return mapped(request, *failure);
  }
  return response;
} catch (...) {
  return rejected(request, wire::INTERNAL);
}
} // namespace kiarabot::core
