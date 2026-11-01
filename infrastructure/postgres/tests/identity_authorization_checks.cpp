// clang-format off
#include "identity_authorization_checks.hpp"

#include <iostream>
#include <stdexcept>

#include "identity_application_tls.hpp"
#include "kiarabot/core/application/target/identity_admin_application.hpp"
#include "kiarabot/infrastructure/identity_crypto/peer_certificate.hpp"
// clang-format on

namespace kiarabot::infrastructure::postgres::test {
namespace {
namespace t = core::target;
namespace crypto = identity_crypto;
void check(bool value) {
  if (!value) {
    throw std::runtime_error("online authorization fixture assertion failed");
  }
}
template <typename Value> Value take(t::Outcome<Value> result) {
  if (const auto* error = std::get_if<t::Failure>(&result)) {
    std::cerr << "P03 assertion: online authorization stable code " << static_cast<int>(error->code)
              << '\n';
  }
  check(std::holds_alternative<Value>(result));
  return std::move(std::get<Value>(result));
}
template <typename Value> void expect(const t::Outcome<Value>& result, t::ErrorCode code) {
  check(std::holds_alternative<t::Failure>(result) && std::get<t::Failure>(result).code == code);
}
void commit(t::IdentitySecurityUnitOfWork& work) {
  check(work.commit() == t::CommitResult::Committed);
}
t::IdentityRequestContext context(t::SecurityClockPort& clock) {
  return {"10000000-0000-4000-8000-000000000011",
          "10000000-0000-4000-8000-000000000012",
          {},
          take(clock.sample()).utc + std::chrono::seconds{1}};
}
class Methods final : public t::AuthorizationMethodPolicyPort {
public:
  t::Outcome<std::optional<t::AuthorizationMethodPolicy>>
  resolve(std::string_view method) override {
    // Test-only trusted fixture mapping of the real contract. Production descriptor resolution is
    // independently owned by the RPC boundary; this fixture never decodes JWT or authorizes TLS.
    return method == "/kiarabot.v1.ModelService/Chat"
               ? std::optional{t::AuthorizationMethodPolicy{
                     t::Service::Model, t::Permission::ModelChat, {t::ResourceKind::Model}}}
               : std::nullopt;
  }
};
} // namespace
void identity_authorization_checks(t::IdentitySecurityUnitOfWorkFactory& factory,
                                   t::DelegationCryptoPort& delegations,
                                   t::SecurityClockPort& clock, const t::SubjectId& user,
                                   const t::CredentialId& credential, const t::SessionId& session,
                                   const t::SubjectId& control) {
  const t::SubjectId runtime{"20000000-0000-4000-8000-000000000011"};
  const t::SubjectId model{"20000000-0000-4000-8000-000000000012"};
  const t::CredentialId runtime_credential{"30000000-0000-4000-8000-000000000011"};
  const t::CredentialId model_credential{"30000000-0000-4000-8000-000000000012"};
  const t::ResourceRef resource{t::ResourceKind::Model, {"50000000-0000-4000-8000-000000000011"}};
  const auto runtime_tls = platform_tls_evidence(runtime, t::Service::Runtime);
  const auto model_tls = platform_tls_evidence(model, t::Service::Model);
  check(runtime_tls.registration.fingerprint != model_tls.registration.fingerprint);
  auto certificates = take(crypto::make_peer_certificate_crypto(
      {{runtime_tls.root_pem, model_tls.root_pem}, crypto::PeerSanKind::Uri}));
  t::IdentityAdminApplication admin(factory, *certificates);
  t::ExactAssignment source_grant{{"40000000-0000-4000-8000-000000000011"},
                                  user,
                                  {},
                                  {{t::Permission::ModelChat, resource}},
                                  {1}};
  t::ExactAssignment capability{
      {"40000000-0000-4000-8000-000000000012"},
      model,
      {},
      {{t::Permission::IdentityAuthorizationCheck, {t::ResourceKind::Principal, {model.value}}}},
      {1}};
  {
    auto work = take(factory.begin());
    const auto now = take(work->current_time());
    const auto enroll = [&](const t::SubjectId& subject, const t::CredentialId& id,
                            const t::CertificateBinding& certificate) {
      const t::ResourceRef binding{t::ResourceKind::Principal, {subject.value}};
      take(work->principals().save({subject, t::PrincipalKind::PlatformService, binding, {1}}, {}));
      take(work->authorizations().save({subject, {1}, false}, {}));
      take(work->credentials().save({{id, {1}},
                                     subject,
                                     certificate,
                                     t::CredentialState::Active,
                                     now,
                                     {},
                                     binding,
                                     certificate.not_after},
                                    {}));
    };
    enroll(runtime, runtime_credential, runtime_tls.registration);
    enroll(model, model_credential, model_tls.registration);
    take(work->exact_assignments().save(
        {{"40000000-0000-4000-8000-000000000013"},
         runtime,
         {},
         {{t::Permission::IdentityDelegationIssue, {t::ResourceKind::Principal, {runtime.value}}}},
         {1}},
        {}));
    take(work->exact_assignments().save(source_grant, {}));
    take(work->exact_assignments().save(capability, {}));
    auto authorization = take(work->authorizations().find(user));
    check(authorization.has_value());
    const auto previous = authorization->version;
    ++authorization->version.value;
    take(work->authorizations().save(*authorization, previous));
    commit(*work);
  }
  const auto peer = [&](const ControlTlsEvidence& tls) {
    return take(admin.authenticate_peer({tls.registration.fingerprint, context(clock).deadline},
                                        tls.leaf_der));
  };
  const auto claims = [&] {
    const auto sample = take(clock.sample());
    const auto issued = t::TimePoint{
        std::chrono::duration_cast<std::chrono::seconds>(sample.utc.time_since_epoch())};
    auto work = take(factory.begin());
    const auto authorization = take(work->authorizations().find(user));
    const auto source = take(work->credentials().current(credential));
    check(authorization && source);
    return t::DelegationClaims{"kiarabot-identity",
                               t::Service::Model,
                               user,
                               runtime,
                               {control, runtime},
                               {{t::Permission::ModelChat, resource}},
                               {"70000000-0000-4000-8000-000000000011"},
                               runtime_tls.registration.fingerprint,
                               authorization->version,
                               issued,
                               issued,
                               issued + std::chrono::seconds{56},
                               2,
                               source->ref,
                               session};
  };
  Methods methods;
  auto app = take(t::make_identity_authorization_application(factory, delegations, clock, methods));
  auto signed_claims = claims();
  auto proof = take(delegations.sign(signed_claims, {"delegation-key"}));
  const auto request = [&](t::SecretView bytes) {
    return t::CheckAuthorizationRequest{
        context(clock), peer(model_tls), bytes, "/kiarabot.v1.ModelService/Chat", {resource}, {}};
  };
  // A real business sender check binds Runtime. A Model leaf is not substituted for JWT cnf.
  const auto runtime_peer = peer(runtime_tls);
  take(delegations.verify(proof->bytes(), t::Service::Model, runtime_peer, take(clock.sample())));
  const auto model_peer = peer(model_tls);
  expect(delegations.verify(proof->bytes(), t::Service::Model, model_peer, take(clock.sample())),
         t::ErrorCode::Unauthenticated);
  auto reply = take(app->check_authorization(request(proof->bytes())));
  check(reply.allowed && !reply.denial &&
        reply.authorization_version == signed_claims.authorization_version &&
        reply.checked_at >= signed_claims.issued_at);
  auto missing_binding = signed_claims;
  missing_binding.permissions[0].resource.id.value = "50000000-0000-4000-8000-000000000012";
  auto narrowed = take(delegations.sign(missing_binding, {"delegation-key"}));
  reply = take(app->check_authorization(request(narrowed->bytes())));
  check(!reply.allowed && reply.denial && reply.denial->code == t::ErrorCode::PermissionDenied);
  {
    auto work = take(factory.begin());
    const auto previous = source_grant.version;
    ++source_grant.version.value;
    source_grant.explicit_permissions[0].permission = t::Permission::ModelRead;
    take(work->exact_assignments().save(source_grant, previous));
    auto authorization = take(work->authorizations().find(user));
    const auto version = authorization->version;
    ++authorization->version.value;
    take(work->authorizations().save(*authorization, version));
    commit(*work);
  }
  expect(app->check_authorization(request(proof->bytes())), t::ErrorCode::Unauthenticated);
  signed_claims = claims();
  proof = take(delegations.sign(signed_claims, {"delegation-key"}));
  reply = take(app->check_authorization(request(proof->bytes())));
  check(!reply.allowed && reply.denial && reply.denial->code == t::ErrorCode::PermissionDenied);
  {
    auto work = take(factory.begin());
    const auto previous = capability.version;
    ++capability.version.value;
    capability.explicit_permissions[0].permission = t::Permission::IdentityKeysRead;
    take(work->exact_assignments().save(capability, previous));
    auto authorization = take(work->authorizations().find(model));
    const auto version = authorization->version;
    ++authorization->version.value;
    take(work->authorizations().save(*authorization, version));
    commit(*work);
  }
  expect(app->check_authorization(request(proof->bytes())), t::ErrorCode::PermissionDenied);
  {
    auto work = take(factory.begin());
    const auto previous = capability.version;
    ++capability.version.value;
    capability.explicit_permissions[0].permission = t::Permission::IdentityAuthorizationCheck;
    take(work->exact_assignments().save(capability, previous));
    auto authorization = take(work->authorizations().find(model));
    const auto version = authorization->version;
    ++authorization->version.value;
    take(work->authorizations().save(*authorization, version));
    auto principal = take(work->principals().find(control));
    const auto principal_version = principal->version;
    ++principal->version.value;
    principal->state = t::LifecycleState::Disabled;
    take(work->principals().save(*principal, principal_version));
    commit(*work);
  }
  expect(app->check_authorization(request(proof->bytes())), t::ErrorCode::Unauthenticated);
  std::cout << "P04 CheckAuthorization actual TLS peers + mature JWT + current PostgreSQL "
               "authority passed\n";
}
} // namespace kiarabot::infrastructure::postgres::test
