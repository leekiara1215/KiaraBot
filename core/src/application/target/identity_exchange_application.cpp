// clang-format off
#include "kiarabot/core/application/target/identity_exchange_application.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "identity_security_application_detail.hpp"
// clang-format on

namespace kiarabot::core::target {
namespace {
namespace detail = identity_security_detail;
using detail::failure;
struct SourceAuthority {
  detail::SubjectAuthority subject;
  CredentialVersionRecord credential;
  std::optional<SessionSecurity> session;
};
struct ExchangeAuthority {
  detail::PeerAuthority runtime;
  detail::PeerAuthority control;
  SourceAuthority source;
  TimePoint checked_at;
  TimePoint latest_expiry;
};
Status valid_clock(const SecurityClockSample& clock) {
  if (!clock.trusted || clock.maximum_known_skew < std::chrono::milliseconds::zero() ||
      clock.maximum_known_skew > kMaximumKnownClockSkew || clock.utc < TimePoint{} ||
      clock.utc > TimePoint::max() - kMaximumKnownClockSkew) {
    return failure(ErrorCode::UpstreamUnavailable);
  }
  return std::monostate{};
}
Status valid_request(const ExchangeIdentityDelegationRequest& request,
                     const IdentityDelegationOptions& options) {
  auto context = detail::context_valid(request.context, false);
  if (const auto* error = std::get_if<Failure>(&context)) {
    return *error;
  }
  const auto* runtime = std::get_if<PlatformPeer>(&request.recipient.binding.peer);
  if (!runtime || runtime->service != Service::Runtime ||
      !detail::bounded_text(runtime->subject.value)) {
    return authentication_rejected();
  }
  if (!detail::bounded_text(options.issuer) || !detail::bounded_text(options.signing_key.value) ||
      (request.audience != Service::Model && request.audience != Service::Scheduler) ||
      request.permissions.empty() || request.permissions.size() > kMaximumDelegationBindings ||
      request.parent_proof.empty() || request.parent_proof.size() > 32768) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  std::set<std::pair<Permission, ResourceRef>, bool (*)(const std::pair<Permission, ResourceRef>&,
                                                        const std::pair<Permission, ResourceRef>&)>
      unique([](const auto& left, const auto& right) {
        return left.first != right.first ? left.first < right.first
                                         : PolicyResourceLess{}(left.second, right.second);
      });
  for (const auto& binding : request.permissions) {
    if (!known_permission(binding.permission) || !valid_policy_resource(binding.resource) ||
        !unique.emplace(binding.permission, binding.resource).second) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
  }
  return std::monostate{};
}
// This is current registration evidence identified by a SIGNED previous-peer fingerprint.
// It is never converted into a fabricated current TLS VerifiedPeer.
Outcome<detail::PeerAuthority> previous_control(IdentitySecurityUnitOfWork& work,
                                                const DelegationClaims& parent, TimePoint now) {
  auto indexed = work.credentials().by_certificate(parent.client_certificate);
  if (const auto* error = std::get_if<Failure>(&indexed)) {
    return *error;
  }
  const auto& row = std::get<std::optional<CredentialVersionRecord>>(indexed);
  if (!row) {
    return authentication_rejected();
  }
  auto head = work.credentials().current(row->ref.id);
  if (const auto* error = std::get_if<Failure>(&head)) {
    return *error;
  }
  const auto& credential = std::get<std::optional<CredentialVersionRecord>>(head);
  if (!credential || credential->ref != row->ref || credential->subject != parent.direct_actor) {
    return authentication_rejected();
  }
  auto subject = detail::read_subject(work, credential->subject);
  if (const auto* error = std::get_if<Failure>(&subject)) {
    return *error;
  }
  auto& authority = std::get<detail::SubjectAuthority>(subject);
  auto current = require_current_credential(authority.principal, *credential, row->ref, now);
  if (const auto* error = std::get_if<Failure>(&current)) {
    return *error;
  }
  auto owner = detail::credential_owner_binding(authority.principal, *credential);
  if (const auto* error = std::get_if<Failure>(&owner)) {
    return *error;
  }
  const auto* certificate = std::get_if<CertificateBinding>(&credential->material);
  if (!certificate || authority.principal.kind != PrincipalKind::PlatformService ||
      certificate->platform_service != Service::Control ||
      certificate->subject != parent.direct_actor ||
      certificate->fingerprint != parent.client_certificate ||
      !detail::bounded_text(certificate->registered_san)) {
    return authentication_rejected();
  }
  return detail::PeerAuthority{*credential, std::move(authority)};
}
Outcome<SourceAuthority> source_authority(IdentitySecurityUnitOfWork& work,
                                          const DelegationClaims& parent, TimePoint now) {
  auto subject = detail::read_subject(work, parent.subject);
  if (const auto* error = std::get_if<Failure>(&subject)) {
    return *error;
  }
  auto head = work.credentials().current(parent.source_credential.id);
  if (const auto* error = std::get_if<Failure>(&head)) {
    return *error;
  }
  const auto& credential = std::get<std::optional<CredentialVersionRecord>>(head);
  if (!credential) {
    return authentication_rejected();
  }
  auto& authority = std::get<detail::SubjectAuthority>(subject);
  std::optional<SessionSecurity> session;
  if (parent.source_session) {
    auto found = detail::read_session(work, *parent.source_session);
    if (const auto* error = std::get_if<Failure>(&found)) {
      return *error;
    }
    session = std::get<detail::SessionAuthority>(found).session;
  }
  auto owner = detail::credential_owner_binding(authority.principal, *credential);
  if (const auto* error = std::get_if<Failure>(&owner)) {
    return *error;
  }
  auto current = require_delegation_origin_current(parent, authority.principal, *credential,
                                                   session ? &*session : nullptr, now);
  if (const auto* error = std::get_if<Failure>(&current)) {
    return *error;
  }
  if (parent.authorization_version != authority.authorization.version) {
    return authentication_rejected();
  }
  return SourceAuthority{std::move(authority), *credential, std::move(session)};
}
Status current_version(IdentitySecurityUnitOfWork& work, const detail::SubjectAuthority& subject) {
  auto found = work.authorizations().find(subject.principal.id);
  if (const auto* error = std::get_if<Failure>(&found)) {
    return *error;
  }
  const auto& row = std::get<std::optional<AuthorizationState>>(found);
  if (!row || row->subject != subject.principal.id || row->version.value == 0) {
    return failure(ErrorCode::Internal);
  }
  if (row->version != subject.authorization.version) {
    return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
  }
  return std::monostate{};
}
Outcome<ExchangeAuthority> authorize_exchange(IdentitySecurityUnitOfWorkFactory& transactions,
                                              const ExchangeIdentityDelegationRequest& request,
                                              const DelegationClaims& parent) {
  auto opened = detail::begin(transactions, request.context);
  if (const auto* error = std::get_if<Failure>(&opened)) {
    return *error;
  }
  auto& work = std::get<detail::Work>(opened);
  auto runtime = detail::read_peer(*work.transaction, request.recipient);
  if (const auto* error = std::get_if<Failure>(&runtime)) {
    return *error;
  }
  auto& actual = std::get<detail::PeerAuthority>(runtime);
  auto peer = detail::check_peer(actual, request.recipient, work.now, false);
  if (const auto* error = std::get_if<Failure>(&peer)) {
    return *error;
  }
  auto control = previous_control(*work.transaction, parent, work.now);
  if (const auto* error = std::get_if<Failure>(&control)) {
    return *error;
  }
  auto source = source_authority(*work.transaction, parent, work.now);
  if (const auto* error = std::get_if<Failure>(&source)) {
    return *error;
  }
  auto& previous = std::get<detail::PeerAuthority>(control);
  auto& original = std::get<SourceAuthority>(source);
  if (work.now < parent.not_before || work.now >= parent.expires_at) {
    return authentication_rejected();
  }
  for (const auto* delegator : {&actual.subject, &previous.subject}) {
    auto allowed = authorize_exact(delegator->principal, Permission::IdentityDelegationIssue,
                                   {ResourceKind::Principal, {delegator->principal.id.value}},
                                   delegator->assignments);
    if (const auto* error = std::get_if<Failure>(&allowed)) {
      return *error;
    }
  }
  for (const auto& binding : request.permissions) {
    if (std::find(parent.permissions.begin(), parent.permissions.end(), binding) ==
        parent.permissions.end()) {
      return failure(ErrorCode::PermissionDenied);
    }
    auto allowed = authorize_exact(original.subject.principal, binding.permission, binding.resource,
                                   original.subject.assignments);
    if (const auto* error = std::get_if<Failure>(&allowed)) {
      return *error;
    }
  }
  for (const auto* subject : {&actual.subject, &previous.subject, &original.subject}) {
    auto version = current_version(*work.transaction, *subject);
    if (const auto* error = std::get_if<Failure>(&version)) {
      return *error;
    }
  }
  const auto* control_certificate = std::get_if<CertificateBinding>(&previous.credential.material);
  if (!control_certificate) {
    return failure(ErrorCode::Internal);
  }
  auto expiry = std::min({parent.expires_at, request.context.deadline,
                          actual.credential.expires_at.value_or(TimePoint::min()),
                          request.recipient.binding.certificate.not_after,
                          previous.credential.expires_at.value_or(TimePoint::min()),
                          control_certificate->not_after,
                          original.credential.expires_at.value_or(TimePoint::min())});
  if (original.session) {
    expiry = std::min(expiry, original.session->access_expires_at);
  }
  auto checkpoint = detail::final_check(work, request.context, expiry, true);
  if (const auto* error = std::get_if<Failure>(&checkpoint)) {
    return *error;
  }
  return ExchangeAuthority{std::move(actual), std::move(previous), std::move(original), work.now,
                           expiry};
}
bool same_authority(const ExchangeAuthority& first, const ExchangeAuthority& final) {
  for (const auto& peers : {std::make_pair(&first.runtime, &final.runtime),
                            std::make_pair(&first.control, &final.control)}) {
    if (peers.first->credential.ref != peers.second->credential.ref ||
        peers.first->subject.principal.id != peers.second->subject.principal.id ||
        peers.first->subject.principal.version != peers.second->subject.principal.version ||
        peers.first->subject.authorization.version != peers.second->subject.authorization.version) {
      return false;
    }
  }
  if (first.source.credential.ref != final.source.credential.ref ||
      first.source.subject.principal.id != final.source.subject.principal.id ||
      first.source.subject.principal.version != final.source.subject.principal.version ||
      first.source.subject.authorization.version != final.source.subject.authorization.version ||
      first.source.session.has_value() != final.source.session.has_value()) {
    return false;
  }
  if (first.source.session && final.source.session) {
    return first.source.session->id == final.source.session->id &&
           first.source.session->version == final.source.session->version &&
           first.source.session->rotation == final.source.session->rotation;
  }
  return true;
}
} // namespace

IdentityExchangeApplication::IdentityExchangeApplication(
    IdentitySecurityUnitOfWorkFactory& transactions, DelegationCryptoPort& delegations,
    RandomPort& random, SecurityClockPort& clock, IdentityDelegationOptions options)
    : transactions_(transactions), delegations_(delegations), random_(random), clock_(clock),
      options_(std::move(options)) {}
Outcome<IdentityDelegationReply>
IdentityExchangeApplication::exchange_delegation(const ExchangeIdentityDelegationRequest& request) {
  try {
    auto valid = valid_request(request, options_);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    auto sampled = clock_.sample();
    if (const auto* error = std::get_if<Failure>(&sampled)) {
      return *error;
    }
    auto clock_valid = valid_clock(std::get<SecurityClockSample>(sampled));
    if (const auto* error = std::get_if<Failure>(&clock_valid)) {
      return *error;
    }
    auto verified = delegations_.verify_exchange_parent(request.parent_proof, request.recipient,
                                                        std::get<SecurityClockSample>(sampled));
    if (const auto* error = std::get_if<Failure>(&verified)) {
      return *error;
    }
    const auto& parent = std::get<VerifiedDelegation>(verified).claims;
    if (std::holds_alternative<Failure>(validate_delegation_shape(parent)) ||
        parent.issuer != options_.issuer || parent.audience != Service::Runtime ||
        parent.business_hops != 1 || parent.actor_chain.size() != 1) {
      return authentication_rejected();
    }
    auto initial = authorize_exchange(transactions_, request, parent);
    if (const auto* error = std::get_if<Failure>(&initial)) {
      return *error;
    }
    const auto& authority = std::get<ExchangeAuthority>(initial);
    auto identifier = detail::random_uuid(random_);
    if (const auto* error = std::get_if<Failure>(&identifier)) {
      return *error;
    }
    auto signing_clock = clock_.sample();
    if (const auto* error = std::get_if<Failure>(&signing_clock)) {
      return *error;
    }
    auto signing_valid = valid_clock(std::get<SecurityClockSample>(signing_clock));
    if (const auto* error = std::get_if<Failure>(&signing_valid)) {
      return *error;
    }
    const auto now =
        std::max(authority.checked_at, std::get<SecurityClockSample>(signing_clock).utc);
    const TimePoint issued_at = std::chrono::floor<std::chrono::seconds>(now);
    auto maximum = identity_deadline(issued_at, kDelegationBodyLifetime);
    if (const auto* error = std::get_if<Failure>(&maximum)) {
      return *error;
    }
    const TimePoint expires_at = std::chrono::floor<std::chrono::seconds>(
        std::min(authority.latest_expiry, std::get<TimePoint>(maximum)));
    if (now >= request.context.deadline) {
      return failure(ErrorCode::Timeout, Recovery::Backoff);
    }
    if (expires_at <= now) {
      return authentication_rejected();
    }
    const auto* runtime = std::get_if<PlatformPeer>(&request.recipient.binding.peer);
    if (!runtime) {
      return authentication_rejected();
    }
    auto actors = parent.actor_chain;
    actors.push_back(runtime->subject);
    DelegationClaims child{parent.issuer,
                           request.audience,
                           parent.subject,
                           runtime->subject,
                           std::move(actors),
                           request.permissions,
                           MessageId{std::get<std::string>(identifier)},
                           request.recipient.binding.certificate.fingerprint,
                           parent.authorization_version,
                           issued_at,
                           issued_at,
                           expires_at,
                           2,
                           parent.source_credential,
                           parent.source_session};
    auto narrowed = validate_delegation_exchange(parent, child, Service::Runtime, runtime->subject);
    if (const auto* error = std::get_if<Failure>(&narrowed)) {
      return *error;
    }
    auto signed_proof = delegations_.sign(child, options_.signing_key);
    if (const auto* error = std::get_if<Failure>(&signed_proof)) {
      return *error;
    }
    const auto& proof = std::get<OwnedSecret>(signed_proof);
    if (!proof || proof->bytes().empty() || proof->bytes().size() > 32768) {
      return failure(ErrorCode::Internal);
    }
    auto final = authorize_exchange(transactions_, request, parent);
    if (const auto* error = std::get_if<Failure>(&final)) {
      return *error;
    }
    const auto& current = std::get<ExchangeAuthority>(final);
    if (!same_authority(authority, current) || current.checked_at >= child.expires_at ||
        child.expires_at > current.latest_expiry) {
      return authentication_rejected();
    }
    return IdentityDelegationReply{std::move(std::get<OwnedSecret>(signed_proof)), std::move(child),
                                   current.checked_at};
  } catch (const std::bad_alloc&) {
    return failure(ErrorCode::UpstreamUnavailable);
  } catch (...) {
    return failure(ErrorCode::Internal);
  }
}
} // namespace kiarabot::core::target
