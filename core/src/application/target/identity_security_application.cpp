// clang-format off
#include "kiarabot/core/application/target/identity_security_application.hpp"

#include <limits>
#include <utility>

#include "identity_security_application_detail.hpp"
// clang-format on

namespace kiarabot::core::target {
namespace {
namespace detail = identity_security_detail;
using detail::failure;
template <typename Function> auto boundary(Function&& function) -> decltype(function()) {
  try {
    return function();
  } catch (const std::bad_alloc&) {
    return failure(ErrorCode::UpstreamUnavailable);
  } catch (...) {
    return failure(ErrorCode::Internal);
  }
}
struct LoginRead {
  TimePoint now;
  std::optional<LoginIdentity> login;
  std::optional<CredentialVersionRecord> credential;
  std::optional<Principal> principal;
  std::optional<EncryptedSecretReceipt> receipt;
};
class Application final : public IdentitySecurityApplication {
public:
  Application(IdentitySecurityDependencies dependencies, IdentitySecurityOptions options)
      : dependencies_(dependencies), options_(std::move(options)), snapshots_(options_) {}
  ServiceReadiness readiness() const override {
    return {false,
            {{Dependency::BusinessImplementation, DependencyState::Available},
             {Dependency::DatabaseSchema, DependencyState::Unverified},
             {Dependency::Credentials, DependencyState::Unverified}}};
  }
  Outcome<IdentityTokenReply> login(const LoginRequest& request) override {
    return boundary([&] {
      return audit_rejection(login_impl(request), request.context, request.control_peer, "Login");
    });
  }
  Outcome<IdentityTokenReply> refresh_session(const RefreshSessionRequest& request) override {
    return boundary([&] {
      return audit_rejection(refresh_impl(request), request.context, request.control_peer,
                             "RefreshSession");
    });
  }
  Outcome<ValidatedSession> validate_session(const AccessSessionRequest& request) override {
    return boundary([&] {
      return audit_rejection(validate_impl(request), request.context, request.control_peer,
                             "ValidateSession");
    });
  }
  Outcome<CurrentPrincipal> get_principal(const AccessSessionRequest& request) override {
    return boundary([&]() -> Outcome<CurrentPrincipal> {
      auto result = audit_rejection(validate_impl(request), request.context, request.control_peer,
                                    "GetPrincipal");
      if (const auto* error = std::get_if<Failure>(&result)) {
        return *error;
      }
      auto& current = std::get<ValidatedSession>(result);
      return CurrentPrincipal{std::move(current.principal), std::move(current.assignments),
                              current.authorization_version, current.checked_at};
    });
  }
  Outcome<RevokeSessionReply> revoke_session(const RevokeSessionRequest& request) override {
    return boundary([&] {
      return audit_rejection(revoke_impl(request), request.context, request.actor.direct_peer,
                             "RevokeSession");
    });
  }
  Outcome<SessionPageReply> list_sessions(const ListSessionsRequest& request) override {
    return boundary([&] {
      return audit_rejection(list_impl(request), request.context, request.actor.direct_peer,
                             "ListSessions");
    });
  }

private:
  template <typename Value>
  Outcome<Value> audit_rejection(Outcome<Value> result, const IdentityRequestContext& context,
                                 const VerifiedPeer& peer, const std::string& method) {
    const auto* error = std::get_if<Failure>(&result);
    if (!error || error->acceptance != Acceptance::NotAccepted ||
        (error->code != ErrorCode::Unauthenticated && error->code != ErrorCode::PermissionDenied &&
         error->code != ErrorCode::RateLimited && error->code != ErrorCode::Conflict)) {
      return result;
    }
    const auto rejection = *error;
    auto opened = detail::begin(dependencies_.transactions, context);
    if (const auto* dependency = std::get_if<Failure>(&opened)) {
      return *dependency;
    }
    auto& work = std::get<detail::Work>(opened);
    // Evidence is already from the TLS boundary. Record the rejected direct peer, never the
    // unverified represented actor, token, password, username or arbitrary error text.
    const auto& actor = peer.binding.certificate.subject;
    if (!detail::bounded_text(actor.value)) {
      return failure(ErrorCode::Internal);
    }
    auto registered = detail::read_peer(*work.transaction, peer);
    if (const auto* dependency = std::get_if<Failure>(&registered)) {
      if (dependency->code == ErrorCode::Unauthenticated) {
        return rejection;
      }
      return *dependency;
    }
    const auto& evidence = std::get<detail::PeerAuthority>(registered);
    if (evidence.credential.ref != peer.binding.credential ||
        evidence.credential.subject != actor || evidence.subject.principal.id != actor) {
      return rejection;
    }
    auto audit = work.transaction->journal().append_audit(
        AuditEntry{actor,
                   {ResourceKind::Principal, {actor.value}},
                   context.request_id,
                   context.trace_id,
                   method,
                   rejection.code,
                   evidence.subject.authorization.version,
                   work.now});
    if (const auto* dependency = std::get_if<Failure>(&audit)) {
      return *dependency;
    }
    auto checkpoint = detail::final_check(work, context, TimePoint::max());
    if (const auto* dependency = std::get_if<Failure>(&checkpoint)) {
      return *dependency;
    }
    auto commit = detail::committed(*work.transaction);
    if (const auto* dependency = std::get_if<Failure>(&commit)) {
      return *dependency;
    }
    return rejection;
  }
  Outcome<LoginRead> read_login(const LoginRequest& request, const std::string& username,
                                const SecretReceiptScope& scope) {
    auto opened = detail::begin(dependencies_.transactions, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& work = std::get<detail::Work>(opened);
    auto peer = detail::read_peer(*work.transaction, request.control_peer);
    if (const auto* error = std::get_if<Failure>(&peer)) {
      return *error;
    }
    auto gate =
        detail::check_peer(std::get<detail::PeerAuthority>(peer), request.control_peer, work.now);
    if (const auto* error = std::get_if<Failure>(&gate)) {
      return *error;
    }
    LoginRead result{work.now, {}, {}, {}, {}};
    auto login = work.transaction->credentials().login(username);
    if (const auto* error = std::get_if<Failure>(&login)) {
      return *error;
    }
    result.login = std::get<std::optional<LoginIdentity>>(login);
    if (result.login) {
      auto credential = work.transaction->credentials().current(result.login->password_credential);
      if (const auto* error = std::get_if<Failure>(&credential)) {
        return *error;
      }
      result.credential = std::get<std::optional<CredentialVersionRecord>>(credential);
      auto principal = work.transaction->principals().find(result.login->subject);
      if (const auto* error = std::get_if<Failure>(&principal)) {
        return *error;
      }
      result.principal = std::get<std::optional<Principal>>(principal);
    }
    auto receipt = work.transaction->secret_receipts().find(scope);
    if (const auto* error = std::get_if<Failure>(&receipt)) {
      return *error;
    }
    result.receipt = std::get<std::optional<EncryptedSecretReceipt>>(receipt);
    return result;
  }
  Status login_current(const LoginRead& read) {
    if (!read.login || !read.credential || !read.principal ||
        read.principal->kind != PrincipalKind::Human || read.login->subject != read.principal->id ||
        read.login->password_credential != read.credential->ref.id ||
        !std::holds_alternative<PasswordVerifier>(read.credential->material)) {
      return authentication_rejected();
    }
    auto owner = detail::credential_owner_binding(*read.principal, *read.credential);
    if (const auto* error = std::get_if<Failure>(&owner)) {
      return *error;
    }
    return require_current_credential(*read.principal, *read.credential, read.credential->ref,
                                      read.now);
  }
  struct ReplayRead {
    TimePoint now;
    SessionSecurity session;
  };
  Outcome<IdentityTokenReply>
  replay(const IdentityRequestContext& context, const VerifiedPeer& peer,
         const EncryptedSecretReceipt& receipt, const SecretReceiptScope& scope,
         const SecretRequestParameters& parameters, const RequestCommitment& commitment,
         const LoginIdentity* login_expectation = nullptr) {
    if (!receipt.binding.session) {
      return failure(ErrorCode::Internal);
    }
    auto current =
        replay_gate(context, peer, receipt, scope, parameters, commitment, login_expectation);
    if (const auto* error = std::get_if<Failure>(&current)) {
      return *error;
    }
    auto opened = dependencies_.secret_receipts.open(receipt.sealed, receipt.binding);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& payload = std::get<SecretReplayPayload>(opened);
    auto* pair = std::get_if<EphemeralTokenPair>(&payload);
    if (!pair || pair->session != *receipt.binding.session ||
        pair->rotation != receipt.binding.result_rotation || !pair->access || !pair->refresh) {
      return failure(ErrorCode::Internal);
    }
    const auto& authenticated = std::get<ReplayRead>(current).session;
    if (pair->access_expires_at != authenticated.access_expires_at ||
        pair->refresh_expires_at != authenticated.refresh_expires_at) {
      return failure(ErrorCode::Internal);
    }
    auto access = dependencies_.tokens.verify(pair->access->bytes(), authenticated.access);
    if (const auto* error = std::get_if<Failure>(&access)) {
      return *error;
    }
    auto refresh = dependencies_.tokens.verify(pair->refresh->bytes(), authenticated.refresh);
    if (const auto* error = std::get_if<Failure>(&refresh)) {
      return *error;
    }
    if (!std::get<bool>(access) || !std::get<bool>(refresh)) {
      return failure(ErrorCode::Internal);
    }
    auto checked =
        replay_gate(context, peer, receipt, scope, parameters, commitment, login_expectation);
    if (const auto* error = std::get_if<Failure>(&checked)) {
      return *error;
    }
    const auto& final = std::get<ReplayRead>(checked);
    if (final.session.version != authenticated.version ||
        !same_token_verifier(final.session.access, authenticated.access) ||
        !same_token_verifier(final.session.refresh, authenticated.refresh)) {
      return authentication_rejected();
    }
    return IdentityTokenReply{std::move(*pair), final.now};
  }
  Outcome<ReplayRead> replay_gate(const IdentityRequestContext& context, const VerifiedPeer& peer,
                                  const EncryptedSecretReceipt& receipt,
                                  const SecretReceiptScope& scope,
                                  const SecretRequestParameters& parameters,
                                  const RequestCommitment& commitment,
                                  const LoginIdentity* login_expectation) {
    if (!receipt.binding.session) {
      return failure(ErrorCode::Internal);
    }
    auto opened = detail::begin(dependencies_.transactions, context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& work = std::get<detail::Work>(opened);
    auto current_peer = detail::read_peer(*work.transaction, peer);
    if (const auto* error = std::get_if<Failure>(&current_peer)) {
      return *error;
    }
    auto peer_gate =
        detail::check_peer(std::get<detail::PeerAuthority>(current_peer), peer, work.now);
    if (const auto* error = std::get_if<Failure>(&peer_gate)) {
      return *error;
    }
    if (const auto* login_scope = std::get_if<LoginReceiptScope>(&scope)) {
      if (!login_expectation ||
          login_expectation->normalized_login != login_scope->normalized_login ||
          login_expectation->subject != receipt.binding.subject ||
          login_expectation->password_credential != receipt.binding.credential.id) {
        return authentication_rejected();
      }
      auto identity = work.transaction->credentials().login(login_scope->normalized_login);
      if (const auto* error = std::get_if<Failure>(&identity)) {
        return *error;
      }
      const auto& row = std::get<std::optional<LoginIdentity>>(identity);
      if (!row || row->normalized_login != login_expectation->normalized_login ||
          row->subject != login_expectation->subject ||
          row->password_credential != login_expectation->password_credential ||
          row->version != login_expectation->version) {
        return authentication_rejected();
      }
    } else if (login_expectation) {
      return failure(ErrorCode::Internal);
    }
    auto current = detail::read_session(*work.transaction, *receipt.binding.session);
    if (const auto* error = std::get_if<Failure>(&current)) {
      return *error;
    }
    const auto& state = std::get<detail::SessionAuthority>(current);
    auto valid =
        require_secret_replay(receipt, scope, parameters, commitment, state.subject.principal,
                              state.credential, &state.session, work.now);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    auto checkpoint = detail::final_check(
        work, context,
        std::min({receipt.binding.retain_until, state.session.refresh_expires_at,
                  state.credential.expires_at.value_or(TimePoint::min()),
                  peer.binding.certificate.not_after,
                  std::get<detail::PeerAuthority>(current_peer)
                      .credential.expires_at.value_or(TimePoint::min())}));
    if (const auto* error = std::get_if<Failure>(&checkpoint)) {
      return *error;
    }
    return ReplayRead{work.now, state.session};
  }
  Outcome<IdentityTokenReply> login_impl(const LoginRequest& request) {
    auto valid = detail::context_valid(request.context, true);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    if (request.password.empty() || request.password.size() > 1024) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    auto normalized = detail::normalized_login(request.username);
    if (const auto* error = std::get_if<Failure>(&normalized)) {
      return *error;
    }
    const auto& username = std::get<std::string>(normalized);
    const SecretReceiptScope scope =
        LoginReceiptScope{request.control_peer.binding.certificate.subject, username,
                          request.context.idempotency_key};
    auto read = read_login(request, username, scope);
    if (const auto* error = std::get_if<Failure>(&read)) {
      return *error;
    }
    auto& first = std::get<LoginRead>(read);
    auto clock = dependencies_.clock.sample();
    if (const auto* error = std::get_if<Failure>(&clock)) {
      return *error;
    }
    auto limited = limiter_.consume(request.control_peer.binding.certificate.subject, username,
                                    std::get<SecurityClockSample>(clock).monotonic);
    if (const auto* error = std::get_if<Failure>(&limited)) {
      return *error;
    }
    const auto* stored =
        first.credential ? std::get_if<PasswordVerifier>(&first.credential->material) : nullptr;
    auto verified = dependencies_.passwords.verify(
        request.password, stored ? *stored : options_.dummy_password_verifier);
    if (const auto* error = std::get_if<Failure>(&verified)) {
      return *error;
    }
    if (!std::get<bool>(verified) || !stored) {
      return authentication_rejected();
    }
    auto state = login_current(first);
    if (const auto* error = std::get_if<Failure>(&state)) {
      return *error;
    }
    if (!first.login || !first.principal || !first.credential) {
      return failure(ErrorCode::Internal);
    }
    const auto initial_login = *first.login;
    const auto initial_principal = *first.principal;
    const auto initial_credential = *first.credential;
    const SecretRequestParameters parameters = LoginRequestParameters{};
    auto committed_request = dependencies_.tokens.commit_request(
        scope, parameters, request.password,
        first.receipt ? first.receipt->binding.request.key : options_.request_commitment_key);
    if (const auto* error = std::get_if<Failure>(&committed_request)) {
      return *error;
    }
    const auto commitment = std::get<RequestCommitment>(committed_request);
    if (first.receipt) {
      if (first.receipt->binding.subject != initial_principal.id ||
          first.receipt->binding.credential != initial_credential.ref) {
        return authentication_rejected();
      }
      return replay(request.context, request.control_peer, *first.receipt, scope, parameters,
                    commitment, &initial_login);
    }
    auto id = detail::random_uuid(dependencies_.random);
    if (const auto* error = std::get_if<Failure>(&id)) {
      return *error;
    }
    const SessionId session{std::get<std::string>(id)};
    auto prepared = detail::prepare_tokens(dependencies_, options_, session, 1, first.now);
    if (const auto* error = std::get_if<Failure>(&prepared)) {
      return *error;
    }
    auto& tokens = std::get<detail::PreparedTokens>(prepared);
    auto retention = detail::add_lifetime(first.now, kSecretReceiptRetention);
    if (const auto* error = std::get_if<Failure>(&retention)) {
      return *error;
    }
    SecretReceiptBinding binding{
        scope, initial_principal.id, session,   initial_credential.ref,        parameters,
        1,     commitment,           first.now, std::get<TimePoint>(retention)};
    SecretReplayPayload payload{std::move(tokens.pair)};
    auto sealed =
        dependencies_.secret_receipts.seal(payload, binding, options_.receipt_encryption_key);
    if (const auto* error = std::get_if<Failure>(&sealed)) {
      return *error;
    }
    EncryptedSecretReceipt receipt{binding, std::move(std::get<SealedSecret>(sealed))};
    auto opened = detail::begin(dependencies_.transactions, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& work = std::get<detail::Work>(opened);
    auto peer = detail::read_peer(*work.transaction, request.control_peer);
    if (const auto* error = std::get_if<Failure>(&peer)) {
      return *error;
    }
    auto peer_gate =
        detail::check_peer(std::get<detail::PeerAuthority>(peer), request.control_peer, work.now);
    if (const auto* error = std::get_if<Failure>(&peer_gate)) {
      return *error;
    }
    auto login = work.transaction->credentials().login(username);
    if (const auto* error = std::get_if<Failure>(&login)) {
      return *error;
    }
    const auto& login_row = std::get<std::optional<LoginIdentity>>(login);
    if (!login_row || login_row->normalized_login != username ||
        login_row->subject != initial_login.subject ||
        login_row->password_credential != initial_login.password_credential ||
        login_row->version != initial_login.version) {
      return authentication_rejected();
    }
    auto credential = work.transaction->credentials().current(initial_credential.ref.id);
    if (const auto* error = std::get_if<Failure>(&credential)) {
      return *error;
    }
    auto subject = detail::read_subject(*work.transaction, initial_principal.id);
    if (const auto* error = std::get_if<Failure>(&subject)) {
      return *error;
    }
    const auto& current_credential = std::get<std::optional<CredentialVersionRecord>>(credential);
    const auto& current_subject = std::get<detail::SubjectAuthority>(subject);
    if (current_subject.principal.kind != PrincipalKind::Human || !current_credential ||
        !std::holds_alternative<PasswordVerifier>(current_credential->material) ||
        std::get<PasswordVerifier>(current_credential->material).encoded !=
            std::get<PasswordVerifier>(initial_credential.material).encoded ||
        std::get<PasswordVerifier>(current_credential->material).hash_policy_version !=
            std::get<PasswordVerifier>(initial_credential.material).hash_policy_version) {
      return authentication_rejected();
    }
    auto owner = detail::credential_owner_binding(current_subject.principal, *current_credential);
    if (const auto* error = std::get_if<Failure>(&owner)) {
      return *error;
    }
    auto gate = require_current_credential(current_subject.principal, *current_credential,
                                           initial_credential.ref, work.now);
    if (const auto* error = std::get_if<Failure>(&gate)) {
      return *error;
    }
    if (work.now >= binding.retain_until ||
        work.now >= std::get<EphemeralTokenPair>(payload).access_expires_at) {
      return authentication_rejected();
    }
    auto existing = work.transaction->secret_receipts().find(scope);
    if (const auto* error = std::get_if<Failure>(&existing)) {
      return *error;
    }
    if (const auto& original = std::get<std::optional<EncryptedSecretReceipt>>(existing)) {
      if (original->binding.subject != initial_principal.id ||
          original->binding.credential != initial_credential.ref) {
        return authentication_rejected();
      }
      auto saved = *original;
      work.transaction.reset();
      return replay(request.context, request.control_peer, saved, scope, parameters, commitment,
                    &initial_login);
    }
    auto count = work.transaction->session_security().count_unexpired();
    if (const auto* error = std::get_if<Failure>(&count)) {
      return *error;
    }
    if (std::get<std::size_t>(count) >= kMaximumUnexpiredSessions) {
      return failure(ErrorCode::RateLimited, Recovery::Backoff);
    }
    auto sessions = work.transaction->session_security().list_metadata(
        initial_principal.id, options_.maximum_sessions_per_subject);
    if (const auto* error = std::get_if<Failure>(&sessions)) {
      return *error;
    }
    if (std::get<std::vector<SessionMetadata>>(sessions).size() >=
        options_.maximum_sessions_per_subject) {
      return failure(ErrorCode::RateLimited, Recovery::Backoff);
    }
    const auto& pair = std::get<EphemeralTokenPair>(payload);
    SessionSecurity security{session,
                             initial_principal.id,
                             initial_credential.ref,
                             tokens.access,
                             tokens.refresh,
                             first.now,
                             pair.access_expires_at,
                             pair.refresh_expires_at,
                             1,
                             {1},
                             false};
    auto saved_security = work.transaction->session_security().save(security, {}, {});
    if (const auto* error = std::get_if<Failure>(&saved_security)) {
      return *error;
    }
    auto inserted = work.transaction->secret_receipts().insert(receipt, options_.secret_receipts);
    if (const auto* error = std::get_if<Failure>(&inserted)) {
      return *error;
    }
    if (std::get<SecretReceiptWrite>(inserted).duplicate) {
      return failure(ErrorCode::Conflict, Recovery::QuerySameIdentity);
    }
    auto audit = work.transaction->journal().append_audit(
        AuditEntry{security.subject,
                   {ResourceKind::Session, {session.value}},
                   request.context.request_id,
                   request.context.trace_id,
                   "Login",
                   {},
                   current_subject.authorization.version,
                   work.now});
    if (const auto* error = std::get_if<Failure>(&audit)) {
      return *error;
    }
    auto checkpoint = detail::final_check(
        work, request.context,
        std::min({binding.retain_until, pair.access_expires_at,
                  current_credential->expires_at.value_or(TimePoint::min()),
                  request.control_peer.binding.certificate.not_after,
                  std::get<detail::PeerAuthority>(peer).credential.expires_at.value_or(
                      TimePoint::min())}));
    if (const auto* error = std::get_if<Failure>(&checkpoint)) {
      return *error;
    }
    auto committed = detail::committed(*work.transaction);
    if (const auto* error = std::get_if<Failure>(&committed)) {
      return *error;
    }
    return IdentityTokenReply{std::move(std::get<EphemeralTokenPair>(payload)), work.now};
  }
  Outcome<ValidatedSession> validate_impl(const AccessSessionRequest& request) {
    auto shape = detail::context_valid(request.context, false);
    if (const auto* error = std::get_if<Failure>(&shape)) {
      return *error;
    }
    auto candidates = detail::token_candidates(dependencies_, options_, request.access_token,
                                               TokenPurpose::Access);
    if (const auto* error = std::get_if<Failure>(&candidates)) {
      return *error;
    }
    std::optional<SessionSecurity> selected;
    {
      auto opened = detail::begin(dependencies_.transactions, request.context);
      if (const auto* error = std::get_if<Failure>(&opened)) {
        return *error;
      }
      auto& work = std::get<detail::Work>(opened);
      auto peer = detail::read_peer(*work.transaction, request.control_peer);
      if (const auto* error = std::get_if<Failure>(&peer)) {
        return *error;
      }
      auto gate =
          detail::check_peer(std::get<detail::PeerAuthority>(peer), request.control_peer, work.now);
      if (const auto* error = std::get_if<Failure>(&gate)) {
        return *error;
      }
      for (const auto& candidate : std::get<std::vector<TokenVerifier>>(candidates)) {
        auto found = work.transaction->session_security().by_access(candidate);
        if (const auto* error = std::get_if<Failure>(&found)) {
          return *error;
        }
        const auto& row = std::get<std::optional<SessionSecurity>>(found);
        if (row) {
          if (selected && selected->id != row->id) {
            return failure(ErrorCode::Internal);
          }
          selected = *row;
        }
      }
    }
    if (!selected) {
      return authentication_rejected();
    }
    auto verified = dependencies_.tokens.verify(request.access_token, selected->access);
    if (const auto* error = std::get_if<Failure>(&verified)) {
      return *error;
    }
    if (!std::get<bool>(verified)) {
      return authentication_rejected();
    }
    auto opened = detail::begin(dependencies_.transactions, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& work = std::get<detail::Work>(opened);
    auto peer = detail::read_peer(*work.transaction, request.control_peer);
    if (const auto* error = std::get_if<Failure>(&peer)) {
      return *error;
    }
    auto gate =
        detail::check_peer(std::get<detail::PeerAuthority>(peer), request.control_peer, work.now);
    if (const auto* error = std::get_if<Failure>(&gate)) {
      return *error;
    }
    auto current = detail::read_session(*work.transaction, selected->id);
    if (const auto* error = std::get_if<Failure>(&current)) {
      return *error;
    }
    auto& authority = std::get<detail::SessionAuthority>(current);
    if (authority.session.version != selected->version ||
        !same_token_verifier(authority.session.access, selected->access)) {
      return authentication_rejected();
    }
    auto state = require_session_current(authority.subject.principal, authority.credential,
                                         authority.session, TokenPurpose::Access, work.now);
    if (const auto* error = std::get_if<Failure>(&state)) {
      return *error;
    }
    auto assignments = validate_assignment_collection(authority.subject.assignments,
                                                      authority.subject.principal.id);
    if (const auto* error = std::get_if<Failure>(&assignments)) {
      return *error;
    }
    auto checkpoint = detail::final_check(
        work, request.context,
        std::min({authority.session.access_expires_at, authority.session.refresh_expires_at,
                  authority.credential.expires_at.value_or(TimePoint::min()),
                  request.control_peer.binding.certificate.not_after,
                  std::get<detail::PeerAuthority>(peer).credential.expires_at.value_or(
                      TimePoint::min())}));
    if (const auto* error = std::get_if<Failure>(&checkpoint)) {
      return *error;
    }
    return ValidatedSession{std::move(authority.subject.principal),
                            session_metadata(authority.session),
                            std::move(authority.subject.assignments),
                            authority.subject.authorization.version, work.now};
  }

  struct RefreshRead {
    TimePoint now;
    RefreshHandle handle;
    detail::SessionAuthority authority;
    std::optional<EncryptedSecretReceipt> receipt;
    RefreshDecision decision;
  };
  Outcome<RefreshRead> read_refresh(const RefreshSessionRequest& request,
                                    const std::vector<TokenVerifier>& candidates) {
    auto opened = detail::begin(dependencies_.transactions, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& work = std::get<detail::Work>(opened);
    auto peer = detail::read_peer(*work.transaction, request.control_peer);
    if (const auto* error = std::get_if<Failure>(&peer)) {
      return *error;
    }
    auto gate =
        detail::check_peer(std::get<detail::PeerAuthority>(peer), request.control_peer, work.now);
    if (const auto* error = std::get_if<Failure>(&gate)) {
      return *error;
    }
    RefreshRecoveryLookup lookup{request.control_peer.binding.certificate.subject,
                                 request.context.idempotency_key};
    std::optional<RefreshHandle> selected;
    for (const auto& candidate : candidates) {
      auto found =
          work.transaction->session_security().resolve_refresh(candidate, lookup, work.now);
      if (const auto* error = std::get_if<Failure>(&found)) {
        return *error;
      }
      const auto& handle = std::get<std::optional<RefreshHandle>>(found);
      if (handle) {
        if (selected) {
          return failure(ErrorCode::Internal);
        }
        selected = *handle;
      }
    }
    if (!selected) {
      return authentication_rejected();
    }
    const auto input = std::visit(
        [](const auto& handle) -> CurrentRefreshHandle {
          if constexpr (requires { handle.input; }) {
            return handle.input;
          } else {
            return handle;
          }
        },
        *selected);
    auto current = detail::read_session(*work.transaction, input.session);
    if (const auto* error = std::get_if<Failure>(&current)) {
      return *error;
    }
    auto& authority = std::get<detail::SessionAuthority>(current);
    auto decision = check_refresh_state(authority.subject.principal, authority.credential,
                                        authority.session, *selected, lookup, work.now);
    if (const auto* error = std::get_if<Failure>(&decision)) {
      return *error;
    }
    const SecretReceiptScope scope =
        RefreshReceiptScope{lookup.control_peer, input.session, lookup.idempotency_key};
    auto receipt = work.transaction->secret_receipts().find(scope);
    if (const auto* error = std::get_if<Failure>(&receipt)) {
      return *error;
    }
    return RefreshRead{work.now, *selected, std::move(authority),
                       std::get<std::optional<EncryptedSecretReceipt>>(receipt),
                       std::get<RefreshDecision>(decision)};
  }
  Outcome<IdentityTokenReply> refresh_impl(const RefreshSessionRequest& request) {
    auto valid = detail::context_valid(request.context, true);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    auto candidates = detail::token_candidates(dependencies_, options_, request.refresh_token,
                                               TokenPurpose::Refresh);
    if (const auto* error = std::get_if<Failure>(&candidates)) {
      return *error;
    }
    auto read = read_refresh(request, std::get<std::vector<TokenVerifier>>(candidates));
    if (const auto* error = std::get_if<Failure>(&read)) {
      return *error;
    }
    auto& first = std::get<RefreshRead>(read);
    const auto input = std::visit(
        [](const auto& handle) -> CurrentRefreshHandle {
          if constexpr (requires { handle.input; }) {
            return handle.input;
          } else {
            return handle;
          }
        },
        first.handle);
    auto verified = dependencies_.tokens.verify(request.refresh_token, input.input_verifier);
    if (const auto* error = std::get_if<Failure>(&verified)) {
      return *error;
    }
    if (!std::get<bool>(verified)) {
      return authentication_rejected();
    }
    const SecretReceiptScope scope =
        RefreshReceiptScope{request.control_peer.binding.certificate.subject, input.session,
                            request.context.idempotency_key};
    const SecretRequestParameters parameters = RefreshRequestParameters{input.input_rotation};
    auto commitment_result = dependencies_.tokens.commit_request(
        scope, parameters, request.refresh_token,
        first.receipt ? first.receipt->binding.request.key : options_.request_commitment_key);
    if (const auto* error = std::get_if<Failure>(&commitment_result)) {
      return *error;
    }
    const auto commitment = std::get<RequestCommitment>(commitment_result);
    if (first.receipt) {
      return replay(request.context, request.control_peer, *first.receipt, scope, parameters,
                    commitment);
    }
    if (first.decision != RefreshDecision::Rotate) {
      return failure(ErrorCode::Internal);
    }
    auto version = detail::next_version(first.authority.session.version);
    if (const auto* error = std::get_if<Failure>(&version)) {
      return *error;
    }
    auto prepared = detail::prepare_tokens(dependencies_, options_, input.session,
                                           input.input_rotation + 1, first.now);
    if (const auto* error = std::get_if<Failure>(&prepared)) {
      return *error;
    }
    auto& tokens = std::get<detail::PreparedTokens>(prepared);
    auto until = detail::add_lifetime(first.now, kSecretReceiptRetention);
    if (const auto* error = std::get_if<Failure>(&until)) {
      return *error;
    }
    const auto retain_until = std::min(std::get<TimePoint>(until), input.input_expires_at);
    SecretReceiptBinding binding{scope,         input.subject,
                                 input.session, input.credential,
                                 parameters,    input.input_rotation + 1,
                                 commitment,    first.now,
                                 retain_until};
    SecretReplayPayload payload{std::move(tokens.pair)};
    auto encrypted =
        dependencies_.secret_receipts.seal(payload, binding, options_.receipt_encryption_key);
    if (const auto* error = std::get_if<Failure>(&encrypted)) {
      return *error;
    }
    EncryptedSecretReceipt receipt{binding, std::move(std::get<SealedSecret>(encrypted))};
    // Re-resolve exact input/key after crypto. A concurrent successful rotation becomes recovery.
    auto final_read = read_refresh(request, std::get<std::vector<TokenVerifier>>(candidates));
    if (const auto* error = std::get_if<Failure>(&final_read)) {
      return *error;
    }
    auto& final_state = std::get<RefreshRead>(final_read);
    if (final_state.receipt) {
      return replay(request.context, request.control_peer, *final_state.receipt, scope, parameters,
                    commitment);
    }
    auto opened = detail::begin(dependencies_.transactions, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& work = std::get<detail::Work>(opened);
    auto peer = detail::read_peer(*work.transaction, request.control_peer);
    if (const auto* error = std::get_if<Failure>(&peer)) {
      return *error;
    }
    auto gate =
        detail::check_peer(std::get<detail::PeerAuthority>(peer), request.control_peer, work.now);
    if (const auto* error = std::get_if<Failure>(&gate)) {
      return *error;
    }
    auto current = detail::read_session(*work.transaction, input.session);
    if (const auto* error = std::get_if<Failure>(&current)) {
      return *error;
    }
    const auto& authority = std::get<detail::SessionAuthority>(current);
    RefreshRecoveryLookup lookup{request.control_peer.binding.certificate.subject,
                                 request.context.idempotency_key};
    auto decision = check_refresh_state(authority.subject.principal, authority.credential,
                                        authority.session, first.handle, lookup, work.now);
    if (const auto* error = std::get_if<Failure>(&decision)) {
      return *error;
    }
    if (std::get<RefreshDecision>(decision) != RefreshDecision::Rotate ||
        authority.session.version != first.authority.session.version || work.now >= retain_until) {
      return authentication_rejected();
    }
    const auto& pair = std::get<EphemeralTokenPair>(payload);
    auto changed = authority.session;
    changed.version = std::get<ResourceVersion>(version);
    changed.rotation = pair.rotation;
    changed.access = tokens.access;
    changed.refresh = tokens.refresh;
    changed.access_expires_at = pair.access_expires_at;
    changed.refresh_expires_at = pair.refresh_expires_at;
    auto consumption =
        work.transaction->session_security().record_refresh_consumption(ConsumedRefreshHandle{
            input, std::get<RefreshReceiptScope>(scope), first.now, retain_until});
    if (const auto* error = std::get_if<Failure>(&consumption)) {
      return *error;
    }
    auto saved = work.transaction->session_security().save(changed, authority.session.version,
                                                           authority.session.rotation);
    if (const auto* error = std::get_if<Failure>(&saved)) {
      return *error;
    }
    auto inserted = work.transaction->secret_receipts().insert(receipt, options_.secret_receipts);
    if (const auto* error = std::get_if<Failure>(&inserted)) {
      return *error;
    }
    if (std::get<SecretReceiptWrite>(inserted).duplicate) {
      return failure(ErrorCode::Conflict, Recovery::QuerySameIdentity);
    }
    auto changed_auth = assignment_changed(authority.subject.authorization,
                                           authority.subject.authorization.version);
    if (const auto* error = std::get_if<Failure>(&changed_auth)) {
      return *error;
    }
    auto auth_saved = work.transaction->authorizations().save(
        std::get<AuthorizationState>(changed_auth), authority.subject.authorization.version);
    if (const auto* error = std::get_if<Failure>(&auth_saved)) {
      return *error;
    }
    auto audit = work.transaction->journal().append_audit(
        AuditEntry{changed.subject,
                   {ResourceKind::Session, {changed.id.value}},
                   request.context.request_id,
                   request.context.trace_id,
                   "RefreshSession",
                   {},
                   authority.subject.authorization.version,
                   work.now});
    if (const auto* error = std::get_if<Failure>(&audit)) {
      return *error;
    }
    auto checkpoint = detail::final_check(
        work, request.context,
        std::min({retain_until, pair.access_expires_at,
                  authority.credential.expires_at.value_or(TimePoint::min()),
                  request.control_peer.binding.certificate.not_after,
                  std::get<detail::PeerAuthority>(peer).credential.expires_at.value_or(
                      TimePoint::min())}));
    if (const auto* error = std::get_if<Failure>(&checkpoint)) {
      return *error;
    }
    auto commit = detail::committed(*work.transaction);
    if (const auto* error = std::get_if<Failure>(&commit)) {
      return *error;
    }
    return IdentityTokenReply{std::move(std::get<EphemeralTokenPair>(payload)), work.now};
  }
  struct ActorAuthority {
    detail::SubjectAuthority subject;
    CredentialVersionRecord credential;
    std::optional<SessionSecurity> session;
    TimePoint peer_valid_until;
  };
  Outcome<VerifiedDelegation> verify_actor(const IdentityActorProof& actor) {
    auto sample = dependencies_.clock.sample();
    if (const auto* error = std::get_if<Failure>(&sample)) {
      return *error;
    }
    const auto& clock = std::get<SecurityClockSample>(sample);
    if (!clock.trusted || clock.maximum_known_skew < std::chrono::milliseconds::zero() ||
        clock.maximum_known_skew > kMaximumKnownClockSkew) {
      return failure(ErrorCode::UpstreamUnavailable);
    }
    auto verified = dependencies_.delegations.verify(actor.delegation, Service::Identity,
                                                     actor.direct_peer, clock);
    if (const auto* error = std::get_if<Failure>(&verified)) {
      return *error;
    }
    if (std::holds_alternative<Failure>(
            validate_delegation_shape(std::get<VerifiedDelegation>(verified).claims))) {
      return authentication_rejected();
    }
    return verified;
  }
  Outcome<ActorAuthority> current_actor(detail::Work& work, const IdentityActorProof& proof,
                                        const DelegationClaims& claims) {
    auto peer = detail::read_peer(*work.transaction, proof.direct_peer);
    if (const auto* error = std::get_if<Failure>(&peer)) {
      return *error;
    }
    auto peer_gate = detail::check_peer(std::get<detail::PeerAuthority>(peer), proof.direct_peer,
                                        work.now, false);
    if (const auto* error = std::get_if<Failure>(&peer_gate)) {
      return *error;
    }
    auto subject = detail::read_subject(*work.transaction, claims.subject);
    if (const auto* error = std::get_if<Failure>(&subject)) {
      return *error;
    }
    auto head = work.transaction->credentials().current(claims.source_credential.id);
    if (const auto* error = std::get_if<Failure>(&head)) {
      return *error;
    }
    const auto& credential = std::get<std::optional<CredentialVersionRecord>>(head);
    if (!credential) {
      return authentication_rejected();
    }
    std::optional<SessionSecurity> session;
    if (claims.source_session) {
      auto row = work.transaction->session_security().find(*claims.source_session);
      if (const auto* error = std::get_if<Failure>(&row)) {
        return *error;
      }
      session = std::get<std::optional<SessionSecurity>>(row);
    }
    auto& authority = std::get<detail::SubjectAuthority>(subject);
    auto owner = detail::credential_owner_binding(authority.principal, *credential);
    if (const auto* error = std::get_if<Failure>(&owner)) {
      return *error;
    }
    auto valid = require_delegation_origin_current(claims, authority.principal, *credential,
                                                   session ? &*session : nullptr, work.now);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    if (claims.authorization_version != authority.authorization.version ||
        claims.audience != Service::Identity ||
        claims.direct_actor != proof.direct_peer.binding.certificate.subject ||
        claims.client_certificate != proof.direct_peer.binding.certificate.fingerprint ||
        work.now < claims.not_before || work.now >= claims.expires_at) {
      return authentication_rejected();
    }
    auto assignments =
        validate_assignment_collection(authority.assignments, authority.principal.id);
    if (const auto* error = std::get_if<Failure>(&assignments)) {
      return *error;
    }
    return ActorAuthority{
        std::move(authority), *credential, std::move(session),
        std::min(
            std::get<detail::PeerAuthority>(peer).credential.expires_at.value_or(TimePoint::min()),
            proof.direct_peer.binding.certificate.not_after)};
  }
  Status proof_permission(const DelegationClaims& claims, Permission permission,
                          const ResourceRef& resource) {
    if (std::find(claims.permissions.begin(), claims.permissions.end(),
                  PermissionBinding{permission, resource}) == claims.permissions.end()) {
      return failure(ErrorCode::PermissionDenied);
    }
    return std::monostate{};
  }
  Outcome<RevokeSessionReply> revoke_impl(const RevokeSessionRequest& request) {
    auto valid = detail::context_valid(request.context, true);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    if (!detail::bounded_text(request.session.value)) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    auto verified = verify_actor(request.actor);
    if (const auto* error = std::get_if<Failure>(&verified)) {
      return *error;
    }
    const auto& claims = std::get<VerifiedDelegation>(verified).claims;
    const ResourceRef resource{ResourceKind::Session, {request.session.value}};
    auto proof = proof_permission(claims, Permission::IdentitySessionRevoke, resource);
    if (const auto* error = std::get_if<Failure>(&proof)) {
      return *error;
    }
    auto opened = detail::begin(dependencies_.transactions, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& work = std::get<detail::Work>(opened);
    auto actor = current_actor(work, request.actor, claims);
    if (const auto* error = std::get_if<Failure>(&actor)) {
      return *error;
    }
    const auto& authority = std::get<ActorAuthority>(actor);
    auto found = work.transaction->session_security().find(request.session);
    if (const auto* error = std::get_if<Failure>(&found)) {
      return *error;
    }
    const auto& target = std::get<std::optional<SessionSecurity>>(found);
    if (!target) {
      return failure(ErrorCode::NotFound, Recovery::ReadCurrent);
    }
    auto base_row = work.transaction->sessions().find(request.session);
    if (const auto* error = std::get_if<Failure>(&base_row)) {
      return *error;
    }
    const auto& base_metadata = std::get<std::optional<LoginSession>>(base_row);
    if (!base_metadata || base_metadata->subject != target->subject ||
        base_metadata->version != target->version || base_metadata->rotation != target->rotation ||
        base_metadata->expires_at != target->refresh_expires_at ||
        base_metadata->revoked != target->revoked) {
      return failure(ErrorCode::Internal);
    }
    auto authorized = authorize_session_revoke(
        authority.subject.principal, session_metadata(*target), authority.subject.assignments);
    if (const auto* error = std::get_if<Failure>(&authorized)) {
      return *error;
    }
    // hashlib.sha256(b"kiarabot.identity.RevokeSession.v1:empty"); content is empty,
    // actor/method/target/idempotency identity are already explicit in ReceiptKey.
    constexpr std::array<unsigned char, 32> kFingerprint{
        0x57, 0x7b, 0x5a, 0x4d, 0x56, 0xea, 0xd6, 0x5e, 0x24, 0x6a, 0x4f,
        0x40, 0x90, 0x78, 0xd3, 0xb6, 0x5b, 0x9c, 0xd5, 0x3b, 0xed, 0xeb,
        0x2c, 0x7e, 0x23, 0xf8, 0xb4, 0x68, 0xde, 0x8d, 0xb6, 0x06};
    Sha256Digest fingerprint;
    for (std::size_t index = 0; index < 32; ++index) {
      fingerprint.bytes[index] = std::byte{kFingerprint[index]};
    }
    const ReceiptKey key{claims.subject, "RevokeSession", resource,
                         request.context.idempotency_key};
    auto original = work.transaction->receipts().find(key);
    if (const auto* error = std::get_if<Failure>(&original)) {
      return *error;
    }
    if (const auto& receipt = std::get<std::optional<CommandReceipt>>(original)) {
      const auto* snapshot = std::get_if<SessionMetadata>(&receipt->result);
      if (!snapshot || receipt->key != key || receipt->fingerprint != fingerprint ||
          snapshot->id != request.session || snapshot->subject != target->subject ||
          !snapshot->revoked || snapshot->version.value == 0 || snapshot->rotation == 0 ||
          snapshot->expires_at <= snapshot->created_at) {
        return failure(ErrorCode::Internal);
      }
      auto valid_until =
          std::min(claims.expires_at, authority.credential.expires_at.value_or(TimePoint::min()));
      if (authority.session) {
        valid_until = std::min(valid_until, authority.session->access_expires_at);
      }
      auto checkpoint = detail::final_check(
          work, request.context, std::min(valid_until, authority.peer_valid_until), true);
      if (const auto* error = std::get_if<Failure>(&checkpoint)) {
        return *error;
      }
      return RevokeSessionReply{*snapshot, work.now};
    }
    auto changed = *target;
    if (!changed.revoked) {
      auto version = detail::next_version(changed.version);
      if (const auto* error = std::get_if<Failure>(&version)) {
        return *error;
      }
      changed.version = std::get<ResourceVersion>(version);
      changed.revoked = true;
      auto saved =
          work.transaction->session_security().save(changed, target->version, target->rotation);
      if (const auto* error = std::get_if<Failure>(&saved)) {
        return *error;
      }
      auto target_auth = work.transaction->authorizations().find(changed.subject);
      if (const auto* error = std::get_if<Failure>(&target_auth)) {
        return *error;
      }
      const auto& auth = std::get<std::optional<AuthorizationState>>(target_auth);
      if (!auth) {
        return failure(ErrorCode::Internal);
      }
      auto next = assignment_changed(*auth, auth->version);
      if (const auto* error = std::get_if<Failure>(&next)) {
        return *error;
      }
      auto saved_auth = work.transaction->authorizations().save(std::get<AuthorizationState>(next),
                                                                auth->version);
      if (const auto* error = std::get_if<Failure>(&saved_auth)) {
        return *error;
      }
    }
    auto expiry = detail::add_lifetime(work.now, kMinimumReceiptRetention);
    if (const auto* error = std::get_if<Failure>(&expiry)) {
      return *error;
    }
    const auto metadata = session_metadata(changed);
    auto receipt = work.transaction->receipts().insert(
        CommandReceipt{key, fingerprint, metadata, work.now, std::get<TimePoint>(expiry)},
        ReceiptCapacity{});
    if (const auto* error = std::get_if<Failure>(&receipt)) {
      return *error;
    }
    if (std::get<ReceiptWrite>(receipt).duplicate) {
      return failure(ErrorCode::Conflict, Recovery::QuerySameIdentity);
    }
    auto audit =
        work.transaction->journal().append_audit(AuditEntry{claims.subject,
                                                            resource,
                                                            request.context.request_id,
                                                            request.context.trace_id,
                                                            "RevokeSession",
                                                            {},
                                                            authority.subject.authorization.version,
                                                            work.now});
    if (const auto* error = std::get_if<Failure>(&audit)) {
      return *error;
    }
    auto valid_until =
        std::min(claims.expires_at, authority.credential.expires_at.value_or(TimePoint::min()));
    if (authority.session) {
      valid_until = std::min(valid_until, authority.session->access_expires_at);
    }
    auto checkpoint = detail::final_check(work, request.context,
                                          std::min(valid_until, authority.peer_valid_until), true);
    if (const auto* error = std::get_if<Failure>(&checkpoint)) {
      return *error;
    }
    auto commit = detail::committed(*work.transaction);
    if (const auto* error = std::get_if<Failure>(&commit)) {
      return *error;
    }
    return RevokeSessionReply{metadata, work.now};
  }
  Outcome<SessionPageReply> list_impl(const ListSessionsRequest& request) {
    auto valid = detail::context_valid(request.context, false);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    const auto page_size = request.page.page_size == 0 ? 20U : request.page.page_size;
    if (page_size > 100 || !detail::bounded_text(request.subject.value) ||
        request.page.page_token.size() > 1024 || request.page.snapshot_id.size() > 1024) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    auto verified = verify_actor(request.actor);
    if (const auto* error = std::get_if<Failure>(&verified)) {
      return *error;
    }
    const auto& claims = std::get<VerifiedDelegation>(verified).claims;
    auto proof = proof_permission(claims, Permission::IdentitySessionRead,
                                  {ResourceKind::Principal, {request.subject.value}});
    if (const auto* error = std::get_if<Failure>(&proof)) {
      return *error;
    }
    std::vector<SessionMetadata> records;
    TimePoint checked_at;
    ResourceVersion authorization_version;
    {
      auto opened = detail::begin(dependencies_.transactions, request.context);
      if (const auto* error = std::get_if<Failure>(&opened)) {
        return *error;
      }
      auto& work = std::get<detail::Work>(opened);
      auto actor = current_actor(work, request.actor, claims);
      if (const auto* error = std::get_if<Failure>(&actor)) {
        return *error;
      }
      const auto& authority = std::get<ActorAuthority>(actor);
      auto authorized = authorize_session_list(authority.subject.principal, request.subject,
                                               authority.subject.assignments);
      if (const auto* error = std::get_if<Failure>(&authorized)) {
        return *error;
      }
      authorization_version = authority.subject.authorization.version;
      auto until =
          std::min({claims.expires_at, authority.credential.expires_at.value_or(TimePoint::min()),
                    authority.peer_valid_until});
      if (authority.session) {
        until = std::min(until, authority.session->access_expires_at);
      }
      auto checkpoint = detail::final_check(work, request.context, until, true);
      if (const auto* error = std::get_if<Failure>(&checkpoint)) {
        return *error;
      }
      checked_at = work.now;
      if (request.page.snapshot_id.empty() && request.page.page_token.empty()) {
        auto found = work.transaction->session_security().list_metadata(
            request.subject, options_.maximum_snapshot_records);
        if (const auto* error = std::get_if<Failure>(&found)) {
          return *error;
        }
        records = std::move(std::get<std::vector<SessionMetadata>>(found));
      }
    }
    detail::SnapshotOwner owner{claims.subject,           request.subject,
                                claims.source_credential, claims.source_session,
                                authorization_version,    page_size};
    if (!request.page.snapshot_id.empty() || !request.page.page_token.empty()) {
      return finish_page(request, claims, owner, snapshots_.read(owner, request.page, checked_at));
    }
    auto id = detail::random_uuid(dependencies_.random);
    if (const auto* error = std::get_if<Failure>(&id)) {
      return *error;
    }
    std::vector<std::string> cursors{std::string{}};
    const auto pages = records.empty() ? std::size_t{1} : (records.size() - 1) / page_size + 1;
    for (std::size_t page = 1; page < pages; ++page) {
      auto cursor = detail::random_uuid(dependencies_.random);
      if (const auto* error = std::get_if<Failure>(&cursor)) {
        return *error;
      }
      cursors.push_back(std::get<std::string>(cursor));
    }
    auto expiry = detail::add_lifetime(checked_at, kSessionSnapshotLifetime);
    if (const auto* error = std::get_if<Failure>(&expiry)) {
      return *error;
    }
    // Final current authorization after random preparation; snapshot timestamp/records stay fixed.
    auto opened = detail::begin(dependencies_.transactions, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& work = std::get<detail::Work>(opened);
    auto actor = current_actor(work, request.actor, claims);
    if (const auto* error = std::get_if<Failure>(&actor)) {
      return *error;
    }
    const auto& authority = std::get<ActorAuthority>(actor);
    auto authorized = authorize_session_list(authority.subject.principal, request.subject,
                                             authority.subject.assignments);
    if (const auto* error = std::get_if<Failure>(&authorized)) {
      return *error;
    }
    if (authority.subject.authorization.version != authorization_version) {
      return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
    }
    auto until =
        std::min({claims.expires_at, authority.credential.expires_at.value_or(TimePoint::min()),
                  authority.peer_valid_until});
    if (authority.session) {
      until = std::min(until, authority.session->access_expires_at);
    }
    auto checkpoint = detail::final_check(work, request.context, until, true);
    if (const auto* error = std::get_if<Failure>(&checkpoint)) {
      return *error;
    }
    const auto final_now = work.now;
    work.transaction.reset();
    return finish_page(
        request, claims, owner,
        snapshots_.insert(detail::Snapshot{owner, std::get<std::string>(id), std::move(cursors),
                                           std::move(records), std::get<TimePoint>(expiry)},
                          final_now));
  }
  Outcome<SessionPageReply> finish_page(const ListSessionsRequest& request,
                                        const DelegationClaims& claims,
                                        const detail::SnapshotOwner& owner,
                                        Outcome<SessionPageReply> result) {
    if (const auto* error = std::get_if<Failure>(&result)) {
      return *error;
    }
    auto opened = detail::begin(dependencies_.transactions, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& work = std::get<detail::Work>(opened);
    auto actor = current_actor(work, request.actor, claims);
    if (const auto* error = std::get_if<Failure>(&actor)) {
      return *error;
    }
    const auto& authority = std::get<ActorAuthority>(actor);
    auto authorized = authorize_session_list(authority.subject.principal, request.subject,
                                             authority.subject.assignments);
    if (const auto* error = std::get_if<Failure>(&authorized)) {
      return *error;
    }
    if (authority.subject.authorization.version != owner.authorization_version) {
      return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
    }
    auto until =
        std::min({claims.expires_at, authority.credential.expires_at.value_or(TimePoint::min()),
                  authority.peer_valid_until, std::get<SessionPageReply>(result).expires_at});
    if (authority.session) {
      until = std::min(until, authority.session->access_expires_at);
    }
    auto checkpoint = detail::final_check(work, request.context, until, true);
    if (const auto* error = std::get_if<Failure>(&checkpoint)) {
      return *error;
    }
    auto page = std::move(std::get<SessionPageReply>(result));
    page.checked_at = work.now;
    return page;
  }
  IdentitySecurityDependencies dependencies_;
  const IdentitySecurityOptions options_;
  detail::Snapshots snapshots_;
  detail::LoginLimiter limiter_;
};
} // namespace

Outcome<std::unique_ptr<IdentitySecurityApplication>>
make_identity_security_application(IdentitySecurityDependencies dependencies,
                                   IdentitySecurityOptions options) {
  return boundary([&]() -> Outcome<std::unique_ptr<IdentitySecurityApplication>> {
    auto valid = detail::validate_options(options);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    return std::unique_ptr<IdentitySecurityApplication>{
        std::make_unique<Application>(dependencies, std::move(options))};
  });
}
} // namespace kiarabot::core::target
