#include <memory>
#include <type_traits>
#include <utility>

#include "kiarabot/core/application/target/identity_security_application.hpp"

namespace target = kiarabot::core::target;
static_assert(std::is_base_of_v<target::ServiceReadinessPort, target::IdentitySecurityApplication>);
static_assert(std::is_same_v<decltype(std::declval<target::IdentitySecurityApplication&>().login(
                                 std::declval<const target::LoginRequest&>())),
                             target::Outcome<target::IdentityTokenReply>>);
static_assert(
    std::is_same_v<decltype(std::declval<target::IdentitySecurityApplication&>().refresh_session(
                       std::declval<const target::RefreshSessionRequest&>())),
                   target::Outcome<target::IdentityTokenReply>>);
static_assert(
    std::is_same_v<decltype(std::declval<target::IdentitySecurityApplication&>().validate_session(
                       std::declval<const target::AccessSessionRequest&>())),
                   target::Outcome<target::ValidatedSession>>);
static_assert(
    std::is_same_v<decltype(std::declval<target::IdentitySecurityApplication&>().get_principal(
                       std::declval<const target::AccessSessionRequest&>())),
                   target::Outcome<target::CurrentPrincipal>>);
static_assert(
    std::is_same_v<decltype(std::declval<target::IdentitySecurityApplication&>().revoke_session(
                       std::declval<const target::RevokeSessionRequest&>())),
                   target::Outcome<target::RevokeSessionReply>>);
static_assert(
    std::is_same_v<decltype(std::declval<target::IdentitySecurityApplication&>().list_sessions(
                       std::declval<const target::ListSessionsRequest&>())),
                   target::Outcome<target::SessionPageReply>>);
static_assert(!std::is_copy_constructible_v<target::IdentityTokenReply>);
static_assert(
    std::is_same_v<decltype(&target::make_identity_security_application),
                   target::Outcome<std::unique_ptr<target::IdentitySecurityApplication>> (*)(
                       target::IdentitySecurityDependencies, target::IdentitySecurityOptions)>);
int main() {}
