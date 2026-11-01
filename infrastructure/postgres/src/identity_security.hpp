#pragma once

#include <memory>

#include "kiarabot/core/ports/target/identity_repository.hpp"
#include "record_access.hpp"

namespace kiarabot::infrastructure::postgres::detail {

// These repositories borrow the provided SQL access and cannot outlive its owning common UoW.
std::unique_ptr<core::target::IdentityCredentialRepository> credential_repository(SqlWork& sql);
std::unique_ptr<core::target::IdentityBootstrapRepository> bootstrap_repository(SqlWork& sql);
std::unique_ptr<core::target::IdentitySessionSecurityRepository>
session_security_repository(SqlWork& sql);
std::unique_ptr<core::target::IdentitySecretReceiptRepository>
secret_receipt_repository(SqlWork& sql);

} // namespace kiarabot::infrastructure::postgres::detail
