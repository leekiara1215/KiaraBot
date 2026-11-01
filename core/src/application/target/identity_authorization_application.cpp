// clang-format off
#include "kiarabot/core/application/target/identity_authorization_application.hpp"

#include <algorithm>
#include <utility>

#include "identity_security_application_detail.hpp"
// clang-format on

namespace kiarabot::core::target {
namespace {
namespace detail = identity_security_detail;
using detail::failure;
bool consumer(Service service) {
  return service == Service::Runtime || service == Service::Host || service == Service::Scheduler ||
         service == Service::Model;
}
bool trusted_clock(const SecurityClockSample& clock) {
  return clock.trusted && clock.maximum_known_skew >= std::chrono::milliseconds::zero() &&
         clock.maximum_known_skew <= kMaximumKnownClockSkew && clock.utc >= TimePoint{} &&
         clock.utc <= TimePoint::max() - std::chrono::seconds{2};
}
struct Watermark {
  SubjectId subject;
  ResourceVersion principal;
  ResourceVersion authorization;
  CredentialVersionRef credential;
  bool operator==(const Watermark&) const = default;
};
struct Authority {
  TimePoint now;
  TimePoint valid_until;
  ResourceVersion version;
  std::vector<Watermark> watermarks;
  std::optional<ResourceVersion> session_version;
  std::optional<std::uint64_t> session_rotation;
  bool allowed = true;
};
struct InitialRecipient {
  TimePoint now;
  Authority authority;
};
void record(Authority& result, const detail::SubjectAuthority& subject,
            const CredentialVersionRecord& credential) {
  result.watermarks.push_back({subject.principal.id, subject.principal.version,
                               subject.authorization.version, credential.ref});
  result.valid_until =
      std::min(result.valid_until, credential.expires_at.value_or(TimePoint::min()));
}
bool explicit_capability(const detail::SubjectAuthority& authority, Permission permission) {
  const PermissionBinding capability{permission,
                                     {ResourceKind::Principal, {authority.principal.id.value}}};
  return std::any_of(authority.assignments.begin(), authority.assignments.end(),
                     [&](const ExactAssignment& assignment) {
                       return std::find(assignment.explicit_permissions.begin(),
                                        assignment.explicit_permissions.end(),
                                        capability) != assignment.explicit_permissions.end();
                     });
}
// This is current registration evidence for a SIGNED actor, never a synthesized VerifiedPeer.
Status registered_actor(IdentitySecurityUnitOfWork& work, const CredentialVersionRecord& row,
                        const SubjectId& subject, Service service, TimePoint now,
                        Authority& snapshot) {
  auto current = work.credentials().current(row.ref.id);
  if (const auto* error = std::get_if<Failure>(&current)) {
    return *error;
  }
  const auto& head = std::get<std::optional<CredentialVersionRecord>>(current);
  if (!head || head->ref != row.ref || head->subject != subject) {
    return authentication_rejected();
  }
  auto found = detail::read_subject(work, subject);
  if (const auto* error = std::get_if<Failure>(&found)) {
    return *error;
  }
  const auto& authority = std::get<detail::SubjectAuthority>(found);
  auto valid = require_current_credential(authority.principal, *head, row.ref, now);
  if (const auto* error = std::get_if<Failure>(&valid)) {
    return *error;
  }
  const auto* certificate = std::get_if<CertificateBinding>(&head->material);
  if (authority.principal.kind != PrincipalKind::PlatformService || !certificate ||
      certificate->subject != subject || certificate->platform_service != service ||
      !std::holds_alternative<CertificateBinding>(row.material) ||
      certificate->fingerprint != std::get<CertificateBinding>(row.material).fingerprint ||
      !detail::bounded_text(certificate->registered_san)) {
    return authentication_rejected();
  }
  valid = detail::credential_owner_binding(authority.principal, *head);
  if (const auto* error = std::get_if<Failure>(&valid)) {
    return *error;
  }
  if (!explicit_capability(authority, Permission::IdentityDelegationIssue)) {
    return authentication_rejected();
  }
  record(snapshot, authority, *head);
  return std::monostate{};
}
class Application final : public IdentityAuthorizationApplication {
public:
  Application(IdentitySecurityUnitOfWorkFactory& transactions, DelegationCryptoPort& crypto,
              SecurityClockPort& clock, AuthorizationMethodPolicyPort& methods)
      : transactions_(transactions), crypto_(crypto), clock_(clock), methods_(methods) {}
  Outcome<CheckAuthorizationReply>
  check_authorization(const CheckAuthorizationRequest& request) override {
    try {
      return execute(request);
    } catch (const std::bad_alloc&) {
      return failure(ErrorCode::UpstreamUnavailable);
    } catch (...) {
      return failure(ErrorCode::Internal);
    }
  }

private:
  Outcome<TimePoint> recipient_current(detail::Work& work, const VerifiedPeer& peer,
                                       Authority* snapshot = nullptr) {
    const auto* platform = std::get_if<PlatformPeer>(&peer.binding.peer);
    if (!platform || !consumer(platform->service)) {
      return authentication_rejected();
    }
    auto read = detail::read_peer(*work.transaction, peer);
    if (const auto* error = std::get_if<Failure>(&read)) {
      return *error;
    }
    const auto& authority = std::get<detail::PeerAuthority>(read);
    auto valid = detail::check_peer(authority, peer, work.now, false);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    // Fixed service capability: explicit tuple only, never business scope or role expansion.
    if (!explicit_capability(authority.subject, Permission::IdentityAuthorizationCheck)) {
      return failure(ErrorCode::PermissionDenied);
    }
    if (snapshot) {
      record(*snapshot, authority.subject, authority.credential);
    }
    return std::min(authority.credential.expires_at.value_or(TimePoint::min()),
                    peer.binding.certificate.not_after);
  }
  Outcome<InitialRecipient> initial_recipient(const CheckAuthorizationRequest& request) {
    auto opened = detail::begin(transactions_, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& work = std::get<detail::Work>(opened);
    Authority snapshot{work.now, TimePoint::max(), {}, {}, {}, {}, true};
    auto valid = recipient_current(work, request.recipient, &snapshot);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    return InitialRecipient{work.now, std::move(snapshot)};
  }
  Outcome<Authority> read_authority(const CheckAuthorizationRequest& request,
                                    const DelegationClaims& claims,
                                    const AuthorizationMethodPolicy& policy, TimePoint started) {
    auto opened = detail::begin(transactions_, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& work = std::get<detail::Work>(opened);
    if (work.now < started) {
      return failure(ErrorCode::UpstreamUnavailable);
    }
    Authority result{work.now, claims.expires_at, {}, {}, {}, {}, true};
    auto recipient = recipient_current(work, request.recipient, &result);
    if (const auto* error = std::get_if<Failure>(&recipient)) {
      return *error;
    }
    result.valid_until = std::min(result.valid_until, std::get<TimePoint>(recipient));
    auto subject = detail::read_subject(*work.transaction, claims.subject);
    if (const auto* error = std::get_if<Failure>(&subject)) {
      return *error;
    }
    const auto& authority = std::get<detail::SubjectAuthority>(subject);
    auto credential = work.transaction->credentials().current(claims.source_credential.id);
    if (const auto* error = std::get_if<Failure>(&credential)) {
      return *error;
    }
    const auto& source = std::get<std::optional<CredentialVersionRecord>>(credential);
    if (!source) {
      return authentication_rejected();
    }
    std::optional<SessionSecurity> session;
    if (claims.source_session) {
      auto found = work.transaction->session_security().find(*claims.source_session);
      if (const auto* error = std::get_if<Failure>(&found)) {
        return *error;
      }
      session = std::get<std::optional<SessionSecurity>>(found);
    }
    auto valid = require_delegation_origin_current(claims, authority.principal, *source,
                                                   session ? &*session : nullptr, work.now);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    valid = detail::credential_owner_binding(authority.principal, *source);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    if (claims.authorization_version != authority.authorization.version ||
        work.now < claims.not_before || work.now >= claims.expires_at) {
      return authentication_rejected();
    }
    result.version = authority.authorization.version;
    record(result, authority, *source);
    if (session) {
      if (!session->control_peer || !valid_session_control_peer(*session->control_peer) ||
          session->control_peer->subject != claims.actor_chain.front()) {
        return authentication_rejected();
      }
      result.session_version = session->version;
      result.session_rotation = session->rotation;
      result.valid_until = std::min(result.valid_until, session->access_expires_at);
    }
    auto direct = work.transaction->credentials().by_certificate(claims.client_certificate);
    if (const auto* error = std::get_if<Failure>(&direct)) {
      return *error;
    }
    const auto& direct_row = std::get<std::optional<CredentialVersionRecord>>(direct);
    if (!direct_row) {
      return authentication_rejected();
    }
    const auto* direct_certificate = std::get_if<CertificateBinding>(&direct_row->material);
    if (!direct_certificate || direct_certificate->fingerprint != claims.client_certificate ||
        (session && claims.business_hops == 1 &&
         direct_row->ref.id != session->control_peer->credential)) {
      return authentication_rejected();
    }
    valid = registered_actor(*work.transaction, *direct_row, claims.direct_actor,
                             claims.business_hops == 1 ? Service::Control : Service::Runtime,
                             work.now, result);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    if (claims.business_hops == 2) {
      Outcome<std::optional<CredentialVersionRecord>> previous =
          session ? work.transaction->credentials().current(session->control_peer->credential)
                  : work.transaction->credentials().by_platform(
                        {Service::Control, claims.actor_chain.front()});
      if (const auto* error = std::get_if<Failure>(&previous)) {
        return *error;
      }
      const auto& row = std::get<std::optional<CredentialVersionRecord>>(previous);
      if (!row) {
        return authentication_rejected();
      }
      valid = registered_actor(*work.transaction, *row, claims.actor_chain.front(),
                               Service::Control, work.now, result);
      if (const auto* error = std::get_if<Failure>(&valid)) {
        return *error;
      }
    }
    for (const auto& resource : request.scope) {
      const PermissionBinding binding{policy.permission, resource};
      if (std::find(claims.permissions.begin(), claims.permissions.end(), binding) ==
          claims.permissions.end()) {
        result.allowed = false;
      }
      auto authorized =
          authorize_exact(authority.principal, policy.permission, resource, authority.assignments);
      if (const auto* error = std::get_if<Failure>(&authorized)) {
        if (error->code != ErrorCode::PermissionDenied) {
          return *error;
        }
        result.allowed = false;
      }
    }
    auto checkpoint = detail::final_check(work, request.context, result.valid_until, true);
    if (const auto* error = std::get_if<Failure>(&checkpoint)) {
      return *error;
    }
    if (started > TimePoint::max() - std::chrono::seconds{1} ||
        work.now > started + std::chrono::seconds{1}) {
      return failure(ErrorCode::Timeout, Recovery::Backoff);
    }
    result.now = work.now;
    return result;
  }
  Outcome<CheckAuthorizationReply> execute(const CheckAuthorizationRequest& request) {
    auto shape = detail::context_valid(request.context, false);
    if (const auto* error = std::get_if<Failure>(&shape)) {
      return *error;
    }
    if (!detail::bounded_text(request.full_method) || request.subject_proof.empty() ||
        request.subject_proof.size() > 32768 || request.scope.empty() ||
        request.scope.size() > 128 ||
        (request.known_authorization_version && request.known_authorization_version->value == 0)) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    auto resolved = methods_.resolve(request.full_method);
    if (const auto* error = std::get_if<Failure>(&resolved)) {
      return *error;
    }
    const auto& policy = std::get<std::optional<AuthorizationMethodPolicy>>(resolved);
    if (!policy) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    if (!consumer(policy->owner) || !known_permission(policy->permission) ||
        policy->scope_kinds.empty()) {
      return failure(ErrorCode::Internal);
    }
    for (std::size_t index = 0; index < request.scope.size(); ++index) {
      const auto& resource = request.scope[index];
      if (!valid_policy_resource(resource) ||
          std::find(policy->scope_kinds.begin(), policy->scope_kinds.end(), resource.kind) ==
              policy->scope_kinds.end() ||
          std::find(request.scope.begin(),
                    request.scope.begin() + static_cast<std::ptrdiff_t>(index),
                    resource) != request.scope.begin() + static_cast<std::ptrdiff_t>(index)) {
        return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
    }
    auto initial = initial_recipient(request);
    if (const auto* error = std::get_if<Failure>(&initial)) {
      return *error;
    }
    const auto& initial_authority = std::get<InitialRecipient>(initial);
    const auto started = initial_authority.now;
    const auto* platform = std::get_if<PlatformPeer>(&request.recipient.binding.peer);
    if (!platform || platform->service != policy->owner) {
      return authentication_rejected();
    }
    auto sample = clock_.sample();
    if (const auto* error = std::get_if<Failure>(&sample)) {
      return *error;
    }
    const auto& clock = std::get<SecurityClockSample>(sample);
    if (!trusted_clock(clock)) {
      return failure(ErrorCode::UpstreamUnavailable);
    }
    auto verified =
        crypto_.verify_for_authorization(request.subject_proof, request.recipient, clock);
    if (const auto* error = std::get_if<Failure>(&verified)) {
      return *error;
    }
    const auto& claims = std::get<AuthorityVerifiedDelegation>(verified).claims;
    if (std::holds_alternative<Failure>(validate_delegation_shape(claims)) ||
        claims.audience != platform->service) {
      return authentication_rejected();
    }
    auto before = read_authority(request, claims, *policy, started);
    if (const auto* error = std::get_if<Failure>(&before)) {
      return *error;
    }
    const auto& initial_watermark = initial_authority.authority.watermarks.front();
    if (std::get<Authority>(before).watermarks.front() != initial_watermark) {
      return authentication_rejected();
    }
    // Separate final readonly transaction: no material/clock/crypto call holds an Identity UoW.
    auto after = read_authority(request, claims, *policy, started);
    if (const auto* error = std::get_if<Failure>(&after)) {
      return *error;
    }
    const auto& previous = std::get<Authority>(before);
    const auto& current = std::get<Authority>(after);
    if (current.now < previous.now) {
      return failure(ErrorCode::UpstreamUnavailable);
    }
    if (current.watermarks != previous.watermarks ||
        current.session_version != previous.session_version ||
        current.session_rotation != previous.session_rotation) {
      return authentication_rejected();
    }
    auto final_sample = clock_.sample();
    if (const auto* error = std::get_if<Failure>(&final_sample)) {
      return *error;
    }
    const auto& final_clock = std::get<SecurityClockSample>(final_sample);
    if (!trusted_clock(final_clock) || final_clock.utc < clock.utc ||
        final_clock.monotonic < clock.monotonic) {
      return failure(ErrorCode::UpstreamUnavailable);
    }
    if (clock.monotonic > std::chrono::steady_clock::time_point::max() - std::chrono::seconds{1} ||
        final_clock.monotonic > clock.monotonic + std::chrono::seconds{1}) {
      return failure(ErrorCode::Timeout, Recovery::Backoff);
    }
    const auto final_now = std::max(final_clock.utc, current.now);
    if (final_now >= std::min(current.valid_until, request.context.deadline)) {
      return failure(ErrorCode::Timeout, Recovery::Backoff);
    }
    if (request.known_authorization_version &&
        request.known_authorization_version->value > current.version.value) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    return CheckAuthorizationReply{
        current.allowed, current.version, current.now,
        current.allowed ? std::nullopt : std::optional{failure(ErrorCode::PermissionDenied)}};
  }
  IdentitySecurityUnitOfWorkFactory& transactions_;
  DelegationCryptoPort& crypto_;
  SecurityClockPort& clock_;
  AuthorizationMethodPolicyPort& methods_;
};
} // namespace
Outcome<std::unique_ptr<IdentityAuthorizationApplication>>
make_identity_authorization_application(IdentitySecurityUnitOfWorkFactory& transactions,
                                        DelegationCryptoPort& crypto, SecurityClockPort& clock,
                                        AuthorizationMethodPolicyPort& methods) {
  try {
    return std::unique_ptr<IdentityAuthorizationApplication>{
        std::make_unique<Application>(transactions, crypto, clock, methods)};
  } catch (const std::bad_alloc&) {
    return failure(ErrorCode::UpstreamUnavailable);
  } catch (...) {
    return failure(ErrorCode::Internal);
  }
}
} // namespace kiarabot::core::target
