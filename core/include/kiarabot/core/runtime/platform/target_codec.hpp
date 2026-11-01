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

// Construct the stable Error member without fabricating business acceptance/recovery facts.
target::Outcome<v1::Error> make_error(target::ErrorCode code, std::string_view request_id,
                                      std::string_view trace_id, bool retryable) noexcept;
// The sole target unary binary detail is RpcFailure; Error-only reference Connect
// details are deliberately rejected, with no fallback or information-losing adapter.
// Validation is structural, not owner recovery policy. The owner must validate
// acceptance/recovery against the actual command and committed facts: an Unknown
// result must never authorize a new-key retry merely because its recovery enum is known.
target::Outcome<grpc::Status> to_grpc_failure(const v1::RpcFailure& failure) noexcept;
target::Outcome<v1::RpcFailure> from_grpc_failure(const grpc::Status& status) noexcept;
target::Outcome<v1::RpcFailure> from_http_failure(int status,
                                                  const v1::RpcFailure& failure) noexcept;

// Error member validation only; this does not decode a complete target failure envelope.
target::Outcome<v1::Error> from_http_error(int status, const v1::Error& error) noexcept;

} // namespace kiarabot::core::platform::target_codec
