#include "kiarabot/core/application/target/identity_principal_application.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "identity_security_application_detail.hpp"
#include "kiarabot/core/application/target/identity_invalidation_application.hpp"
#include "kiarabot/core/domain/target/identity_credential_rules.hpp"
#include "kiarabot/core/domain/target/identity_state_rules.hpp"

namespace kiarabot::core::target {
namespace {
namespace detail = identity_security_detail;

Failure invalid() {
  return detail::failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
}
Failure internal() {
  return detail::failure(ErrorCode::Internal);
}
// Resource-scoped management/read operations hide existence: a missing target and a denied
// target both return NOT_FOUND. Authentication failures stay UNAUTHENTICATED.
Failure hidden_not_found() {
  return detail::failure(ErrorCode::NotFound, Recovery::ReadCurrent);
}
Failure scoped_authorization(const Failure& failure) {
  return failure.code == ErrorCode::PermissionDenied ? hidden_not_found() : failure;
}
std::string delivery_id(const RequestCommitment& commitment) {
  constexpr std::array<char, 16> kDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                         '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result = "delivery-";
  result.reserve(result.size() + commitment.tag.bytes.size() * 2);
  for (const auto byte : commitment.tag.bytes) {
    const auto value = std::to_integer<unsigned int>(byte);
    result.push_back(kDigits[value >> 4U]);
    result.push_back(kDigits[value & 0x0fU]);
  }
  return result;
}
ResourceRef principal_resource(const Principal& principal) {
  return principal.binding.value_or(
      ResourceRef{ResourceKind::Principal, ResourceId{principal.id.value}});
}
bool bounded(const std::string& value) {
  return detail::bounded_text(value);
}
Status validate_options(const IdentityPrincipalOptions& options) {
  if (!bounded(options.token_key.value) || !bounded(options.request_commitment_key.value) ||
      !bounded(options.receipt_encryption_key.value) ||
      options.bootstrap_credential_lifetime <= std::chrono::nanoseconds::zero() ||
      options.password_credential_lifetime <= std::chrono::nanoseconds::zero() ||
      options.secret_receipts.max_records == 0 || options.maximum_allowed_services == 0 ||
      options.maximum_allowed_services > 16) {
    return invalid();
  }
  return std::monostate{};
}
// Public registration only creates non-human workload principals; offline bootstrap owns humans.
bool register_shape_valid(const RegisterPrincipalCommand& command,
                          std::size_t maximum_allowed_services) {
  return known_principal_kind(command.kind) && command.kind != PrincipalKind::Human &&
         valid_principal_binding(command.kind, command.binding) &&
         !command.allowed_services.empty() &&
         command.allowed_services.size() <= maximum_allowed_services &&
         valid_pending_audiences(CredentialKind::WorkloadBootstrap, command.allowed_services) &&
         bounded(command.context.request_id) && bounded(command.context.trace_id) &&
         bounded(command.context.idempotency_key) && command.context.deadline != TimePoint{};
}
Outcome<SubjectId> registration_subject(const RegisterPrincipalCommand& command,
                                        RandomPort& random) {
  if (command.kind == PrincipalKind::PlatformService) {
    return SubjectId{command.binding.id.value};
  }
  auto generated = detail::random_uuid(random);
  if (const auto* error = std::get_if<Failure>(&generated)) {
    return *error;
  }
  return SubjectId{std::get<std::string>(generated)};
}
Outcome<CredentialId> registration_credential(RandomPort& random) {
  auto generated = detail::random_uuid(random);
  if (const auto* error = std::get_if<Failure>(&generated)) {
    return *error;
  }
  return CredentialId{std::get<std::string>(generated)};
}
// Re-reads current registration of the authenticated platform peer and re-checks TLS evidence.
Outcome<detail::PeerAuthority> current_actor(IdentitySecurityUnitOfWork& work,
                                             const VerifiedPeer& peer, TimePoint now) {
  auto read = detail::read_peer(work, peer);
  if (const auto* error = std::get_if<Failure>(&read)) {
    return *error;
  }
  auto& authority = std::get<detail::PeerAuthority>(read);
  auto gated = detail::check_peer(authority, peer, now, false);
  if (const auto* error = std::get_if<Failure>(&gated)) {
    return *error;
  }
  return authority;
}
Outcome<Principal> current_principal(IdentitySecurityUnitOfWork& work, const SubjectId& subject) {
  auto found = work.principals().find(subject);
  if (const auto* error = std::get_if<Failure>(&found)) {
    return *error;
  }
  const auto& principal = std::get<std::optional<Principal>>(found);
  if (!principal) {
    return detail::failure(ErrorCode::Internal);
  }
  return *principal;
}
Status authorize(const detail::PeerAuthority& authority, Permission permission,
                 const ResourceRef& resource) {
  return authorize_exact(authority.subject.principal, permission, resource,
                         authority.subject.assignments);
}
TimePoint peer_valid_until(const detail::PeerAuthority& authority) {
  const auto expires = authority.credential.expires_at.value_or(TimePoint::min());
  const auto* certificate = std::get_if<CertificateBinding>(&authority.credential.material);
  if (!certificate) {
    return expires;
  }
  return std::min(expires, certificate->not_after);
}
// Kind-level rotation capability. Only workload bootstrap tokens and human password verifiers
// can be re-issued from trusted server-generated secrets; certificate binding enrollment owns
// its own flow and stays a stable CapabilityNotSupported for this RPC.
enum class RotationKind { WorkloadBootstrap, Password };
Outcome<RotationKind> rotation_kind(const CredentialVersionRecord& credential) {
  if (const auto* pending = std::get_if<PendingCredentialMaterial>(&credential.material)) {
    if (credential.state != CredentialState::Pending) {
      return detail::failure(ErrorCode::Internal);
    }
    if (pending->kind == CredentialKind::WorkloadBootstrap) {
      return RotationKind::WorkloadBootstrap;
    }
    if (pending->kind == CredentialKind::Password) {
      return RotationKind::Password;
    }
    return detail::failure(ErrorCode::CapabilityNotSupported);
  }
  if (const auto* bootstrap = std::get_if<WorkloadBootstrapVerifier>(&credential.material)) {
    if (credential.state != CredentialState::Active || bootstrap->audiences.empty()) {
      return detail::failure(ErrorCode::CapabilityNotSupported);
    }
    return RotationKind::WorkloadBootstrap;
  }
  if (std::holds_alternative<PasswordVerifier>(credential.material)) {
    if (credential.state != CredentialState::Active) {
      return detail::failure(ErrorCode::CapabilityNotSupported);
    }
    return RotationKind::Password;
  }
  return detail::failure(ErrorCode::CapabilityNotSupported);
}
Outcome<std::vector<Service>> bootstrap_audiences(const CredentialVersionRecord& credential) {
  if (const auto* pending = std::get_if<PendingCredentialMaterial>(&credential.material)) {
    if (pending->kind != CredentialKind::WorkloadBootstrap || pending->audiences.empty()) {
      return detail::failure(ErrorCode::CapabilityNotSupported);
    }
    return pending->audiences;
  }
  if (const auto* bootstrap = std::get_if<WorkloadBootstrapVerifier>(&credential.material)) {
    if (bootstrap->audiences.empty()) {
      return detail::failure(ErrorCode::CapabilityNotSupported);
    }
    return bootstrap->audiences;
  }
  return detail::failure(ErrorCode::CapabilityNotSupported);
}
Outcome<EncryptedSecretReceipt>
seal_delivery(const IdentityPrincipalDependencies& dependencies,
              const IdentityPrincipalOptions& options, const CredentialDeliveryScope& scope,
              const CredentialDeliveryParameters& parameters, const SubjectId& subject,
              const CredentialVersionRecord& issued, TimePoint now, TimePoint retain_until,
              const SecretReplayPayload& payload) {
  auto commitment = dependencies.tokens.commit_request(
      scope, parameters, std::get<EphemeralCredentialDelivery>(payload).secret->bytes(),
      options.request_commitment_key);
  if (const auto* error = std::get_if<Failure>(&commitment)) {
    return *error;
  }
  const SecretReceiptBinding binding{scope,
                                     subject,
                                     std::nullopt,
                                     issued.ref,
                                     parameters,
                                     0,
                                     std::get<RequestCommitment>(commitment),
                                     now,
                                     retain_until};
  auto sealed = dependencies.receipts.seal(payload, binding, options.receipt_encryption_key);
  if (const auto* error = std::get_if<Failure>(&sealed)) {
    return *error;
  }
  return EncryptedSecretReceipt{binding, std::move(std::get<SealedSecret>(sealed))};
}
class Application final : public IdentityPrincipalApplication {
public:
  Application(IdentityPrincipalDependencies dependencies, IdentityPrincipalOptions options)
      : dependencies_(dependencies), options_(std::move(options)) {}
  Outcome<RegisteredPrincipalSnapshot>
  register_principal(const RegisterPrincipalCommand& command) override {
    try {
      if (!register_shape_valid(command, options_.maximum_allowed_services)) {
        return invalid();
      }
      auto fingerprint = dependencies_.fingerprints.register_principal(
          command.kind, command.binding, command.allowed_services);
      if (const auto* error = std::get_if<Failure>(&fingerprint)) {
        return *error;
      }
      auto subject = registration_subject(command, dependencies_.random);
      if (const auto* error = std::get_if<Failure>(&subject)) {
        return *error;
      }
      auto credential_id = registration_credential(dependencies_.random);
      if (const auto* error = std::get_if<Failure>(&credential_id)) {
        return *error;
      }
      auto opened = detail::begin(dependencies_.transactions, command.context);
      if (const auto* error = std::get_if<Failure>(&opened)) {
        return *error;
      }
      auto& work = std::get<detail::Work>(opened);
      auto actor = current_actor(*work.transaction, command.actor, work.now);
      if (const auto* error = std::get_if<Failure>(&actor)) {
        return *error;
      }
      const auto& authority = std::get<detail::PeerAuthority>(actor);
      auto authorized = authorize(authority, Permission::IdentityPrincipalManage, command.binding);
      if (const auto* error = std::get_if<Failure>(&authorized)) {
        return *error;
      }
      const ReceiptKey key{authority.subject.principal.id, "RegisterPrincipal", command.binding,
                           command.context.idempotency_key};
      auto original = work.transaction->receipts().find(key);
      if (const auto* error = std::get_if<Failure>(&original)) {
        return *error;
      }
      if (const auto& receipt = std::get<std::optional<CommandReceipt>>(original)) {
        const auto* snapshot = std::get_if<RegisteredPrincipalSnapshot>(&receipt->result);
        if (receipt->key != key || !snapshot) {
          return internal();
        }
        if (receipt->fingerprint != std::get<Sha256Digest>(fingerprint)) {
          return detail::failure(ErrorCode::Conflict, Recovery::QuerySameIdentity);
        }
        if (snapshot->principal.version.value != 1 || snapshot->principal.kind != command.kind ||
            snapshot->principal.binding != command.binding ||
            snapshot->initial_credential.state != CredentialStatusState::Pending ||
            snapshot->initial_credential.expires_at) {
          return internal();
        }
        auto checkpoint =
            detail::final_check(work, command.context, peer_valid_until(authority), true);
        if (const auto* error = std::get_if<Failure>(&checkpoint)) {
          return *error;
        }
        work.transaction->rollback();
        return *snapshot;
      }
      const Principal principal{std::get<SubjectId>(subject),
                                command.kind,
                                command.binding,
                                {1},
                                LifecycleState::Disabled};
      if (!bounded(principal.id.value) || !principal.binding ||
          !valid_resource_ref(*principal.binding)) {
        return invalid();
      }
      auto pending = make_pending_credential(std::get<CredentialId>(credential_id), principal.id,
                                             CredentialKind::WorkloadBootstrap, command.binding,
                                             work.now, command.allowed_services);
      if (const auto* error = std::get_if<Failure>(&pending)) {
        return *error;
      }
      const auto& credential = std::get<CredentialVersionRecord>(pending);
      auto status = credential_status(credential, work.now);
      if (std::holds_alternative<Failure>(status)) {
        return internal();
      }
      const RegisteredPrincipalSnapshot snapshot{principal,
                                                 std::get<CredentialStatusSnapshot>(status)};
      auto retention = detail::add_lifetime(work.now, kMinimumReceiptRetention);
      if (const auto* error = std::get_if<Failure>(&retention)) {
        return *error;
      }
      auto saved_principal = work.transaction->principals().save(principal, std::nullopt);
      if (const auto* error = std::get_if<Failure>(&saved_principal)) {
        // The Identity-private v1 uniqueness rule is one Principal per binding, independent of
        // idempotency keys. A duplicate binding save is reported as a stable conflict.
        if (error->code == ErrorCode::Conflict) {
          return detail::failure(ErrorCode::Conflict, Recovery::QuerySameIdentity);
        }
        return *error;
      }
      auto saved_credential = work.transaction->credentials().save(credential, std::nullopt);
      if (const auto* error = std::get_if<Failure>(&saved_credential)) {
        return *error;
      }
      auto receipt = work.transaction->receipts().insert(
          CommandReceipt{key, std::get<Sha256Digest>(fingerprint), snapshot, work.now,
                         std::get<TimePoint>(retention)},
          ReceiptCapacity{});
      if (const auto* error = std::get_if<Failure>(&receipt)) {
        return *error;
      }
      auto audit = work.transaction->journal().append_audit(
          AuditEntry{authority.subject.principal.id,
                     {ResourceKind::Principal, ResourceId{principal.id.value}},
                     command.context.request_id,
                     command.context.trace_id,
                     "RegisterPrincipal",
                     {},
                     authority.subject.authorization.version,
                     work.now});
      if (const auto* error = std::get_if<Failure>(&audit)) {
        return *error;
      }
      auto checkpoint =
          detail::final_check(work, command.context, peer_valid_until(authority), true);
      if (const auto* error = std::get_if<Failure>(&checkpoint)) {
        return *error;
      }
      auto committed = detail::committed(*work.transaction);
      if (const auto* error = std::get_if<Failure>(&committed)) {
        return *error;
      }
      return snapshot;
    } catch (...) {
      return internal();
    }
  }
  Outcome<CredentialDeliveryReply>
  rotate_credential(const RotateCredentialCommand& command) override {
    try {
      if (!bounded(command.credential.value) || command.expected_version.value == 0 ||
          !bounded(command.context.idempotency_key) || !bounded(command.context.request_id) ||
          !bounded(command.context.trace_id) || command.context.deadline == TimePoint{}) {
        return invalid();
      }
      struct Read {
        TimePoint now;
        CredentialVersionRecord credential;
        detail::PeerAuthority authority;
        std::optional<EncryptedSecretReceipt> receipt;
      };
      const auto read_phase = [&]() -> Outcome<Read> {
        auto opened = detail::begin(dependencies_.transactions, command.context);
        if (const auto* error = std::get_if<Failure>(&opened)) {
          return *error;
        }
        auto& work = std::get<detail::Work>(opened);
        auto found = work.transaction->credentials().current(command.credential);
        if (const auto* error = std::get_if<Failure>(&found)) {
          return *error;
        }
        const auto& credential = std::get<std::optional<CredentialVersionRecord>>(found);
        if (!credential) {
          return detail::failure(ErrorCode::NotFound, Recovery::ReadCurrent);
        }
        auto actor = current_actor(*work.transaction, command.actor, work.now);
        if (const auto* error = std::get_if<Failure>(&actor)) {
          return *error;
        }
        const auto& authority = std::get<detail::PeerAuthority>(actor);
        auto authorized =
            authorize(authority, Permission::IdentityPrincipalManage, credential->binding);
        if (const auto* error = std::get_if<Failure>(&authorized)) {
          return scoped_authorization(*error);
        }
        auto principal = current_principal(*work.transaction, credential->subject);
        if (const auto* error = std::get_if<Failure>(&principal)) {
          return *error;
        }
        const auto& principal_value = std::get<Principal>(principal);
        const auto owner = detail::credential_owner_binding(principal_value, *credential);
        if (const auto* error = std::get_if<Failure>(&owner)) {
          return *error;
        }
        if (principal_value.state != LifecycleState::Active &&
            principal_value.state != LifecycleState::Disabled) {
          return detail::failure(ErrorCode::Conflict, Recovery::ReadCurrent);
        }
        const CredentialDeliveryScope scope{authority.subject.principal.id, command.credential,
                                            command.context.idempotency_key};
        auto receipt = work.transaction->secret_receipts().find(scope);
        if (const auto* error = std::get_if<Failure>(&receipt)) {
          return *error;
        }
        return Read{work.now, *credential, authority,
                    std::get<std::optional<EncryptedSecretReceipt>>(receipt)};
      };
      auto first = read_phase();
      if (const auto* error = std::get_if<Failure>(&first)) {
        return *error;
      }
      const auto& initial = std::get<Read>(first);
      const CredentialDeliveryScope scope{initial.authority.subject.principal.id,
                                          command.credential, command.context.idempotency_key};
      if (initial.receipt) {
        if (initial.credential.ref.version.value != command.expected_version.value + 1) {
          return detail::failure(ErrorCode::Conflict, Recovery::ReadCurrent);
        }
        const auto parameters = CredentialDeliveryParameters{command.expected_version};
        if (std::holds_alternative<Failure>(validate_secret_receipt(*initial.receipt))) {
          return internal();
        }
        auto unsealed =
            dependencies_.receipts.open(initial.receipt->sealed, initial.receipt->binding);
        if (std::holds_alternative<Failure>(unsealed)) {
          return internal();
        }
        auto& payload = std::get<SecretReplayPayload>(unsealed);
        auto* delivery = std::get_if<EphemeralCredentialDelivery>(&payload);
        if (!delivery || !delivery->secret || delivery->secret->bytes().empty() ||
            delivery->credential != initial.receipt->binding.credential ||
            delivery->expires_at <= initial.now) {
          return internal();
        }
        auto commitment = dependencies_.tokens.commit_request(
            scope, parameters, delivery->secret->bytes(), options_.request_commitment_key);
        if (const auto* error = std::get_if<Failure>(&commitment)) {
          return *error;
        }
        auto opened = detail::begin(dependencies_.transactions, command.context);
        if (const auto* error = std::get_if<Failure>(&opened)) {
          return *error;
        }
        auto& work = std::get<detail::Work>(opened);
        auto actor = current_actor(*work.transaction, command.actor, work.now);
        if (const auto* error = std::get_if<Failure>(&actor)) {
          return *error;
        }
        const auto& authority = std::get<detail::PeerAuthority>(actor);
        auto current = work.transaction->credentials().current(command.credential);
        if (const auto* error = std::get_if<Failure>(&current)) {
          return *error;
        }
        const auto& credential = std::get<std::optional<CredentialVersionRecord>>(current);
        if (!credential || credential->ref != initial.credential.ref) {
          return detail::failure(ErrorCode::Conflict, Recovery::ReadCurrent);
        }
        auto authorized =
            authorize(authority, Permission::IdentityPrincipalManage, credential->binding);
        if (const auto* error = std::get_if<Failure>(&authorized)) {
          return scoped_authorization(*error);
        }
        auto principal = current_principal(*work.transaction, credential->subject);
        if (const auto* error = std::get_if<Failure>(&principal)) {
          return *error;
        }
        auto replay = require_secret_replay(
            *initial.receipt, scope, parameters, std::get<RequestCommitment>(commitment),
            std::get<Principal>(principal), *credential, nullptr, work.now);
        if (const auto* error = std::get_if<Failure>(&replay)) {
          return *error;
        }
        auto checkpoint = detail::final_check(
            work, command.context,
            std::min({peer_valid_until(authority), initial.receipt->binding.retain_until,
                      delivery->expires_at}),
            true);
        if (const auto* error = std::get_if<Failure>(&checkpoint)) {
          return *error;
        }
        work.transaction->rollback();
        return CredentialDeliveryReply{delivery->credential, std::move(delivery->secret),
                                       delivery_id(std::get<RequestCommitment>(commitment)),
                                       delivery->expires_at, work.now};
      }
      if (initial.credential.ref.version != command.expected_version) {
        return detail::failure(ErrorCode::Conflict, Recovery::ReadCurrent);
      }
      if (initial.credential.state == CredentialState::Revoked) {
        return detail::failure(ErrorCode::Conflict, Recovery::ReadCurrent);
      }
      auto kind = rotation_kind(initial.credential);
      if (const auto* error = std::get_if<Failure>(&kind)) {
        return *error;
      }
      auto version = detail::next_version(initial.credential.ref.version);
      if (const auto* error = std::get_if<Failure>(&version)) {
        return *error;
      }
      const CredentialVersionRef issued_ref{initial.credential.ref.id,
                                            std::get<ResourceVersion>(version)};
      auto issued = dependencies_.random.opaque_token();
      if (const auto* error = std::get_if<Failure>(&issued)) {
        return *error;
      }
      auto secret = std::move(std::get<OwnedSecret>(issued));
      if (!secret || secret->bytes().empty()) {
        return internal();
      }
      TimePoint expires;
      CredentialMaterial material = PendingCredentialMaterial{};
      if (std::get<RotationKind>(kind) == RotationKind::WorkloadBootstrap) {
        auto audiences = bootstrap_audiences(initial.credential);
        if (const auto* error = std::get_if<Failure>(&audiences)) {
          return *error;
        }
        auto lifetime = detail::add_lifetime(initial.now, options_.bootstrap_credential_lifetime);
        if (const auto* error = std::get_if<Failure>(&lifetime)) {
          return *error;
        }
        expires = std::get<TimePoint>(lifetime);
        auto verifier = dependencies_.tokens.make_verifier(
            secret->bytes(), TokenPurpose::WorkloadBootstrap, options_.token_key);
        if (const auto* error = std::get_if<Failure>(&verifier)) {
          return *error;
        }
        material =
            WorkloadBootstrapVerifier{std::get<TokenVerifier>(verifier), initial.credential.binding,
                                      std::get<std::vector<Service>>(audiences), expires};
      } else {
        auto lifetime = detail::add_lifetime(initial.now, options_.password_credential_lifetime);
        if (const auto* error = std::get_if<Failure>(&lifetime)) {
          return *error;
        }
        expires = std::get<TimePoint>(lifetime);
        auto verifier = dependencies_.passwords.derive(secret->bytes());
        if (const auto* error = std::get_if<Failure>(&verifier)) {
          return *error;
        }
        material = std::get<PasswordVerifier>(verifier);
      }
      const CredentialVersionRecord record{issued_ref,
                                           initial.credential.subject,
                                           std::move(material),
                                           CredentialState::Active,
                                           initial.now,
                                           std::nullopt,
                                           initial.credential.binding,
                                           expires};
      if (std::holds_alternative<Failure>(validate_credential_record(record))) {
        return internal();
      }
      const auto parameters = CredentialDeliveryParameters{initial.credential.ref.version};
      auto retention = detail::add_lifetime(initial.now, kSecretReceiptRetention);
      if (const auto* error = std::get_if<Failure>(&retention)) {
        return *error;
      }
      SecretReplayPayload payload{
          EphemeralCredentialDelivery{issued_ref, std::move(secret), expires}};
      auto sealed =
          seal_delivery(dependencies_, options_, scope, parameters, initial.credential.subject,
                        record, initial.now, std::get<TimePoint>(retention), payload);
      if (const auto* error = std::get_if<Failure>(&sealed)) {
        return *error;
      }
      auto receipt = std::get<EncryptedSecretReceipt>(sealed);
      auto opened = detail::begin(dependencies_.transactions, command.context);
      if (const auto* error = std::get_if<Failure>(&opened)) {
        return *error;
      }
      auto& work = std::get<detail::Work>(opened);
      auto actor = current_actor(*work.transaction, command.actor, work.now);
      if (const auto* error = std::get_if<Failure>(&actor)) {
        return *error;
      }
      const auto& authority = std::get<detail::PeerAuthority>(actor);
      auto current = work.transaction->credentials().current(command.credential);
      if (const auto* error = std::get_if<Failure>(&current)) {
        return *error;
      }
      const auto& credential = std::get<std::optional<CredentialVersionRecord>>(current);
      if (!credential || credential->ref != initial.credential.ref) {
        return detail::failure(ErrorCode::Conflict, Recovery::ReadCurrent);
      }
      auto authorized =
          authorize(authority, Permission::IdentityPrincipalManage, credential->binding);
      if (const auto* error = std::get_if<Failure>(&authorized)) {
        return scoped_authorization(*error);
      }
      auto principal = current_principal(*work.transaction, credential->subject);
      if (const auto* error = std::get_if<Failure>(&principal)) {
        return *error;
      }
      const auto& current_principal_value = std::get<Principal>(principal);
      const auto owner = detail::credential_owner_binding(current_principal_value, *credential);
      if (const auto* error = std::get_if<Failure>(&owner)) {
        return *error;
      }
      // First successful rotation turns the registration's Disabled principal live. The same
      // short UoW conditionally advances the principal version; other non-live states fail closed.
      if (current_principal_value.state == LifecycleState::Disabled) {
        auto activated = current_principal_value;
        auto next_principal = detail::next_version(activated.version);
        if (const auto* error = std::get_if<Failure>(&next_principal)) {
          return *error;
        }
        activated.version = std::get<ResourceVersion>(next_principal);
        activated.state = LifecycleState::Active;
        auto saved_principal =
            work.transaction->principals().save(activated, current_principal_value.version);
        if (const auto* error = std::get_if<Failure>(&saved_principal)) {
          return *error;
        }
      } else if (current_principal_value.state != LifecycleState::Active) {
        return detail::failure(ErrorCode::Conflict, Recovery::ReadCurrent);
      }
      auto saved = work.transaction->credentials().save(record, credential->ref.version);
      if (const auto* error = std::get_if<Failure>(&saved)) {
        return *error;
      }
      auto inserted = work.transaction->secret_receipts().insert(receipt, options_.secret_receipts);
      if (const auto* error = std::get_if<Failure>(&inserted)) {
        return *error;
      }
      if (std::get<SecretReceiptWrite>(inserted).duplicate) {
        return detail::failure(ErrorCode::Conflict, Recovery::QuerySameIdentity);
      }
      auto audit = work.transaction->journal().append_audit(
          AuditEntry{authority.subject.principal.id,
                     {ResourceKind::Credential, ResourceId{issued_ref.id.value}},
                     command.context.request_id,
                     command.context.trace_id,
                     "RotateCredential",
                     {},
                     authority.subject.authorization.version,
                     work.now});
      if (const auto* error = std::get_if<Failure>(&audit)) {
        return *error;
      }
      auto checkpoint = detail::final_check(
          work, command.context,
          std::min({peer_valid_until(authority), receipt.binding.retain_until, expires}), true);
      if (const auto* error = std::get_if<Failure>(&checkpoint)) {
        return *error;
      }
      auto committed = detail::committed(*work.transaction);
      if (const auto* error = std::get_if<Failure>(&committed)) {
        return *error;
      }
      return CredentialDeliveryReply{
          issued_ref, std::move(std::get<EphemeralCredentialDelivery>(payload).secret),
          delivery_id(receipt.binding.request), expires, work.now};
    } catch (...) {
      return internal();
    }
  }
  Outcome<RevokeCredentialReply>
  revoke_credential(const RevokeCredentialCommand& command) override {
    try {
      if (!bounded(command.credential.value) || command.expected_version.value == 0 ||
          !bounded(command.context.idempotency_key) || !bounded(command.context.request_id) ||
          !bounded(command.context.trace_id) || command.context.deadline == TimePoint{}) {
        return invalid();
      }
      // Mature outer fingerprint of domain + credential id + expected version; computed outside
      // the UoW. Same idempotency key with a different expected version conflicts on replay.
      auto fingerprint = dependencies_.fingerprints.revoke_credential(command.credential,
                                                                      command.expected_version);
      if (const auto* error = std::get_if<Failure>(&fingerprint)) {
        return *error;
      }
      auto opened = detail::begin(dependencies_.transactions, command.context);
      if (const auto* error = std::get_if<Failure>(&opened)) {
        return *error;
      }
      auto& work = std::get<detail::Work>(opened);
      auto found = work.transaction->credentials().current(command.credential);
      if (const auto* error = std::get_if<Failure>(&found)) {
        return *error;
      }
      const auto& credential = std::get<std::optional<CredentialVersionRecord>>(found);
      if (!credential) {
        return detail::failure(ErrorCode::NotFound, Recovery::ReadCurrent);
      }
      auto actor = current_actor(*work.transaction, command.actor, work.now);
      if (const auto* error = std::get_if<Failure>(&actor)) {
        return *error;
      }
      const auto& authority = std::get<detail::PeerAuthority>(actor);
      auto authorized =
          authorize(authority, Permission::IdentityPrincipalManage, credential->binding);
      if (const auto* error = std::get_if<Failure>(&authorized)) {
        return scoped_authorization(*error);
      }
      auto principal = current_principal(*work.transaction, credential->subject);
      if (const auto* error = std::get_if<Failure>(&principal)) {
        return *error;
      }
      const auto owner =
          detail::credential_owner_binding(std::get<Principal>(principal), *credential);
      if (const auto* error = std::get_if<Failure>(&owner)) {
        return *error;
      }
      const ResourceRef resource{ResourceKind::Credential, ResourceId{command.credential.value}};
      const ReceiptKey key{authority.subject.principal.id, "RevokeCredential", resource,
                           command.context.idempotency_key};
      auto original = work.transaction->receipts().find(key);
      if (const auto* error = std::get_if<Failure>(&original)) {
        return *error;
      }
      if (const auto& receipt = std::get<std::optional<CommandReceipt>>(original)) {
        const auto* result = std::get_if<ResourceMutationResult>(&receipt->result);
        if (receipt->key != key || !result || result->resource != resource ||
            result->version.value == 0) {
          return internal();
        }
        if (receipt->fingerprint != std::get<Sha256Digest>(fingerprint)) {
          return detail::failure(ErrorCode::Conflict, Recovery::QuerySameIdentity);
        }
        auto checkpoint =
            detail::final_check(work, command.context, peer_valid_until(authority), true);
        if (const auto* error = std::get_if<Failure>(&checkpoint)) {
          return *error;
        }
        work.transaction->rollback();
        return RevokeCredentialReply{result->version, work.now};
      }
      if (credential->state == CredentialState::Revoked) {
        return detail::failure(ErrorCode::Conflict, Recovery::ReadCurrent);
      }
      if (credential->ref.version != command.expected_version) {
        return detail::failure(ErrorCode::Conflict, Recovery::ReadCurrent);
      }
      auto version = detail::next_version(credential->ref.version);
      if (const auto* error = std::get_if<Failure>(&version)) {
        return *error;
      }
      CredentialVersionRecord revoked = *credential;
      revoked.ref.version = std::get<ResourceVersion>(version);
      revoked.state = CredentialState::Revoked;
      revoked.revoked_at = work.now;
      if (std::holds_alternative<Failure>(validate_credential_record(revoked))) {
        return internal();
      }
      auto retention = detail::add_lifetime(work.now, kMinimumReceiptRetention);
      if (const auto* error = std::get_if<Failure>(&retention)) {
        return *error;
      }
      auto saved = work.transaction->credentials().save(revoked, credential->ref.version);
      if (const auto* error = std::get_if<Failure>(&saved)) {
        return *error;
      }
      // Same physical transaction: advance the subject authorization watermark and append the
      // bounded invalidation fact so stream/snapshot consumers converge on the revoked credential.
      auto authorization = work.transaction->authorizations().find(revoked.subject);
      if (const auto* error = std::get_if<Failure>(&authorization)) {
        return *error;
      }
      const auto& current_authorization =
          std::get<std::optional<AuthorizationState>>(authorization);
      ResourceVersion authorization_version{1};
      if (current_authorization) {
        if (current_authorization->subject != revoked.subject ||
            current_authorization->version.value == 0) {
          return internal();
        }
        auto advanced = assignment_changed(*current_authorization, current_authorization->version);
        if (const auto* error = std::get_if<Failure>(&advanced)) {
          return *error;
        }
        const auto& next_authorization = std::get<AuthorizationState>(advanced);
        auto saved_authorization = work.transaction->authorizations().save(
            next_authorization, current_authorization->version);
        if (const auto* error = std::get_if<Failure>(&saved_authorization)) {
          return *error;
        }
        authorization_version = next_authorization.version;
      } else {
        // First materialized authority for this subject (e.g. a freshly registered workload):
        // initialize the watermark in the same transaction as the credential revocation.
        auto created = work.transaction->authorizations().save(
            AuthorizationState{revoked.subject, authorization_version, true}, std::nullopt);
        if (const auto* error = std::get_if<Failure>(&created)) {
          return *error;
        }
      }
      auto invalidation = append_identity_invalidation(
          *work.transaction, dependencies_.random,
          InvalidationEvent{revoked.subject,
                            authorization_version,
                            {{ResourceKind::Principal, ResourceId{revoked.subject.value}}},
                            false},
          work.now);
      if (const auto* error = std::get_if<Failure>(&invalidation)) {
        return *error;
      }
      auto receipt = work.transaction->receipts().insert(
          CommandReceipt{key, std::get<Sha256Digest>(fingerprint),
                         ResourceMutationResult{resource, revoked.ref.version}, work.now,
                         std::get<TimePoint>(retention)},
          ReceiptCapacity{});
      if (const auto* error = std::get_if<Failure>(&receipt)) {
        return *error;
      }
      auto audit = work.transaction->journal().append_audit(
          AuditEntry{authority.subject.principal.id,
                     resource,
                     command.context.request_id,
                     command.context.trace_id,
                     "RevokeCredential",
                     {},
                     authority.subject.authorization.version,
                     work.now});
      if (const auto* error = std::get_if<Failure>(&audit)) {
        return *error;
      }
      auto checkpoint =
          detail::final_check(work, command.context, peer_valid_until(authority), true);
      if (const auto* error = std::get_if<Failure>(&checkpoint)) {
        return *error;
      }
      auto committed = detail::committed(*work.transaction);
      if (const auto* error = std::get_if<Failure>(&committed)) {
        return *error;
      }
      return RevokeCredentialReply{revoked.ref.version, work.now};
    } catch (...) {
      return internal();
    }
  }
  Outcome<PrincipalStatusReply>
  get_principal_status(const GetPrincipalStatusQuery& query) override {
    try {
      if (!bounded(query.principal.value)) {
        return invalid();
      }
      auto opened = detail::begin(dependencies_.transactions, query.context);
      if (const auto* error = std::get_if<Failure>(&opened)) {
        return *error;
      }
      auto& work = std::get<detail::Work>(opened);
      auto found = work.transaction->principals().find(query.principal);
      if (const auto* error = std::get_if<Failure>(&found)) {
        return *error;
      }
      const auto& principal = std::get<std::optional<Principal>>(found);
      if (!principal) {
        return detail::failure(ErrorCode::NotFound, Recovery::ReadCurrent);
      }
      const auto& value = *principal;
      auto actor = current_actor(*work.transaction, query.actor, work.now);
      if (const auto* error = std::get_if<Failure>(&actor)) {
        return *error;
      }
      const auto& authority = std::get<detail::PeerAuthority>(actor);
      auto authorized =
          authorize(authority, Permission::IdentityPrincipalRead, principal_resource(value));
      if (const auto* error = std::get_if<Failure>(&authorized)) {
        return scoped_authorization(*error);
      }
      auto checkpoint = detail::final_check(work, query.context, peer_valid_until(authority), true);
      if (const auto* error = std::get_if<Failure>(&checkpoint)) {
        return *error;
      }
      work.transaction->rollback();
      return PrincipalStatusReply{value, work.now};
    } catch (...) {
      return internal();
    }
  }
  Outcome<CredentialStatusReply>
  get_credential_status(const GetCredentialStatusQuery& query) override {
    try {
      if (!bounded(query.credential.value)) {
        return invalid();
      }
      auto opened = detail::begin(dependencies_.transactions, query.context);
      if (const auto* error = std::get_if<Failure>(&opened)) {
        return *error;
      }
      auto& work = std::get<detail::Work>(opened);
      auto found = work.transaction->credentials().current(query.credential);
      if (const auto* error = std::get_if<Failure>(&found)) {
        return *error;
      }
      const auto& credential = std::get<std::optional<CredentialVersionRecord>>(found);
      if (!credential) {
        return detail::failure(ErrorCode::NotFound, Recovery::ReadCurrent);
      }
      auto actor = current_actor(*work.transaction, query.actor, work.now);
      if (const auto* error = std::get_if<Failure>(&actor)) {
        return *error;
      }
      const auto& authority = std::get<detail::PeerAuthority>(actor);
      auto authorized =
          authorize(authority, Permission::IdentityPrincipalRead, credential->binding);
      if (const auto* error = std::get_if<Failure>(&authorized)) {
        return scoped_authorization(*error);
      }
      auto status = credential_status(*credential, work.now);
      if (std::holds_alternative<Failure>(status)) {
        return internal();
      }
      auto checkpoint = detail::final_check(work, query.context, peer_valid_until(authority), true);
      if (const auto* error = std::get_if<Failure>(&checkpoint)) {
        return *error;
      }
      work.transaction->rollback();
      return std::get<CredentialStatusSnapshot>(status);
    } catch (...) {
      return internal();
    }
  }

private:
  IdentityPrincipalDependencies dependencies_;
  IdentityPrincipalOptions options_;
};
} // namespace

Outcome<std::unique_ptr<IdentityPrincipalApplication>>
make_identity_principal_application(IdentityPrincipalDependencies dependencies,
                                    IdentityPrincipalOptions options) {
  try {
    const auto valid = validate_options(options);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    return std::unique_ptr<IdentityPrincipalApplication>{
        std::make_unique<Application>(dependencies, std::move(options))};
  } catch (...) {
    return detail::failure(ErrorCode::Internal);
  }
}

} // namespace kiarabot::core::target
