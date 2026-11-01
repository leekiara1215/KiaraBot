#include <cstdlib>
#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>

#include "kiarabot/core/domain/target/identity_credential_rules.hpp"
#include "kiarabot/core/domain/target/persistence.hpp"
#include "kiarabot/core/ports/target/identity_crypto.hpp"
#include "kiarabot/core/ports/target/identity_repository.hpp"

namespace target = kiarabot::core::target;
static_assert(std::is_base_of_v<target::IdentityUnitOfWork, target::IdentitySecurityUnitOfWork>);
static_assert(
    std::is_same_v<decltype(std::declval<target::IdentitySecurityUnitOfWorkFactory&>().begin()),
                   target::Outcome<std::unique_ptr<target::IdentitySecurityUnitOfWork>>>);
static_assert(std::is_same_v<decltype(std::declval<target::IdentityUnitOfWorkFactory&>().begin()),
                             target::Outcome<std::unique_ptr<target::IdentityUnitOfWork>>>);
static_assert(!std::is_copy_constructible_v<target::EphemeralTokenPair>);
static_assert(!std::is_convertible_v<target::EncryptedSecretReceipt, target::ReplayResult>);
static_assert(!std::is_convertible_v<target::OwnedSecret, target::ReplayResult>);
static_assert(!std::is_convertible_v<target::SecretReplayPayload, target::ReplayResult>);
static_assert(!std::is_copy_constructible_v<target::SecretReplayPayload>);
static_assert(std::is_same_v<
              decltype(std::declval<target::IdentitySessionSecurityRepository&>().resolve_refresh(
                  std::declval<const target::TokenVerifier&>(),
                  std::declval<const target::RefreshRecoveryLookup&>(), target::TimePoint{})),
              target::Outcome<std::optional<target::RefreshHandle>>>);
static_assert(std::is_same_v<decltype(std::declval<target::IdentitySessionSecurityRepository&>()
                                          .list_metadata(std::declval<const target::SubjectId&>(),
                                                         std::size_t{1})),
                             target::Outcome<std::vector<target::SessionMetadata>>>);
static_assert(
    std::is_same_v<decltype(std::declval<target::IdentityCredentialRepository&>().by_certificate(
                       std::declval<const target::Sha256Digest&>())),
                   target::Outcome<std::optional<target::CredentialVersionRecord>>>);
static_assert(target::kAccessTokenLifetime == std::chrono::minutes{15});
static_assert(target::kRefreshTokenLifetime == std::chrono::hours{24 * 30});
static_assert(target::kSecretReceiptRetention == std::chrono::hours{24});
void check(bool passed, const char* scenario) {
  if (!passed) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}
int main() {
  const target::TimePoint created{std::chrono::hours{1}};
  auto stub = std::get<target::CredentialVersionRecord>(target::make_pending_credential(
      {"credential"}, {"principal"}, target::CredentialKind::Password,
      {target::ResourceKind::Principal, {"principal"}}, created));
  check(stub.state == target::CredentialState::Pending && stub.ref.version.value == 1 &&
            std::holds_alternative<target::PendingCredentialMaterial>(stub.material),
        "registration creates explicit pending version-one stub without a password verifier");
  stub.state = target::CredentialState::Active;
  check(std::holds_alternative<target::Failure>(target::validate_credential_record(stub)),
        "pending material can never authenticate as active");
  stub.material = target::PasswordVerifier{"", {1}};
  check(std::holds_alternative<target::Failure>(target::validate_credential_record(stub)),
        "active empty password hash is invalid");
  stub.material = target::PasswordVerifier{"mature-adapter-output", {1}};
  check(std::holds_alternative<target::Failure>(target::validate_credential_record(stub)),
        "active credential cannot fabricate absent expiry");
  stub.expires_at = created + std::chrono::hours{1};
  check(std::holds_alternative<std::monostate>(target::validate_credential_record(stub)),
        "active material has structural verifier while actual crypto validation remains external");
  check(
      std::get<target::CredentialStatusSnapshot>(target::credential_status(stub, *stub.expires_at))
              .state == target::CredentialStatusState::Expired,
      "credential expires exactly at authoritative cutoff");
  stub.state = target::CredentialState::Revoked;
  stub.revoked_at = created;
  check(
      std::get<target::CredentialStatusSnapshot>(target::credential_status(stub, *stub.expires_at))
              .state == target::CredentialStatusState::Revoked,
      "revocation status wins over expiry projection");
  stub.revoked_at.reset();
  stub.state = target::CredentialState::Pending;
  check(std::holds_alternative<target::Failure>(target::validate_credential_record(stub)),
        "pending state cannot conceal material that was already issued");
  for (const auto kind :
       {target::CredentialKind::WorkloadBootstrap, target::CredentialKind::CertificateBinding}) {
    const auto result =
        target::make_pending_credential({"credential"}, {"principal"}, kind,
                                        {target::ResourceKind::Instance, {"instance"}}, created);
    check(std::holds_alternative<target::CredentialVersionRecord>(result),
          "non-password registration also has a typed no-secret pending material");
  }
  check(std::holds_alternative<target::Failure>(target::make_pending_credential(
            {"credential"}, {"principal"}, target::CredentialKind::Unspecified,
            {target::ResourceKind::Principal, {"principal"}}, created)),
        "unspecified pending credential kind rejected");
  auto pending = std::get<target::CredentialVersionRecord>(
      target::make_pending_credential({"pending"}, {"principal"}, target::CredentialKind::Password,
                                      {target::ResourceKind::Principal, {"principal"}}, created));
  pending.expires_at = created + std::chrono::hours{1};
  check(std::holds_alternative<target::Failure>(target::validate_credential_record(pending)),
        "Pending cannot fabricate a credential expiry before secret issuance");
  auto bounded = std::get<target::CredentialVersionRecord>(target::make_pending_credential(
      {"bounded"}, {"principal"}, target::CredentialKind::WorkloadBootstrap,
      {target::ResourceKind::Instance, {"instance"}}, created));
  bounded.state = target::CredentialState::Active;
  bounded.material =
      target::WorkloadBootstrapVerifier{{target::TokenPurpose::WorkloadBootstrap, {"key"}, {}},
                                        bounded.binding,
                                        {target::Service::Runtime},
                                        created + std::chrono::hours{1}};
  bounded.expires_at = created + std::chrono::hours{2};
  check(std::holds_alternative<target::Failure>(target::validate_credential_record(bounded)),
        "credential expiry cannot outlive bootstrap verifier expiry");
  bounded.material = target::CertificateBinding{{"principal"},    {},
                                                "registered-san", target::Service::Control,
                                                created,          created + std::chrono::hours{1}};
  check(std::holds_alternative<target::Failure>(target::validate_credential_record(bounded)),
        "credential expiry cannot outlive the actual certificate");
  check(static_cast<unsigned int>(target::Permission::MessageRead) == 40,
        "G08 explicit message.read is present without becoming a role grant");
  const target::SecretReceiptScope refresh_scope =
      target::RefreshReceiptScope{{"control"}, {"session"}, "key"};
  const target::SecretRequestParameters refresh_first = target::RefreshRequestParameters{1};
  const target::SecretRequestParameters refresh_changed = target::RefreshRequestParameters{2};
  check(std::holds_alternative<std::monostate>(
            target::validate_secret_request_parameters(refresh_scope, refresh_first)),
        "refresh scope is stable while the input rotation is authenticated request content");
  check(refresh_first != refresh_changed,
        "changing rotation cannot disappear from request parameters");
  const target::SecretReceiptScope delivery_scope =
      target::CredentialDeliveryScope{{"actor"}, {"credential"}, "key"};
  const target::SecretRequestParameters delivery_first = target::CredentialDeliveryParameters{{1}};
  const target::SecretRequestParameters delivery_changed =
      target::CredentialDeliveryParameters{{2}};
  check(std::holds_alternative<std::monostate>(
            target::validate_secret_request_parameters(delivery_scope, delivery_first)),
        "RotateCredential expected version is content even without a supplied secret");
  check(delivery_first != delivery_changed,
        "changing expected version must reach commitment adapter");
  target::SecretReceiptBinding delivery_binding{delivery_scope,
                                                {"actor"},
                                                std::nullopt,
                                                {{"credential"}, {2}},
                                                delivery_first,
                                                0,
                                                {},
                                                created,
                                                created + target::kSecretReceiptRetention};
  auto changed_binding = delivery_binding;
  changed_binding.parameters = delivery_changed;
  check(delivery_binding.scope == changed_binding.scope && delivery_binding != changed_binding,
        "same caller/credential/key stays same scope while changed expected version changes AEAD "
        "binding");
  target::SecretReceiptBinding refresh_binding{refresh_scope,
                                               {"principal"},
                                               target::SessionId{"session"},
                                               {{"credential"}, {1}},
                                               refresh_first,
                                               2,
                                               {},
                                               created,
                                               created + target::kSecretReceiptRetention};
  changed_binding = refresh_binding;
  changed_binding.parameters = refresh_changed;
  check(refresh_binding.scope == changed_binding.scope && refresh_binding != changed_binding,
        "same caller/session/key does not split into another receipt for a changed input rotation");
  check(std::holds_alternative<target::Failure>(
            target::validate_secret_request_parameters(delivery_scope, refresh_first)),
        "receipt method scope and typed parameters cannot be mixed");
  target::DelegationClaims origin{};
  origin.subject = {"principal"};
  check(std::holds_alternative<target::Failure>(target::validate_delegation_origin(origin)),
        "missing source credential cannot become a valid proof origin");
  origin.source_credential = {{"credential"}, {1}};
  origin.source_session = target::SessionId{"source-session"};
  auto exchanged = origin;
  check(std::holds_alternative<std::monostate>(target::require_preserved_origin(origin, exchanged)),
        "exchange preserves signed original source");
  exchanged.source_session.reset();
  check(
      std::holds_alternative<target::Failure>(target::require_preserved_origin(origin, exchanged)),
      "exchange cannot remove a source session and masquerade as workload origin");
  exchanged = origin;
  exchanged.source_credential.version.value = 2;
  check(
      std::holds_alternative<target::Failure>(target::require_preserved_origin(origin, exchanged)),
      "exchange cannot replace originating credential version");
  target::SessionSecurity session{};
  session.id = {"session"};
  session.subject = {"principal"};
  session.created_at = created;
  session.access_expires_at = created + target::kAccessTokenLifetime;
  session.refresh_expires_at = created + target::kRefreshTokenLifetime;
  session.rotation = 7;
  session.version = {9};
  const auto metadata = target::session_metadata(session);
  check(metadata.created_at == created && metadata.expires_at == session.refresh_expires_at &&
            metadata.expires_at != session.access_expires_at && metadata.rotation == 7,
        "session projection preserves immutable creation and refresh expiry instead of query time");
}
