#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/application/target/identity_security_application.hpp"
#include "kiarabot/core/ports/target/identity_repository.hpp"

namespace kiarabot::core::target {

// P12l read-only human principal directory. The caller is the verified Control platform peer; the
// application rechecks its current registration/credential and identity.principal.read on at least
// one currently granted resource (the Control management catalog grants directory-wide visibility,
// otherwise only principals/roles on the explicitly granted resources are visible). It never reads
// another owner's database or returns credentials/assignment internals.
struct PrincipalDirectoryCommand {
  IdentityRequestContext context;
  VerifiedPeer control_peer;
  std::uint32_t page_size = 0;
  std::string page_token;
  std::optional<LifecycleState> state_filter; // nullopt = every non-deleted state
};
struct PrincipalDirectorySummary {
  Principal principal;
  std::vector<FixedRole> roles;
};
struct PrincipalDirectoryReply {
  std::vector<PrincipalDirectorySummary> principals;
  std::uint32_t page_size = 0;
  std::string next_page_token;
  TimePoint checked_at;
};
class IdentityPrincipalDirectoryApplication {
public:
  virtual ~IdentityPrincipalDirectoryApplication() = default;
  virtual Outcome<PrincipalDirectoryReply>
  list_principals(const PrincipalDirectoryCommand& command) = 0;
};
Outcome<std::unique_ptr<IdentityPrincipalDirectoryApplication>>
make_identity_principal_directory_application(IdentitySecurityUnitOfWorkFactory& transactions);

} // namespace kiarabot::core::target
