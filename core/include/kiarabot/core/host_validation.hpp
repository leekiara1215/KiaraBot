#pragma once

#include "kiarabot/v1/plugins.pb.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kiarabot::core {

// Validates only the envelope and operation shape. Authorization, idempotency,
// storage policy and IANA time-zone resolution are checked by their owners.
inline std::optional<kiarabot::v1::Error>
validate_host_request(const kiarabot::v1::HostRequest& request,
                      std::chrono::system_clock::time_point now) {
  auto reject = [&](kiarabot::v1::ErrorCode code, std::string_view message) {
    kiarabot::v1::Error error;
    error.set_code(code);
    error.set_message(std::string(message));
    error.set_request_id(request.request_id());
    error.set_trace_id(request.trace_id());
    return std::optional<kiarabot::v1::Error>(std::move(error));
  };
  const auto valid_id = [](std::string_view value) {
    return !value.empty() && value.size() <= 256;
  };
  if (!valid_id(request.request_id()) || !valid_id(request.trace_id()) || !request.has_deadline()) {
    return reject(kiarabot::v1::INVALID_ARGUMENT, "Invalid request metadata");
  }
  const auto& deadline = request.deadline();
  if (deadline.seconds() < -62135596800LL || deadline.seconds() > 253402300799LL ||
      deadline.nanos() < 0 || deadline.nanos() >= 1000000000) {
    return reject(kiarabot::v1::INVALID_ARGUMENT, "Invalid deadline");
  }
  const auto now_seconds = std::chrono::floor<std::chrono::seconds>(now.time_since_epoch());
  const auto now_nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch() - now_seconds);
  const auto delta_seconds = deadline.seconds() - now_seconds.count();
  if (delta_seconds < 0 || (delta_seconds == 0 && deadline.nanos() <= now_nanos.count())) {
    return reject(kiarabot::v1::TIMEOUT, "Request deadline expired");
  }
  if (delta_seconds > 300 || (delta_seconds == 300 && deadline.nanos() > now_nanos.count())) {
    return reject(kiarabot::v1::INVALID_ARGUMENT, "Deadline exceeds five minutes");
  }
  const bool mutating = request.has_storage_set() || request.has_storage_delete() ||
                        request.has_task_create() || request.has_task_cancel();
  if (mutating &&
      (request.idempotency_key().size() < 16 || request.idempotency_key().size() > 128)) {
    return reject(kiarabot::v1::INVALID_ARGUMENT, "Invalid idempotency key");
  }
  switch (request.operation_case()) {
  case kiarabot::v1::HostRequest::kModelChat:
    if (!valid_id(request.model_chat().model()) || request.model_chat().messages().empty()) {
      return reject(kiarabot::v1::INVALID_ARGUMENT, "Invalid model request");
    }
    for (const auto& message : request.model_chat().messages()) {
      if (message.role() != "system" && message.role() != "user" && message.role() != "assistant") {
        return reject(kiarabot::v1::INVALID_ARGUMENT, "Invalid model message role");
      }
    }
    break;
  case kiarabot::v1::HostRequest::kStorageGet:
    if (!valid_id(request.storage_get().key())) {
      return reject(kiarabot::v1::INVALID_ARGUMENT, "Invalid storage key");
    }
    break;
  case kiarabot::v1::HostRequest::kStorageSet:
    if (!valid_id(request.storage_set().key())) {
      return reject(kiarabot::v1::INVALID_ARGUMENT, "Invalid storage key");
    }
    break;
  case kiarabot::v1::HostRequest::kStorageDelete:
    if (!valid_id(request.storage_delete().key())) {
      return reject(kiarabot::v1::INVALID_ARGUMENT, "Invalid storage key");
    }
    break;
  case kiarabot::v1::HostRequest::kTaskCreate:
    if (!valid_id(request.task_create().task_id()) ||
        !valid_id(request.task_create().target_conversation()) ||
        request.task_create().when_case() == kiarabot::v1::TaskCreateRequest::WHEN_NOT_SET) {
      return reject(kiarabot::v1::INVALID_ARGUMENT, "Invalid task request");
    }
    break;
  case kiarabot::v1::HostRequest::kTaskCancel:
    if (!valid_id(request.task_cancel().task_id())) {
      return reject(kiarabot::v1::INVALID_ARGUMENT, "Invalid task ID");
    }
    break;
  case kiarabot::v1::HostRequest::kTaskGet:
    if (!valid_id(request.task_get().task_id())) {
      return reject(kiarabot::v1::INVALID_ARGUMENT, "Invalid task ID");
    }
    break;
  case kiarabot::v1::HostRequest::OPERATION_NOT_SET:
  default:
    return reject(kiarabot::v1::INVALID_ARGUMENT, "Missing operation");
  }
  return std::nullopt;
}

} // namespace kiarabot::core
