#pragma once

#include "kiarabot/core/ports/target/scheduler_ports.hpp"

namespace kiarabot::infrastructure::identity_crypto {

// Raw-byte SHA-256. Calls own their digest state and may run concurrently; no business encoding.
class Sha256 final : public core::target::SchedulerDigestPort {
public:
  core::target::Outcome<core::target::Sha256Digest> sha256(std::string_view value) override;
};

} // namespace kiarabot::infrastructure::identity_crypto
