#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

#include "kiarabot/core/domain/target/identity_policy.hpp"
#include "kiarabot/core/domain/target/identity_state_rules.hpp"

namespace target = kiarabot::core::target;
using std::chrono_literals::operator""s;
using std::chrono_literals::operator""h;
void check(bool condition, std::string_view scenario) {
  if (!condition) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}
template <typename Value>
bool error_is(const target::Outcome<Value>& result, target::ErrorCode code) {
  const auto* failure = std::get_if<target::Failure>(&result);
  return failure && failure->code == code;
}
bool success(const target::Status& result) {
  return std::holds_alternative<std::monostate>(result);
}
template <typename Value> bool equals(const target::Outcome<Value>& result, Value expected) {
  const auto* value = std::get_if<Value>(&result);
  return value && *value == expected;
}
target::Principal human() {
  return {
      {"human"}, target::PrincipalKind::Human, std::nullopt, {1}, target::LifecycleState::Active};
}
target::CredentialVersionRecord credential(target::TimePoint now) {
  return {{{"password"}, {1}},
          {"human"},
          target::PasswordVerifier{"adapter-verifier", {1}},
          target::CredentialState::Active,
          now - 1h,
          std::nullopt,
          {target::ResourceKind::Principal, {"human"}},
          now + 24h * 365};
}
target::SessionSecurity session(target::TimePoint now) {
  return {{"session"},
          {"human"},
          {{"password"}, {1}},
          {target::TokenPurpose::Access, {"token-key"}, {}},
          {target::TokenPurpose::Refresh, {"token-key"}, {}},
          now,
          now + target::kAccessTokenLifetime,
          now + target::kRefreshTokenLifetime,
          1,
          {1},
          false};
}
target::EncryptedSecretReceipt refresh_receipt(target::TimePoint now) {
  // Structural ciphertext placeholder only: these are state tests, not an AEAD adapter/fake crypto.
  return {{target::RefreshReceiptScope{{"control"}, {"session"}, "refresh-key"},
           {"human"},
           target::SessionId{"session"},
           {{"password"}, {1}},
           target::RefreshRequestParameters{1},
           2,
           {{"commit-key"}, {}},
           now,
           now + target::kSecretReceiptRetention},
          {{"receipt-key"}, 1, {std::byte{1}}, {std::byte{2}}}};
}
void policy_rules() {
  const auto actor = human();
  const target::ResourceRef a{target::ResourceKind::Account, {"A"}};
  const target::ResourceRef b{target::ResourceKind::Account, {"B"}};
  const target::ExactAssignment admin{
      {"admin-a"}, actor.id, {{target::FixedRole::Admin, a}}, {}, {1}};
  const target::ExactAssignment viewer{
      {"viewer-b"}, actor.id, {{target::FixedRole::Viewer, b}}, {}, {1}};
  std::array grants{admin, viewer};
  check(success(target::authorize_exact(actor, target::Permission::AccountManage, a, grants)),
        "admin may manage its exact account");
  check(error_is(target::authorize_exact(actor, target::Permission::AccountManage, b, grants),
                 target::ErrorCode::PermissionDenied),
        "admin@A plus viewer@B cannot become admin@B");
  check(success(target::authorize_exact(actor, target::Permission::AccountRead, b, grants)),
        "viewer reads its exact account");
  check(error_is(target::authorize_exact(actor, target::Permission::MessageRead, a, grants),
                 target::ErrorCode::PermissionDenied),
        "admin never implicitly reads messages");
  for (const auto role :
       {target::FixedRole::Admin, target::FixedRole::Operator, target::FixedRole::Viewer}) {
    check(!target::default_role_allows(role, target::Permission::MessageRead),
          "all default roles exclude message.read");
    check(!target::default_role_allows(role, target::Permission::ActionInspect),
          "all default roles exclude action.inspect");
  }
  const target::ExactAssignment sensitive{
      {"explicit"}, actor.id, {}, {{target::Permission::MessageRead, a}}, {1}};
  check(success(target::authorize_exact(actor, target::Permission::MessageRead, a,
                                        std::span{&sensitive, 1})),
        "explicit exact sensitive grant works");
  check(error_is(target::authorize_exact(actor, target::Permission::MessageRead, b,
                                         std::span{&sensitive, 1}),
                 target::ErrorCode::PermissionDenied),
        "explicit sensitive grant does not cross account scope");
  const target::ExactAssignment non_product{
      {"non-product"},
      actor.id,
      {},
      {{target::Permission::AccountRead, a}, {target::Permission::MessageRead, b}},
      {1}};
  check(error_is(target::project_assignment(non_product), target::ErrorCode::Internal),
        "non-product explicit pairs cannot be flattened into extra wire permissions");
  check(error_is(target::authorize_exact(actor, target::Permission::MessageRead, b,
                                         std::span{&non_product, 1}),
                 target::ErrorCode::Internal),
        "corrupt stored basis fails closed instead of recombining grants");
  const target::ExactAssignment product{{"product"},
                                        actor.id,
                                        {},
                                        {{target::Permission::AccountRead, a},
                                         {target::Permission::MessageRead, a},
                                         {target::Permission::AccountRead, b},
                                         {target::Permission::MessageRead, b}},
                                        {1}};
  const auto projection =
      std::get<target::AssignmentProjection>(target::project_assignment(product));
  check(!projection.role && projection.permissions.size() == 2 && projection.scope.size() == 2,
        "complete intended product safely projects to one explicit wire assignment");
  const std::array duplicate_rows{admin, admin};
  check(error_is(target::validate_assignment_collection(duplicate_rows, actor.id),
                 target::ErrorCode::Internal),
        "one assignment ID cannot be returned twice as independent scope fragments");
  auto catalog = admin;
  catalog.roles.front().resource.kind = target::ResourceKind::Catalog;
  check(error_is(target::authorize_exact(actor, target::Permission::AccountManage, a,
                                         std::span{&catalog, 1}),
                 target::ErrorCode::PermissionDenied),
        "Catalog never inherits authority over same-ID child resource");
  check(error_is(target::permission_from_name("unknown.permission"),
                 target::ErrorCode::InvalidArgument),
        "unknown permission string is not extensible authority");
  check(equals(target::permission_from_name("message.read"), target::Permission::MessageRead),
        "explicit G08 word is registered");
  volatile int unknown_permission_input = 65535;
  check(error_is(target::authorize_exact(
                     actor, static_cast<target::Permission>(unknown_permission_input), a, grants),
                 target::ErrorCode::InvalidArgument),
        "unknown enum also rejects before role scan");
  volatile int unknown_role_input = 65535;
  check(!target::default_role_allows(static_cast<target::FixedRole>(unknown_role_input),
                                     target::Permission::ResourceRead),
        "unknown role cannot inherit a common capability");
  auto plugin = actor;
  plugin.kind = target::PrincipalKind::Plugin;
  check(error_is(target::authorize_exact(plugin, target::Permission::AccountManage, a, grants),
                 target::ErrorCode::PermissionDenied),
        "plugin does not inherit human admin role");
  check(success(target::authorize_session_list(actor, actor.id, {})),
        "self session list is explicit principal-owner rule");
  const auto own = target::session_metadata(session(target::TimePoint{24h}));
  check(success(target::authorize_session_revoke(actor, own, {})),
        "self may revoke only its owned session");
  auto other = own;
  other.subject = {"other"};
  other.id = {"other-session"};
  const target::ExactAssignment principal_admin{
      {"principal-admin"},
      actor.id,
      {{target::FixedRole::Admin, {target::ResourceKind::Principal, {"other"}}}},
      {},
      {1}};
  check(error_is(target::authorize_session_revoke(actor, other, std::span{&principal_admin, 1}),
                 target::ErrorCode::PermissionDenied),
        "admin Principal grant is not inheritance over another user's Session");
  const target::ExactAssignment session_grant{
      {"session-revoke"},
      actor.id,
      {},
      {{target::Permission::IdentitySessionRevoke,
        {target::ResourceKind::Session, {"other-session"}}}},
      {1}};
  check(success(target::authorize_session_revoke(actor, other, std::span{&session_grant, 1})),
        "exact foreign Session grant can authorize revoke");
  const target::ExactAssignmentSpec role_spec{
      {"human"}, target::RoleAssignmentSpec{target::FixedRole::Viewer, {a}}};
  check(error_is(target::validate_assignment_spec(role_spec, plugin),
                 target::ErrorCode::InvalidArgument),
        "fixed-role assignment is human-only");
  const target::ExactAssignmentSpec empty{{"human"}, target::ExplicitAssignmentSpec{{}, {a}}};
  check(
      error_is(target::validate_assignment_spec(empty, actor), target::ErrorCode::InvalidArgument),
      "empty explicit permission set rejected");
  const target::ExactAssignmentSpec duplicate{
      {"human"},
      target::ExplicitAssignmentSpec{
          {target::Permission::AccountRead, target::Permission::AccountRead}, {a}}};
  check(error_is(target::validate_assignment_spec(duplicate, actor),
                 target::ErrorCode::InvalidArgument),
        "duplicate explicit permissions rejected");
  const target::ExactAssignmentSpec move_to_b{
      {"human"}, target::RoleAssignmentSpec{target::FixedRole::Viewer, {b}}};
  check(
      error_is(target::authorize_assignment_change(actor, &viewer, role_spec, std::span{&admin, 1}),
               target::ErrorCode::PermissionDenied),
      "assignment update needs authority over old scope as well as new");
  check(
      error_is(target::authorize_assignment_change(actor, &admin, move_to_b, std::span{&admin, 1}),
               target::ErrorCode::PermissionDenied),
      "assignment update cannot escape current scope into unauthorized new scope");
  check(error_is(target::prepare_assignment(admin.id, move_to_b, actor, &admin,
                                            target::ResourceVersion{2}),
                 target::ErrorCode::Conflict),
        "stale assignment version conflicts");
  auto foreign_spec = move_to_b;
  foreign_spec.subject = {"other"};
  auto foreign = actor;
  foreign.id = {"other"};
  check(error_is(target::prepare_assignment(admin.id, foreign_spec, foreign, &admin,
                                            target::ResourceVersion{1}),
                 target::ErrorCode::Conflict),
        "updating grant cannot replace subject");
  const auto created = std::get<target::ExactAssignment>(
      target::prepare_assignment({"server-generated"}, role_spec, actor, nullptr, std::nullopt));
  check(created.version.value == 1 && created.roles.size() == 1 &&
            created.explicit_permissions.empty(),
        "new role assignment starts version1 without mixed basis");
  const target::AuthorizationState version{{"human"}, {9}, false};
  const auto changed =
      std::get<target::AuthorizationState>(target::assignment_changed(version, {9}));
  check(changed.version.value == 10 && changed.invalidation_dirty && !version.invalidation_dirty,
        "grant change advances durable invalidation watermark without mutating input");
  check(error_is(target::assignment_changed(version, {8}), target::ErrorCode::Conflict),
        "authorization watermark is compare-and-set");
  check(error_is(target::assignment_changed(
                     {{"human"}, {std::numeric_limits<std::uint64_t>::max()}, true},
                     {std::numeric_limits<std::uint64_t>::max()}),
                 target::ErrorCode::Internal),
        "authorization version cannot wrap");
}
void state_rules(target::TimePoint now) {
  const auto subject = human();
  const auto current_credential = credential(now);
  const auto initial = session(now);
  check(success(target::require_session_current(subject, current_credential, initial,
                                                target::TokenPurpose::Access, now)),
        "current session validates");
  check(error_is(target::require_session_current(subject, current_credential, initial,
                                                 target::TokenPurpose::Access,
                                                 initial.access_expires_at),
                 target::ErrorCode::Unauthenticated),
        "access exact expiry rejects");
  check(success(target::require_session_current(subject, current_credential, initial,
                                                target::TokenPurpose::Refresh,
                                                initial.access_expires_at)),
        "expired access does not invalidate still-live refresh");
  check(error_is(target::require_session_current(subject, current_credential, initial,
                                                 target::TokenPurpose::Refresh,
                                                 initial.refresh_expires_at),
                 target::ErrorCode::Unauthenticated),
        "refresh exact expiry rejects");
  auto rotated_credential = current_credential;
  rotated_credential.ref.version = {2};
  check(error_is(target::require_session_current(subject, rotated_credential, initial,
                                                 target::TokenPurpose::Access, now),
                 target::ErrorCode::Unauthenticated),
        "credential rotation invalidates old session binding immediately");
  auto expired_credential = current_credential;
  expired_credential.expires_at = now;
  check(error_is(target::require_session_current(subject, expired_credential, initial,
                                                 target::TokenPurpose::Access, now),
                 target::ErrorCode::Unauthenticated),
        "credential expiry gates otherwise-live session");
  auto disabled = subject;
  disabled.state = target::LifecycleState::Disabled;
  check(error_is(target::require_session_current(disabled, current_credential, initial,
                                                 target::TokenPurpose::Access, now),
                 target::ErrorCode::Unauthenticated),
        "disabled principal cannot use still-live token");
  const target::CurrentRefreshHandle input{initial.id,      subject.id, initial.credential,
                                           initial.refresh, 1,          initial.refresh_expires_at};
  const target::RefreshRecoveryLookup key{{"control"}, "refresh-key"};
  check(equals(target::check_refresh_state(subject, current_credential, initial, input, key, now),
               target::RefreshDecision::Rotate),
        "current input permits one rotation");
  auto after = initial;
  after.rotation = 2;
  after.version = {2};
  after.refresh.tag.bytes[0] = std::byte{1};
  const target::ConsumedRefreshHandle consumed{
      input, {{"control"}, initial.id, "refresh-key"}, now, now + target::kSecretReceiptRetention};
  check(equals(target::check_refresh_state(subject, current_credential, after, consumed, key,
                                           now + 1s),
               target::RefreshDecision::ReplayOriginal),
        "old input and original key recover successful lost response");
  auto new_key = key;
  new_key.idempotency_key = "new-key";
  check(error_is(target::check_refresh_state(subject, current_credential, after, consumed, new_key,
                                             now + 1s),
                 target::ErrorCode::Unauthenticated),
        "consumed input with new key never gets recovery authority");
  check(error_is(target::check_refresh_state(subject, current_credential, after, consumed, key,
                                             consumed.recovery_until),
                 target::ErrorCode::Unauthenticated),
        "24h history cutoff is exclusive");
  auto later = after;
  later.rotation = 3;
  check(error_is(target::check_refresh_state(subject, current_credential, later, consumed, key,
                                             now + 1s),
                 target::ErrorCode::Unauthenticated),
        "later session rotation blocks older successful response recovery");
  auto receipt = refresh_receipt(now);
  check(success(target::require_secret_replay(receipt, receipt.binding.scope,
                                              receipt.binding.parameters, receipt.binding.request,
                                              subject, current_credential, &after, now + 1s)),
        "eligible refresh success may proceed to AEAD opening");
  auto different = receipt.binding.request;
  different.tag.bytes[0] = std::byte{1};
  check(error_is(target::require_secret_replay(receipt, receipt.binding.scope,
                                               receipt.binding.parameters, different, subject,
                                               current_credential, &after, now + 1s),
                 target::ErrorCode::Conflict),
        "same stable scope and changed keyed request commitment conflicts");
  auto revoked = after;
  revoked.revoked = true;
  check(error_is(target::require_secret_replay(receipt, receipt.binding.scope,
                                               receipt.binding.parameters, different, subject,
                                               current_credential, &revoked, now + 1s),
                 target::ErrorCode::Unauthenticated),
        "revoked current session is checked before old receipt conflict");
  check(error_is(target::require_secret_replay(receipt, receipt.binding.scope,
                                               receipt.binding.parameters, different, subject,
                                               rotated_credential, &after, now + 1s),
                 target::ErrorCode::Unauthenticated),
        "current credential version beats old secret receipt");
  check(error_is(target::require_secret_replay(receipt, receipt.binding.scope,
                                               receipt.binding.parameters, receipt.binding.request,
                                               subject, current_credential, &after,
                                               receipt.binding.retain_until),
                 target::ErrorCode::Unauthenticated),
        "secret receipt exact expiry cannot decrypt");
  auto excessive = receipt;
  excessive.binding.retain_until += 1s;
  check(error_is(target::validate_secret_receipt(excessive), target::ErrorCode::InvalidArgument),
        "secret receipt cannot silently become ordinary seven-day retention");
  auto login = receipt;
  login.binding.scope = target::LoginReceiptScope{{"control"}, "user", "login-key"};
  login.binding.parameters = target::LoginRequestParameters{};
  login.binding.result_rotation = 1;
  check(success(target::require_secret_replay(login, login.binding.scope, login.binding.parameters,
                                              login.binding.request, subject, current_credential,
                                              &initial, now)),
        "login replay uses stable peer/login/key and current result rotation");
  check(error_is(target::require_secret_replay(login, login.binding.scope, login.binding.parameters,
                                               login.binding.request, subject, current_credential,
                                               &after, now),
                 target::ErrorCode::Unauthenticated),
        "login receipt cannot revive pre-refresh token pair");
  auto delivery = receipt;
  delivery.binding.scope = target::CredentialDeliveryScope{{"admin"}, {"password"}, "rotate-key"};
  delivery.binding.session.reset();
  delivery.binding.credential.version = {2};
  delivery.binding.parameters = target::CredentialDeliveryParameters{{1}};
  delivery.binding.result_rotation = 0;
  check(success(target::require_secret_replay(delivery, delivery.binding.scope,
                                              delivery.binding.parameters, delivery.binding.request,
                                              subject, rotated_credential, nullptr, now)),
        "authorized rotation can replay same original secret delivery");
  check(error_is(target::require_secret_replay(
                     delivery, delivery.binding.scope, target::CredentialDeliveryParameters{{2}},
                     delivery.binding.request, subject, rotated_credential, nullptr, now),
                 target::ErrorCode::Conflict),
        "same rotate key with changed expected version conflicts rather than splitting scope");
  target::DelegationClaims claims{};
  claims.subject = subject.id;
  claims.source_credential = current_credential.ref;
  claims.source_session = initial.id;
  claims.authorization_version = {77};
  check(success(target::require_delegation_origin_current(claims, subject, current_credential,
                                                          &initial, now)),
        "live explicit delegation origin checks");
  auto revoked_origin = initial;
  revoked_origin.revoked = true;
  check(error_is(target::require_delegation_origin_current(claims, subject, current_credential,
                                                           &revoked_origin, now),
                 target::ErrorCode::Unauthenticated),
        "source session revoke rejects even with unchanged authorization_version");
  check(error_is(target::require_delegation_origin_current(claims, subject, rotated_credential,
                                                           &initial, now),
                 target::ErrorCode::Unauthenticated),
        "source credential change rejects even with unchanged authorization_version");
  claims.source_session.reset();
  check(error_is(target::require_delegation_origin_current(claims, subject, current_credential,
                                                           nullptr, now),
                 target::ErrorCode::Unauthenticated),
        "human proof cannot drop source session to masquerade as workload");
  target::DelegationClaims parent{};
  parent.issuer = "identity";
  parent.audience = target::Service::Runtime;
  parent.subject = subject.id;
  parent.direct_actor = {"control"};
  parent.actor_chain = {{"control"}};
  parent.permissions = {{target::Permission::MessageRead, {target::ResourceKind::Account, {"A"}}}};
  parent.proof_id = {"parent-proof"};
  parent.authorization_version = {1};
  parent.issued_at = now;
  parent.not_before = now;
  parent.expires_at = now + 56s;
  parent.business_hops = 1;
  parent.source_credential = current_credential.ref;
  parent.source_session = initial.id;
  check(success(target::validate_delegation_shape(parent)),
        "first business edge is hops1 with Control-only actor chain");
  auto child = parent;
  child.audience = target::Service::Model;
  child.direct_actor = {"runtime"};
  child.actor_chain.push_back({"runtime"});
  child.business_hops = 2;
  child.proof_id = {"child-proof"};
  check(success(target::validate_delegation_exchange(parent, child, target::Service::Runtime,
                                                     {"runtime"})),
        "second business edge appends Runtime exactly once without counting Identity RPC");
  auto third = child;
  third.audience = target::Service::Host;
  third.direct_actor = {"model"};
  third.actor_chain.push_back({"model"});
  third.business_hops = 3;
  check(error_is(
            target::validate_delegation_exchange(child, third, target::Service::Model, {"model"}),
            target::ErrorCode::InvalidArgument),
        "third business edge is rejected");
  auto widened = child;
  widened.permissions.push_back(
      {target::Permission::MessageRead, {target::ResourceKind::Account, {"B"}}});
  check(error_is(target::validate_delegation_exchange(parent, widened, target::Service::Runtime,
                                                      {"runtime"}),
                 target::ErrorCode::PermissionDenied),
        "exchange cannot widen exact permission-resource pairs");
  widened = child;
  widened.source_session.reset();
  check(error_is(target::validate_delegation_exchange(parent, widened, target::Service::Runtime,
                                                      {"runtime"}),
                 target::ErrorCode::PermissionDenied),
        "exchange cannot erase original source session");
  auto zero = parent;
  zero.business_hops = 0;
  zero.actor_chain.clear();
  check(error_is(target::validate_delegation_shape(zero), target::ErrorCode::InvalidArgument),
        "zero-hop proof is not a valid business delegation");
  const target::AdministratorBootstrap marker{
      target::BootstrapState::Required, {1}, std::nullopt, std::nullopt};
  const auto bootstrapped = std::get<target::AdministratorBootstrap>(
      target::consume_administrator_bootstrap(marker, {1}, subject.id, now));
  check(bootstrapped.state == target::BootstrapState::Consumed && bootstrapped.version.value == 2,
        "offline bootstrap marker consumes once with version");
  check(error_is(target::consume_administrator_bootstrap(bootstrapped, {2}, subject.id, now),
                 target::ErrorCode::Conflict),
        "consumed bootstrap never reopens on retry");
  check(error_is(target::identity_deadline(target::TimePoint::max(), 1s),
                 target::ErrorCode::InvalidArgument),
        "expiry arithmetic never wraps");
}
void print_role_catalog() {
  constexpr std::array roles{std::pair{std::string_view{"admin"}, target::FixedRole::Admin},
                             std::pair{std::string_view{"operator"}, target::FixedRole::Operator},
                             std::pair{std::string_view{"viewer"}, target::FixedRole::Viewer}};
  for (const auto& [name, role] : roles) {
    for (const auto& [permission_name, permission] : target::kPermissionNames) {
      if (target::default_role_allows(role, permission)) {
        std::cout << name << '\t' << permission_name << '\n';
      }
    }
  }
}
int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view{argv[1]} == "--role-catalog") {
    print_role_catalog();
    return 0;
  }
  if (argc != 1) {
    std::cerr << "usage: identity_rules_test [--role-catalog]\n";
    return 2;
  }
  policy_rules();
  state_rules(target::TimePoint{24h});
}
