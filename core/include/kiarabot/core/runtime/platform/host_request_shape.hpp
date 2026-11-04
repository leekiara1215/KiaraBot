#pragma once

#include <optional>

#include <google/protobuf/unknown_field_set.h>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/v1/plugins.pb.h"

namespace kiarabot::core::platform {

// Wire-boundary classification only, after current-stream authorization. Known oneof fields
// already obey protobuf last-one-wins; their overwritten raw history is not available here.
// Unknown fields outside the operation allocation range are compatible metadata.
inline std::optional<target::ErrorCode> host_request_shape_error(const v1::HostRequest& request) {
  const auto& unknown = request.GetReflection()->GetUnknownFields(request);
  int candidates = 0;
  bool wrong_type = false;
  for (int index = 0; index < unknown.field_count(); ++index) {
    const auto& field = unknown.field(index);
    if (field.number() >= 10 && field.number() <= 127) {
      ++candidates;
      wrong_type =
          wrong_type || field.type() != google::protobuf::UnknownField::TYPE_LENGTH_DELIMITED;
    }
  }
  const bool known = request.operation_case() != v1::HostRequest::OPERATION_NOT_SET;
  if (candidates != 0) {
    if (known || candidates != 1 || wrong_type) {
      return target::ErrorCode::InvalidArgument;
    }
    return target::ErrorCode::CapabilityNotSupported;
  }
  if (!known) {
    return target::ErrorCode::InvalidArgument;
  }
  return std::nullopt;
}

} // namespace kiarabot::core::platform
