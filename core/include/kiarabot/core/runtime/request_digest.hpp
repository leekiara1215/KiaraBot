#pragma once

#include <string>

#include <google/protobuf/message_lite.h>

namespace kiarabot::core {
// SHA-256 of deterministic protobuf encoding; throws only on internal failure.
std::string request_digest(const google::protobuf::MessageLite& message);
std::string random_instance_id();
} // namespace kiarabot::core
