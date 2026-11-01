#pragma once

#include "kiarabot/core/application/target/identity_authorization_application.hpp"

namespace kiarabot::infrastructure::postgres::test {
// Called only after the owned fixture's existing assertions; mutates its disposable identities.
void identity_authorization_checks(core::target::IdentitySecurityUnitOfWorkFactory& factory,
                                   core::target::DelegationCryptoPort& crypto,
                                   core::target::SecurityClockPort& clock,
                                   const core::target::SubjectId& user,
                                   const core::target::CredentialId& credential,
                                   const core::target::SessionId& session,
                                   const core::target::SubjectId& control);
} // namespace kiarabot::infrastructure::postgres::test
