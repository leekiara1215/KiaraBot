#pragma once

#include <string_view>

#include <google/protobuf/timestamp.pb.h>
#include <grpcpp/support/status.h>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/core/domain/target/identity_permissions.hpp"
#include "kiarabot/v1/common.pb.h"
#include "kiarabot/v1/platform.pb.h"
#include "kiarabot/v1/policy.pb.h"

namespace kiarabot::core::platform::target_codec {

// Boundary-only conversions. Unknown/UNSPECIFIED/invalid values return InvalidArgument;
// Allocation/parse exceptions become Internal; no exception escapes this boundary.
// An absent optional message must be rejected by its caller before invoking a conversion.
target::Outcome<google::protobuf::Timestamp> to_wire(target::TimePoint value) noexcept;
target::Outcome<target::TimePoint> from_wire(const google::protobuf::Timestamp& value) noexcept;
target::Outcome<v1::ResourceKind> to_wire(target::ResourceKind value) noexcept;
target::Outcome<target::ResourceKind> from_wire(v1::ResourceKind value) noexcept;
target::Outcome<v1::ResourceRef> to_wire(const target::ResourceRef& value) noexcept;
target::Outcome<target::ResourceRef> from_wire(const v1::ResourceRef& value) noexcept;
target::Outcome<v1::Permission> to_wire(target::Permission value) noexcept;
target::Outcome<target::Permission> from_wire(v1::Permission value) noexcept;
target::Outcome<v1::ErrorCode> to_wire(target::ErrorCode value) noexcept;
target::Outcome<target::ErrorCode> from_wire(v1::ErrorCode value) noexcept;
target::Outcome<int> http_status(target::ErrorCode value) noexcept;
target::Outcome<grpc::StatusCode> grpc_status(target::ErrorCode value) noexcept;

// These carry stable Error only, never flatten Failure's acceptance/recovery/operation.
// Use RpcFailure separately when an owner response carries those additional facts.
target::Outcome<v1::Error> make_error(target::ErrorCode code, std::string_view request_id,
                                      std::string_view trace_id, bool retryable) noexcept;
target::Outcome<grpc::Status> to_grpc_error(const v1::Error& error) noexcept;
target::Outcome<v1::Error> from_grpc_error(const grpc::Status& status) noexcept;
target::Outcome<v1::Error> from_http_error(int status, const v1::Error& error) noexcept;

} // namespace kiarabot::core::platform::target_codec
