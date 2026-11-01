#pragma once

#include "kiarabot/core/domain/target/identity_types.hpp"

namespace kiarabot::core::target {

// Full ORIGINAL public success, stored atomically with registration. Contains no secret or
// verifier. Never rebuild initial_credential from a later current-head/status lookup on replay.
struct RegisteredPrincipalSnapshot {
  Principal principal;
  CredentialStatusSnapshot initial_credential;
};

} // namespace kiarabot::core::target
