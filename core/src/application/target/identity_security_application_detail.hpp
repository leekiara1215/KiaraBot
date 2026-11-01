#pragma once

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

#include "kiarabot/core/application/target/identity_security_application.hpp"
#include "kiarabot/core/domain/target/identity_state_rules.hpp"

namespace kiarabot::core::target::identity_security_detail {

inline Failure failure(ErrorCode code, Recovery recovery = Recovery::Diagnose,
                       Acceptance acceptance = Acceptance::NotAccepted) {
  return {code, acceptance, recovery, {}, {}};
}
inline bool bounded_text(std::string_view value) {
  return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string_view::npos;
}
inline Status validate_options(const IdentitySecurityOptions& options) {
  if (!bounded_text(options.token_key.value) ||
      !bounded_text(options.request_commitment_key.value) ||
      !bounded_text(options.receipt_encryption_key.value) ||
      options.token_verification_keys.empty() || options.token_verification_keys.size() > 16 ||
      options.dummy_password_verifier.encoded.empty() ||
      options.dummy_password_verifier.hash_policy_version.value == 0 ||
      options.password_credential_lifetime <= std::chrono::nanoseconds::zero() ||
      options.secret_receipts.max_records == 0 || options.maximum_sessions_per_subject == 0 ||
      options.maximum_snapshots == 0 || options.maximum_snapshot_records == 0) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  std::set<std::string> keys;
  for (const auto& key : options.token_verification_keys) {
    if (!bounded_text(key.value) || !keys.insert(key.value).second) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
  }
  if (!keys.contains(options.token_key.value)) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return std::monostate{};
}

struct Work {
  std::unique_ptr<IdentitySecurityUnitOfWork> transaction;
  TimePoint now;
};
// Ownership makes release before crypto explicit; no returned repository reference survives Work.
inline Outcome<Work> begin(IdentitySecurityUnitOfWorkFactory& factory,
                           const IdentityRequestContext& context) {
  auto opened = factory.begin();
  if (const auto* error = std::get_if<Failure>(&opened)) {
    return *error;
  }
  auto transaction = std::move(std::get<std::unique_ptr<IdentitySecurityUnitOfWork>>(opened));
  if (!transaction) {
    return failure(ErrorCode::Internal);
  }
  auto sampled = transaction->current_time();
  if (const auto* error = std::get_if<Failure>(&sampled)) {
    return *error;
  }
  const auto now = std::get<TimePoint>(sampled);
  if (now >= context.deadline) {
    return failure(ErrorCode::Timeout, Recovery::Backoff);
  }
  return Work{std::move(transaction), now};
}
inline Status committed(IdentitySecurityUnitOfWork& transaction) {
  switch (transaction.commit()) {
  case CommitResult::Committed:
    return std::monostate{};
  case CommitResult::Unknown:
    return failure(ErrorCode::UpstreamUnavailable, Recovery::QuerySameIdentity,
                   Acceptance::Unknown);
  case CommitResult::RolledBack:
    return failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
  }
  return failure(ErrorCode::Internal);
}
inline Status final_check(Work& work, const IdentityRequestContext& context, TimePoint valid_until,
                          bool authorization_checkpoint = false) {
  auto sampled = work.transaction->current_time();
  if (const auto* error = std::get_if<Failure>(&sampled)) {
    return *error;
  }
  const auto now = std::get<TimePoint>(sampled);
  if (now < work.now) {
    return failure(ErrorCode::UpstreamUnavailable);
  }
  if (now >= context.deadline || now >= valid_until ||
      (authorization_checkpoint && work.now <= TimePoint::max() - std::chrono::seconds{1} &&
       now > work.now + std::chrono::seconds{1})) {
    return failure(ErrorCode::Timeout, Recovery::Backoff);
  }
  work.now = now;
  return std::monostate{};
}

inline Outcome<TimePoint> add_lifetime(TimePoint now, std::chrono::nanoseconds lifetime) {
  if (lifetime <= std::chrono::nanoseconds::zero() || now > TimePoint::max() - lifetime) {
    return failure(ErrorCode::Internal);
  }
  return now + lifetime;
}
inline Outcome<ResourceVersion> next_version(ResourceVersion version) {
  if (version.value == 0 || version.value == std::numeric_limits<std::uint64_t>::max()) {
    return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
  }
  return ResourceVersion{version.value + 1};
}

// Public identifiers only; reusable bearer token material is produced by RandomPort::opaque_token.
inline Outcome<std::string> random_uuid(RandomPort& random) {
  auto generated = random.bytes(16);
  if (const auto* error = std::get_if<Failure>(&generated)) {
    return *error;
  }
  const auto& material = std::get<OwnedSecret>(generated);
  if (!material || material->bytes().size() != 16) {
    return failure(ErrorCode::Internal);
  }
  std::array<std::byte, 16> bytes;
  std::copy(material->bytes().begin(), material->bytes().end(), bytes.begin());
  bytes[6] = (bytes[6] & std::byte{0x0f}) | std::byte{0x40};
  bytes[8] = (bytes[8] & std::byte{0x3f}) | std::byte{0x80};
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result.push_back('-');
    }
    const auto value = std::to_integer<unsigned>(bytes[index]);
    result.push_back(kHex[value >> 4]);
    result.push_back(kHex[value & 15]);
  }
  return result;
}

struct SubjectAuthority {
  Principal principal;
  AuthorizationState authorization;
  std::vector<ExactAssignment> assignments;
};
inline Outcome<SubjectAuthority> read_subject(IdentitySecurityUnitOfWork& work,
                                              const SubjectId& subject) {
  auto principal = work.principals().find(subject);
  if (const auto* error = std::get_if<Failure>(&principal)) {
    return *error;
  }
  const auto& found = std::get<std::optional<Principal>>(principal);
  if (!found) {
    return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  auto authorization = work.authorizations().find(subject);
  if (const auto* error = std::get_if<Failure>(&authorization)) {
    return *error;
  }
  const auto& current = std::get<std::optional<AuthorizationState>>(authorization);
  if (!current) {
    return failure(ErrorCode::Internal);
  }
  auto assignments = work.exact_assignments().for_subject(subject);
  if (const auto* error = std::get_if<Failure>(&assignments)) {
    return *error;
  }
  if (found->id != subject || current->subject != subject || current->version.value == 0) {
    return failure(ErrorCode::Internal);
  }
  const auto collection =
      validate_assignment_collection(std::get<std::vector<ExactAssignment>>(assignments), subject);
  if (std::holds_alternative<Failure>(collection)) {
    return failure(ErrorCode::Internal);
  }
  if (!known_principal_kind(found->kind) || found->version.value == 0) {
    return failure(ErrorCode::Internal);
  }
  for (const auto& assignment : std::get<std::vector<ExactAssignment>>(assignments)) {
    if (assignment.subject != subject) {
      return failure(ErrorCode::Internal);
    }
  }
  return SubjectAuthority{*found, *current,
                          std::move(std::get<std::vector<ExactAssignment>>(assignments))};
}
struct PeerAuthority {
  CredentialVersionRecord credential;
  SubjectAuthority subject;
};
inline Outcome<PeerAuthority> read_peer(IdentitySecurityUnitOfWork& work,
                                        const VerifiedPeer& peer) {
  auto registered = work.credentials().by_certificate(peer.binding.certificate.fingerprint);
  if (const auto* error = std::get_if<Failure>(&registered)) {
    return *error;
  }
  const auto& certificate = std::get<std::optional<CredentialVersionRecord>>(registered);
  if (!certificate) {
    return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  auto head = work.credentials().current(certificate->ref.id);
  if (const auto* error = std::get_if<Failure>(&head)) {
    return *error;
  }
  const auto& credential = std::get<std::optional<CredentialVersionRecord>>(head);
  if (!credential || credential->ref != certificate->ref) {
    return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  auto subject = read_subject(work, credential->subject);
  if (const auto* error = std::get_if<Failure>(&subject)) {
    return *error;
  }
  return PeerAuthority{*credential, std::move(std::get<SubjectAuthority>(subject))};
}
inline Status credential_owner_binding(const Principal& principal,
                                       const CredentialVersionRecord& credential);
// Reads current typed registration; never constructs TLS evidence or pins a certificate version.
inline Outcome<PeerAuthority> read_session_control(IdentitySecurityUnitOfWork& work,
                                                   const SessionSecurity& session, TimePoint now);
// Stable-ID match only; the caller must separately check_peer against actual current TLS.
inline Status require_session_control_peer(const SessionSecurity& session,
                                           const VerifiedPeer& direct) {
  const auto* platform = std::get_if<PlatformPeer>(&direct.binding.peer);
  if (!session.control_peer || !valid_session_control_peer(*session.control_peer) || !platform ||
      platform->service != Service::Control || session.control_peer->subject != platform->subject ||
      session.control_peer->subject != direct.binding.certificate.subject ||
      session.control_peer->credential != direct.binding.credential.id) {
    return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  return std::monostate{};
}
struct SessionAuthority {
  SessionSecurity session;
  CredentialVersionRecord credential;
  SubjectAuthority subject;
  PeerAuthority control;
};
inline Outcome<SessionAuthority> read_session(IdentitySecurityUnitOfWork& work, const SessionId& id,
                                              TimePoint now) {
  auto row = work.session_security().find(id);
  if (const auto* error = std::get_if<Failure>(&row)) {
    return *error;
  }
  const auto& session = std::get<std::optional<SessionSecurity>>(row);
  if (!session) {
    return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  auto base = work.sessions().find(id);
  if (const auto* error = std::get_if<Failure>(&base)) {
    return *error;
  }
  const auto& metadata = std::get<std::optional<LoginSession>>(base);
  if (!metadata || metadata->id != session->id || metadata->subject != session->subject ||
      metadata->version != session->version || metadata->rotation != session->rotation ||
      metadata->expires_at != session->refresh_expires_at ||
      metadata->revoked != session->revoked) {
    return failure(ErrorCode::Internal);
  }
  auto head = work.credentials().current(session->credential.id);
  if (const auto* error = std::get_if<Failure>(&head)) {
    return *error;
  }
  const auto& credential = std::get<std::optional<CredentialVersionRecord>>(head);
  if (!credential) {
    return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  auto subject = read_subject(work, session->subject);
  if (const auto* error = std::get_if<Failure>(&subject)) {
    return *error;
  }
  auto binding =
      credential_owner_binding(std::get<SubjectAuthority>(subject).principal, *credential);
  if (const auto* error = std::get_if<Failure>(&binding)) {
    return *error;
  }
  auto control = read_session_control(work, *session, now);
  if (const auto* error = std::get_if<Failure>(&control)) {
    return *error;
  }
  return SessionAuthority{*session, *credential, std::move(std::get<SubjectAuthority>(subject)),
                          std::move(std::get<PeerAuthority>(control))};
}

inline Status credential_owner_binding(const Principal& principal,
                                       const CredentialVersionRecord& credential) {
  if (principal.kind == PrincipalKind::Human) {
    return credential.binding == ResourceRef{ResourceKind::Principal, {principal.id.value}}
               ? Status{std::monostate{}}
               : Status{failure(ErrorCode::Internal)};
  }
  if (!principal.binding || credential.binding != *principal.binding) {
    return failure(ErrorCode::Internal);
  }
  return std::monostate{};
}

inline Status check_peer(const PeerAuthority& authority, const VerifiedPeer& peer, TimePoint now,
                         bool control_only = true) {
  auto owner = credential_owner_binding(authority.subject.principal, authority.credential);
  if (std::holds_alternative<Failure>(owner)) {
    return owner;
  }
  const auto current = require_current_credential(authority.subject.principal, authority.credential,
                                                  peer.binding.credential, now);
  if (std::holds_alternative<Failure>(current)) {
    return current;
  }
  const auto* certificate = std::get_if<CertificateBinding>(&authority.credential.material);
  const auto* platform = std::get_if<PlatformPeer>(&peer.binding.peer);
  if (!certificate || !platform || !valid_service(platform->service) ||
      (control_only && platform->service != Service::Control) ||
      authority.subject.principal.kind != PrincipalKind::PlatformService ||
      platform->subject != authority.subject.principal.id ||
      certificate->subject != platform->subject ||
      certificate->fingerprint != peer.binding.certificate.fingerprint ||
      certificate->registered_san != peer.binding.certificate.registered_san ||
      certificate->platform_service != platform->service ||
      peer.binding.certificate.platform_service != certificate->platform_service ||
      peer.binding.certificate.subject != certificate->subject ||
      peer.binding.certificate.not_before != certificate->not_before ||
      peer.binding.certificate.not_after != certificate->not_after) {
    return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  return std::monostate{};
}
inline Outcome<PeerAuthority> read_session_control(IdentitySecurityUnitOfWork& work,
                                                   const SessionSecurity& session, TimePoint now) {
  if (!session.control_peer || !valid_session_control_peer(*session.control_peer) ||
      !bounded_text(session.control_peer->subject.value) ||
      !bounded_text(session.control_peer->credential.value)) {
    return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  const auto& expected = *session.control_peer;
  auto current = work.credentials().current(expected.credential);
  if (const auto* error = std::get_if<Failure>(&current)) {
    return *error;
  }
  const auto& credential = std::get<std::optional<CredentialVersionRecord>>(current);
  if (!credential || credential->ref.id != expected.credential ||
      credential->subject != expected.subject) {
    return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  auto subject = read_subject(work, expected.subject);
  if (const auto* error = std::get_if<Failure>(&subject)) {
    return *error;
  }
  auto& authority = std::get<SubjectAuthority>(subject);
  auto owner = credential_owner_binding(authority.principal, *credential);
  if (const auto* error = std::get_if<Failure>(&owner)) {
    return *error;
  }
  auto gate = require_current_credential(authority.principal, *credential, credential->ref, now);
  if (const auto* error = std::get_if<Failure>(&gate)) {
    return *error;
  }
  const auto* certificate = std::get_if<CertificateBinding>(&credential->material);
  if (authority.principal.kind != PrincipalKind::PlatformService ||
      authority.principal.binding !=
          ResourceRef{ResourceKind::Principal, {expected.subject.value}} ||
      !certificate || certificate->subject != expected.subject ||
      certificate->platform_service != Service::Control ||
      !bounded_text(certificate->registered_san)) {
    return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  return PeerAuthority{*credential, std::move(authority)};
}
inline Outcome<SessionAuthority> read_session(IdentitySecurityUnitOfWork& work,
                                              const SessionId& id) {
  auto now = work.current_time();
  if (const auto* error = std::get_if<Failure>(&now)) {
    return *error;
  }
  return read_session(work, id, std::get<TimePoint>(now));
}
inline TimePoint session_control_valid_until(const PeerAuthority& control) {
  const auto* certificate = std::get_if<CertificateBinding>(&control.credential.material);
  return certificate ? std::min(control.credential.expires_at.value_or(TimePoint::min()),
                                certificate->not_after)
                     : TimePoint::min();
}
inline Outcome<std::string> normalized_login(std::string_view username) {
  if (username.empty() || username.size() > 128) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  std::string result;
  for (char character : username) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
    if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
          character == '_' || character == '.' || character == '-')) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    result.push_back(character);
  }
  return result;
}
inline Status context_valid(const IdentityRequestContext& context, bool mutation) {
  if (!bounded_text(context.request_id) || !bounded_text(context.trace_id) ||
      (mutation && !bounded_text(context.idempotency_key))) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return std::monostate{};
}
class LoginLimiter {
public:
  Status consume(const SubjectId& peer, const std::string& username,
                 std::chrono::steady_clock::time_point now) {
    std::lock_guard lock(mutex_);
    if (last_sample_ && now < *last_sample_) {
      return failure(ErrorCode::UpstreamUnavailable);
    }
    last_sample_ = now;
    for (auto iterator = windows_.begin(); iterator != windows_.end();) {
      if (iterator->second.start <=
              std::chrono::steady_clock::time_point::max() - std::chrono::minutes{1} &&
          now >= iterator->second.start + std::chrono::minutes{1}) {
        iterator = windows_.erase(iterator);
      } else {
        ++iterator;
      }
    }
    const auto peer_key = std::make_pair(peer.value, std::string{});
    const auto user_key = std::make_pair(std::string{}, username);
    const auto peer_found = windows_.find(peer_key);
    const auto user_found = windows_.find(user_key);
    const std::size_t needed =
        (peer_found == windows_.end() ? 1U : 0U) + (user_found == windows_.end() ? 1U : 0U);
    if (windows_.size() > 4096 - needed) {
      return failure(ErrorCode::RateLimited, Recovery::Backoff);
    }
    auto& peer_window = windows_.try_emplace(peer_key, Window{now, 0}).first->second;
    auto& user_window = windows_.try_emplace(user_key, Window{now, 0}).first->second;
    if (now < peer_window.start || now < user_window.start) {
      return failure(ErrorCode::UpstreamUnavailable);
    }
    const bool rejected = peer_window.count >= 60 || user_window.count >= 30;
    if (peer_window.count < 61) {
      ++peer_window.count;
    }
    if (user_window.count < 31) {
      ++user_window.count;
    }
    return rejected ? Status{failure(ErrorCode::RateLimited, Recovery::Backoff)}
                    : Status{std::monostate{}};
  }

private:
  struct Window {
    std::chrono::steady_clock::time_point start;
    unsigned count;
  };
  std::mutex mutex_;
  std::map<std::pair<std::string, std::string>, Window> windows_;
  std::optional<std::chrono::steady_clock::time_point> last_sample_;
};

struct PreparedTokens {
  EphemeralTokenPair pair;
  TokenVerifier access;
  TokenVerifier refresh;
};
inline Outcome<PreparedTokens> prepare_tokens(const IdentitySecurityDependencies& dependencies,
                                              const IdentitySecurityOptions& options,
                                              const SessionId& session, std::uint64_t rotation,
                                              TimePoint issued_at) {
  auto access_expiry = add_lifetime(issued_at, kAccessTokenLifetime);
  if (const auto* error = std::get_if<Failure>(&access_expiry)) {
    return *error;
  }
  auto refresh_expiry = add_lifetime(issued_at, kRefreshTokenLifetime);
  if (const auto* error = std::get_if<Failure>(&refresh_expiry)) {
    return *error;
  }
  auto access = dependencies.random.opaque_token();
  if (const auto* error = std::get_if<Failure>(&access)) {
    return *error;
  }
  auto refresh = dependencies.random.opaque_token();
  if (const auto* error = std::get_if<Failure>(&refresh)) {
    return *error;
  }
  auto access_secret = std::move(std::get<OwnedSecret>(access));
  auto refresh_secret = std::move(std::get<OwnedSecret>(refresh));
  if (!access_secret || !refresh_secret) {
    return failure(ErrorCode::Internal);
  }
  auto access_verifier = dependencies.tokens.make_verifier(access_secret->bytes(),
                                                           TokenPurpose::Access, options.token_key);
  if (const auto* error = std::get_if<Failure>(&access_verifier)) {
    return *error;
  }
  auto refresh_verifier = dependencies.tokens.make_verifier(
      refresh_secret->bytes(), TokenPurpose::Refresh, options.token_key);
  if (const auto* error = std::get_if<Failure>(&refresh_verifier)) {
    return *error;
  }
  return PreparedTokens{
      EphemeralTokenPair{session, std::move(access_secret), std::move(refresh_secret),
                         std::get<TimePoint>(access_expiry), std::get<TimePoint>(refresh_expiry),
                         rotation},
      std::get<TokenVerifier>(access_verifier), std::get<TokenVerifier>(refresh_verifier)};
}
inline Outcome<std::vector<TokenVerifier>>
token_candidates(const IdentitySecurityDependencies& dependencies,
                 const IdentitySecurityOptions& options, SecretView token, TokenPurpose purpose) {
  std::vector<TokenVerifier> result;
  result.reserve(options.token_verification_keys.size());
  for (const auto& key : options.token_verification_keys) {
    auto candidate = dependencies.tokens.make_verifier(token, purpose, key);
    if (const auto* error = std::get_if<Failure>(&candidate)) {
      return *error;
    }
    result.push_back(std::get<TokenVerifier>(candidate));
  }
  return result;
}
inline bool same_token(const TokenVerifier& first, const TokenVerifier& second) {
  return first.purpose == second.purpose && first.key == second.key && first.tag == second.tag;
}

struct SnapshotOwner {
  SubjectId actor;
  SubjectId target;
  CredentialVersionRef source_credential;
  std::optional<SessionId> source_session;
  ResourceVersion authorization_version;
  std::uint32_t page_size;
  bool operator==(const SnapshotOwner&) const = default;
};
struct Snapshot {
  SnapshotOwner owner;
  std::string id;
  std::vector<std::string> cursors;
  std::vector<SessionMetadata> records;
  TimePoint expires_at;
};
// Caller freshly authorizes the owner before every operation. No crypto or repository calls
// occur under this mutex. Valid snapshots are never evicted to make room for another caller.
class Snapshots {
public:
  explicit Snapshots(const IdentitySecurityOptions& options)
      : maximum_snapshots_(options.maximum_snapshots),
        maximum_records_(options.maximum_snapshot_records) {}
  Outcome<SessionPageReply> insert(Snapshot snapshot, TimePoint now) {
    std::lock_guard lock(mutex_);
    prune(now);
    if (snapshot.owner.page_size == 0 || snapshot.owner.page_size > 100 || snapshot.id.empty() ||
        snapshot.expires_at <= now || snapshot.cursors.empty() ||
        !snapshot.cursors.front().empty()) {
      return failure(ErrorCode::Internal);
    }
    const auto pages = snapshot.records.empty()
                           ? std::size_t{1}
                           : (snapshot.records.size() - 1) / snapshot.owner.page_size + 1;
    const auto maximum_expiry = add_lifetime(now, kSessionSnapshotLifetime);
    if (const auto* error = std::get_if<Failure>(&maximum_expiry)) {
      return *error;
    }
    if (snapshot.expires_at > std::get<TimePoint>(maximum_expiry)) {
      return failure(ErrorCode::Internal);
    }
    std::set<std::string> session_ids;
    for (const auto& record : snapshot.records) {
      if (record.subject != snapshot.owner.target || record.id.value.empty() ||
          !session_ids.insert(record.id.value).second) {
        return failure(ErrorCode::Internal);
      }
    }
    std::set<std::string> unique_cursors;
    for (const auto& cursor : snapshot.cursors) {
      if (!unique_cursors.insert(cursor).second) {
        return failure(ErrorCode::Internal);
      }
    }
    if (snapshot.cursors.size() != pages) {
      return failure(ErrorCode::Internal);
    }
    if (snapshots_.size() >= maximum_snapshots_ ||
        snapshot.records.size() > maximum_records_ - records_) {
      return failure(ErrorCode::RateLimited, Recovery::Backoff);
    }
    if (snapshots_.contains(snapshot.id)) {
      return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
    }
    for (std::size_t index = 1; index < snapshot.cursors.size(); ++index) {
      if (cursor_index_.contains(snapshot.cursors[index])) {
        return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
      }
    }
    auto result = page(snapshot, 0, now);
    const auto count = snapshot.records.size();
    const auto id = snapshot.id;
    auto stored = snapshots_.emplace(id, std::move(snapshot)).first;
    try {
      for (std::size_t index = 1; index < stored->second.cursors.size(); ++index) {
        cursor_index_.emplace(stored->second.cursors[index], std::make_pair(id, index));
      }
    } catch (...) {
      for (std::size_t index = 1; index < stored->second.cursors.size(); ++index) {
        cursor_index_.erase(stored->second.cursors[index]);
      }
      snapshots_.erase(stored);
      throw;
    }
    records_ += count;
    return result;
  }
  Outcome<SessionPageReply> read(const SnapshotOwner& owner, const SessionPageRequest& requested,
                                 TimePoint now) {
    std::lock_guard lock(mutex_);
    prune(now);
    std::string id = requested.snapshot_id;
    std::size_t index = 0;
    if (!requested.page_token.empty()) {
      const auto cursor = cursor_index_.find(requested.page_token);
      if (cursor == cursor_index_.end()) {
        return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
      if (!id.empty() && id != cursor->second.first) {
        return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
      id = cursor->second.first;
      index = cursor->second.second;
    }
    const auto found = snapshots_.find(id);
    if (found == snapshots_.end() || found->second.owner != owner) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    return page(found->second, index, now);
  }

private:
  static SessionPageReply page(const Snapshot& snapshot, std::size_t index, TimePoint now) {
    const auto begin = index * snapshot.owner.page_size;
    const auto end = std::min(snapshot.records.size(), begin + snapshot.owner.page_size);
    return {
        std::vector<SessionMetadata>(snapshot.records.begin() + static_cast<std::ptrdiff_t>(begin),
                                     snapshot.records.begin() + static_cast<std::ptrdiff_t>(end)),
        snapshot.owner.page_size,
        snapshot.id,
        index + 1 < snapshot.cursors.size() ? snapshot.cursors[index + 1] : std::string{},
        snapshot.expires_at,
        snapshot.owner.authorization_version,
        now};
  }
  void prune(TimePoint now) {
    for (auto iterator = snapshots_.begin(); iterator != snapshots_.end();) {
      if (iterator->second.expires_at <= now) {
        for (std::size_t index = 1; index < iterator->second.cursors.size(); ++index) {
          cursor_index_.erase(iterator->second.cursors[index]);
        }
        records_ -= iterator->second.records.size();
        iterator = snapshots_.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }
  const std::size_t maximum_snapshots_;
  const std::size_t maximum_records_;
  std::mutex mutex_;
  std::map<std::string, Snapshot> snapshots_;
  // At most one entry per continuation page, globally bounded by maximum_records_.
  std::map<std::string, std::pair<std::string, std::size_t>> cursor_index_;
  std::size_t records_ = 0;
};

} // namespace kiarabot::core::target::identity_security_detail
