#include "kiarabot/core/application/target/identity_admin_application.hpp"

#include <exception>
#include <set>
#include <span>
#include <string>
#include <utility>

#include "kiarabot/core/domain/target/identity_state_rules.hpp"

namespace kiarabot::core::target {
namespace {
constexpr std::size_t kMaximumPlatformPeers = 16;
Failure internal_failure() {
  return rejected(ErrorCode::Internal, Recovery::Diagnose);
}
// Only the target platform services that must call Identity as an mTLS consumer/mutator peer.
// Control keeps its dedicated enrollment shape and Identity is never a client of itself here.
bool service_peer(Service service) {
  return service == Service::Runtime || service == Service::Scheduler ||
         service == Service::Model || service == Service::Host;
}
Status check_deadline(TimePoint now, TimePoint deadline) {
  if (now >= deadline) {
    return rejected(ErrorCode::Timeout, Recovery::Backoff);
  }
  return std::monostate{};
}
Outcome<RegisteredPeer> read_peer(IdentitySecurityUnitOfWork& work,
                                  const PeerRegistrationRequest& request, TimePoint now) {
  const auto deadline = check_deadline(now, request.deadline);
  if (const auto* failure = std::get_if<Failure>(&deadline)) {
    return *failure;
  }
  auto found = work.credentials().by_certificate(request.leaf_fingerprint);
  if (const auto* failure = std::get_if<Failure>(&found)) {
    return *failure;
  }
  const auto& row = std::get<std::optional<CredentialVersionRecord>>(found);
  if (!row) {
    return authentication_rejected();
  }
  const auto* certificate = std::get_if<CertificateBinding>(&row->material);
  if (!certificate || certificate->fingerprint != request.leaf_fingerprint) {
    return internal_failure();
  }
  auto subject = work.principals().find(row->subject);
  if (const auto* failure = std::get_if<Failure>(&subject)) {
    return *failure;
  }
  const auto& principal = std::get<std::optional<Principal>>(subject);
  if (!principal) {
    return authentication_rejected();
  }
  auto current = work.credentials().current(row->ref.id);
  if (const auto* failure = std::get_if<Failure>(&current)) {
    return *failure;
  }
  const auto& head = std::get<std::optional<CredentialVersionRecord>>(current);
  if (!head || head->ref != row->ref) {
    return authentication_rejected();
  }
  const auto* live_certificate = std::get_if<CertificateBinding>(&head->material);
  if (!live_certificate || live_certificate->fingerprint != certificate->fingerprint ||
      live_certificate->registered_san != certificate->registered_san ||
      live_certificate->platform_service != certificate->platform_service ||
      head->subject != row->subject) {
    return internal_failure();
  }
  const auto valid = require_current_credential(*principal, *head, row->ref, now);
  if (const auto* failure = std::get_if<Failure>(&valid)) {
    return *failure;
  }
  if (principal->kind == PrincipalKind::PlatformService && certificate->platform_service) {
    const bool host = *certificate->platform_service == Service::Host;
    if ((host && (!principal->binding || principal->binding->kind != ResourceKind::Host ||
                  principal->binding->id.value.empty() || head->binding != *principal->binding)) ||
        (!host && head->binding != ResourceRef{ResourceKind::Principal, {principal->id.value}})) {
      return internal_failure();
    }
    return RegisteredPeer{PlatformPeer{*certificate->platform_service, principal->id},
                          *live_certificate, head->ref};
  }
  if (certificate->platform_service || !principal->binding ||
      head->binding != *principal->binding) {
    return authentication_rejected();
  }
  if (principal->kind == PrincipalKind::Adapter &&
      principal->binding->kind == ResourceKind::Adapter) {
    return RegisteredPeer{AdapterPeer{principal->binding->id, principal->id}, *live_certificate,
                          head->ref};
  }
  if (principal->kind == PrincipalKind::Plugin &&
      principal->binding->kind == ResourceKind::Instance) {
    return RegisteredPeer{PluginPeer{InstanceId{principal->binding->id.value}, principal->id},
                          *live_certificate, head->ref};
  }
  return authentication_rejected();
}
bool same_peer(const RegisteredPeer& left, const RegisteredPeer& right) {
  // These fields are authoritative immutable binding metadata, not secret comparisons.
  if (left.credential != right.credential || left.peer.index() != right.peer.index() ||
      left.certificate.subject != right.certificate.subject ||
      left.certificate.fingerprint != right.certificate.fingerprint ||
      left.certificate.registered_san != right.certificate.registered_san ||
      left.certificate.platform_service != right.certificate.platform_service ||
      left.certificate.not_before != right.certificate.not_before ||
      left.certificate.not_after != right.certificate.not_after) {
    return false;
  }
  if (const auto* platform = std::get_if<PlatformPeer>(&left.peer)) {
    const auto& other = std::get<PlatformPeer>(right.peer);
    return platform->service == other.service && platform->subject == other.subject;
  }
  if (const auto* adapter = std::get_if<AdapterPeer>(&left.peer)) {
    const auto& other = std::get<AdapterPeer>(right.peer);
    return adapter->adapter == other.adapter && adapter->subject == other.subject;
  }
  const auto& plugin = std::get<PluginPeer>(left.peer);
  const auto& other = std::get<PluginPeer>(right.peer);
  return plugin.instance == other.instance && plugin.subject == other.subject;
}
Status save_assignments(IdentitySecurityUnitOfWork& work, const SubjectId& subject,
                        const std::vector<ExactAssignment>& assignments) {
  const auto valid = validate_assignment_collection(assignments, subject);
  if (const auto* failure = std::get_if<Failure>(&valid)) {
    return *failure;
  }
  for (const auto& assignment : assignments) {
    if (assignment.version.value != 1) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    const auto saved = work.exact_assignments().save(assignment, std::nullopt);
    if (const auto* failure = std::get_if<Failure>(&saved)) {
      return *failure;
    }
  }
  return std::monostate{};
}
} // namespace

IdentityAdminApplication::IdentityAdminApplication(IdentitySecurityUnitOfWorkFactory& transactions,
                                                   PeerCertificatePort& certificates)
    : transactions_(transactions), certificates_(certificates) {}

Outcome<RegisteredPeer>
IdentityAdminApplication::registered_peer(const PeerRegistrationRequest& request) {
  try {
    auto begun = transactions_.begin();
    if (const auto* failure = std::get_if<Failure>(&begun)) {
      return *failure;
    }
    auto work = std::move(std::get<std::unique_ptr<IdentitySecurityUnitOfWork>>(begun));
    if (!work) {
      return internal_failure();
    }
    const auto now = work->current_time();
    if (const auto* failure = std::get_if<Failure>(&now)) {
      return *failure;
    }
    return read_peer(*work, request, std::get<TimePoint>(now));
  } catch (const std::exception&) {
    return internal_failure();
  } catch (...) {
    return internal_failure();
  }
}
Outcome<VerifiedPeer>
IdentityAdminApplication::authenticate_peer(const PeerRegistrationRequest& request,
                                            std::span<const std::byte> presented_chain) {
  try {
    auto before = registered_peer(request);
    if (const auto* failure = std::get_if<Failure>(&before)) {
      return *failure;
    }
    // The read UoW has been destroyed before key/certificate adapter code executes.
    auto begun = transactions_.begin();
    if (const auto* failure = std::get_if<Failure>(&begun)) {
      return *failure;
    }
    auto work = std::move(std::get<std::unique_ptr<IdentitySecurityUnitOfWork>>(begun));
    if (!work) {
      return internal_failure();
    }
    const auto now = work->current_time();
    if (const auto* failure = std::get_if<Failure>(&now)) {
      return *failure;
    }
    work.reset();
    auto verified = certificates_.verify(presented_chain, std::get<RegisteredPeer>(before),
                                         std::get<TimePoint>(now));
    if (const auto* failure = std::get_if<Failure>(&verified)) {
      return *failure;
    }
    if (!same_peer(std::get<VerifiedPeer>(verified).binding, std::get<RegisteredPeer>(before))) {
      return internal_failure();
    }
    auto after = registered_peer(request);
    if (const auto* failure = std::get_if<Failure>(&after)) {
      return *failure;
    }
    if (!same_peer(std::get<RegisteredPeer>(before), std::get<RegisteredPeer>(after))) {
      return authentication_rejected();
    }
    return std::move(std::get<VerifiedPeer>(verified));
  } catch (...) {
    return internal_failure();
  }
}

Outcome<IdentityBootstrapReply>
IdentityAdminApplication::bootstrap_offline(const OfflineIdentityBootstrap& request) {
  try {
    if (request.administrator.value.empty() || request.control.value.empty() ||
        request.administrator == request.control || request.password_credential.value.empty() ||
        request.control_credential.value.empty() ||
        request.password_credential == request.control_credential ||
        request.normalized_login.empty() || request.context.request_id.empty() ||
        request.context.trace_id.empty() || request.password.encoded.empty() ||
        request.password.hash_policy_version.value == 0 ||
        request.control_certificate.subject != request.control ||
        request.control_certificate.platform_service != Service::Control ||
        request.administrator_assignments.empty() || request.control_assignments.empty()) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    for (const auto& assignment : request.control_assignments) {
      if (!assignment.roles.empty()) {
        return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
    }
    for (const auto& admin : request.administrator_assignments) {
      for (const auto& control : request.control_assignments) {
        if (admin.id == control.id) {
          return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
        }
      }
    }
    // Administrator enrollment must actually include an explicit scoped Admin role. No implicit
    // all-resources role or invented catalog identifier is synthesized by this offline command.
    bool administrator_role = false;
    for (const auto& assignment : request.administrator_assignments) {
      for (const auto& role : assignment.roles) {
        administrator_role = administrator_role || role.role == FixedRole::Admin;
      }
    }
    if (!administrator_role) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    // Platform service peers are optional, but a supplied enrollment must be a genuine registered
    // service identity with its own certificate binding and explicit scoped assignments. Roles are
    // human-only; never synthesize a capability or reuse the administrator/Control identities.
    if (request.platform_peers.size() > kMaximumPlatformPeers) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    std::set<std::string> subject_ids{request.administrator.value, request.control.value};
    std::set<std::string> credential_ids{request.password_credential.value,
                                         request.control_credential.value};
    std::set<std::string> assignment_ids;
    std::set<std::string> host_ids;
    for (const auto& assignment : request.administrator_assignments) {
      assignment_ids.insert(assignment.id.value);
    }
    for (const auto& assignment : request.control_assignments) {
      assignment_ids.insert(assignment.id.value);
    }
    for (const auto& peer : request.platform_peers) {
      if ((peer.service == Service::Host) != peer.host.has_value() ||
          (peer.host && (peer.host->value.empty() || peer.host->value.size() > 128 ||
                         !host_ids.insert(peer.host->value).second))) {
        return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
      if (peer.principal.value.empty() || peer.credential.value.empty() ||
          !service_peer(peer.service) || peer.certificate.subject != peer.principal ||
          peer.certificate.platform_service != peer.service ||
          peer.certificate.not_after <= peer.certificate.not_before || peer.assignments.empty() ||
          peer.certificate.fingerprint == Sha256Digest{}) {
        return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
      if (!subject_ids.insert(peer.principal.value).second ||
          !credential_ids.insert(peer.credential.value).second) {
        return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
      if (std::holds_alternative<Failure>(
              validate_assignment_collection(peer.assignments, peer.principal))) {
        return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
      for (const auto& assignment : peer.assignments) {
        if (!assignment.roles.empty() || !assignment_ids.insert(assignment.id.value).second) {
          return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
        }
      }
    }
    auto begun = transactions_.begin();
    if (const auto* failure = std::get_if<Failure>(&begun)) {
      return *failure;
    }
    auto work = std::move(std::get<std::unique_ptr<IdentitySecurityUnitOfWork>>(begun));
    if (!work) {
      return internal_failure();
    }
    const auto sampled = work->current_time();
    if (const auto* failure = std::get_if<Failure>(&sampled)) {
      return *failure;
    }
    const auto now = std::get<TimePoint>(sampled);
    const auto deadline = check_deadline(now, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&deadline)) {
      return *failure;
    }
    auto marker = work->bootstrap().state();
    if (const auto* failure = std::get_if<Failure>(&marker)) {
      return *failure;
    }
    auto consumed =
        consume_administrator_bootstrap(std::get<AdministratorBootstrap>(marker),
                                        request.expected_marker, request.administrator, now);
    if (const auto* failure = std::get_if<Failure>(&consumed)) {
      return *failure;
    }
    const ResourceRef admin_resource{ResourceKind::Principal,
                                     ResourceId{request.administrator.value}};
    const ResourceRef control_resource{ResourceKind::Principal, ResourceId{request.control.value}};
    const Principal administrator{
        request.administrator, PrincipalKind::Human, admin_resource, {1}, LifecycleState::Active};
    const Principal control{request.control,
                            PrincipalKind::PlatformService,
                            control_resource,
                            {1},
                            LifecycleState::Active};
    const CredentialVersionRecord password{{request.password_credential, {1}},
                                           request.administrator,
                                           request.password,
                                           CredentialState::Active,
                                           now,
                                           std::nullopt,
                                           admin_resource,
                                           request.password_expires_at};
    const CredentialVersionRecord certificate{{request.control_credential, {1}},
                                              request.control,
                                              request.control_certificate,
                                              CredentialState::Active,
                                              now,
                                              std::nullopt,
                                              control_resource,
                                              request.control_certificate.not_after};
    for (const auto* record : {&password, &certificate}) {
      const auto valid = validate_credential_record(*record);
      if (const auto* failure = std::get_if<Failure>(&valid)) {
        return *failure;
      }
    }
    if (request.control_certificate.not_before > now) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    for (const auto& peer : request.platform_peers) {
      if (peer.certificate.not_before > now || peer.certificate.not_after <= now) {
        return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
    }
    // Lambdas execute sequentially: stop at the first failing port, never issue further writes.
    const auto apply = [&](const auto& action) -> std::optional<Failure> {
      const auto result = action();
      if (const auto* failure = std::get_if<Failure>(&result)) {
        return *failure;
      }
      return std::nullopt;
    };
    if (auto failure =
            apply([&] { return work->principals().save(administrator, std::nullopt); })) {
      return *failure;
    }
    if (auto failure = apply([&] { return work->principals().save(control, std::nullopt); })) {
      return *failure;
    }
    if (auto failure = apply([&] { return work->credentials().save(password, std::nullopt); })) {
      return *failure;
    }
    if (auto failure = apply([&] { return work->credentials().save(certificate, std::nullopt); })) {
      return *failure;
    }
    if (auto failure = apply([&] {
          return work->credentials().save_login(LoginIdentity{request.normalized_login,
                                                              request.administrator,
                                                              request.password_credential,
                                                              {1}},
                                                std::nullopt);
        })) {
      return *failure;
    }
    if (auto failure = apply([&] {
          return save_assignments(*work, request.administrator, request.administrator_assignments);
        })) {
      return *failure;
    }
    if (auto failure = apply([&] {
          return save_assignments(*work, request.control, request.control_assignments);
        })) {
      return *failure;
    }
    for (const auto& subject : {request.administrator, request.control}) {
      if (auto failure = apply([&] {
            return work->authorizations().save(AuthorizationState{subject, {1}, true},
                                               std::nullopt);
          })) {
        return *failure;
      }
    }
    // Optional platform service peers share the same once-only transaction: each gets an
    // explicit self binding, certificate credential, authorization watermark and offline audit.
    for (const auto& peer : request.platform_peers) {
      const ResourceRef peer_resource{ResourceKind::Principal, ResourceId{peer.principal.value}};
      const ResourceRef binding =
          peer.host ? ResourceRef{ResourceKind::Host, *peer.host} : peer_resource;
      const Principal platform_principal{
          peer.principal, PrincipalKind::PlatformService, binding, {1}, LifecycleState::Active};
      const CredentialVersionRecord certificate{{peer.credential, {1}},
                                                peer.principal,
                                                peer.certificate,
                                                CredentialState::Active,
                                                now,
                                                std::nullopt,
                                                binding,
                                                peer.certificate.not_after};
      const auto valid = validate_credential_record(certificate);
      if (const auto* failure = std::get_if<Failure>(&valid)) {
        return *failure;
      }
      if (auto failure =
              apply([&] { return work->principals().save(platform_principal, std::nullopt); })) {
        return *failure;
      }
      if (auto failure =
              apply([&] { return work->credentials().save(certificate, std::nullopt); })) {
        return *failure;
      }
      if (auto failure =
              apply([&] { return save_assignments(*work, peer.principal, peer.assignments); })) {
        return *failure;
      }
      if (auto failure = apply([&] {
            return work->authorizations().save(AuthorizationState{peer.principal, {1}, true},
                                               std::nullopt);
          })) {
        return *failure;
      }
      if (auto failure = apply([&] {
            return work->journal().append_audit(AuditEntry{request.administrator,
                                                           peer_resource,
                                                           request.context.request_id,
                                                           request.context.trace_id,
                                                           "BootstrapPlatformPeer",
                                                           std::nullopt,
                                                           {1},
                                                           now});
          })) {
        return *failure;
      }
    }
    if (auto failure = apply([&] {
          return work->journal().append_audit(AuditEntry{request.administrator,
                                                         admin_resource,
                                                         request.context.request_id,
                                                         request.context.trace_id,
                                                         "BootstrapAdministrator",
                                                         std::nullopt,
                                                         {1},
                                                         now});
        })) {
      return *failure;
    }
    if (auto failure = apply([&] {
          return work->bootstrap().consume(std::get<AdministratorBootstrap>(consumed),
                                           request.expected_marker);
        })) {
      return *failure;
    }
    switch (work->commit()) {
    case CommitResult::Committed:
      return IdentityBootstrapReply{std::get<AdministratorBootstrap>(consumed), now};
    case CommitResult::RolledBack:
      return rejected(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
    case CommitResult::Unknown:
      return Failure{ErrorCode::UpstreamUnavailable, Acceptance::Unknown,
                     Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
    }
    return internal_failure();
  } catch (...) {
    return internal_failure();
  }
}

} // namespace kiarabot::core::target
