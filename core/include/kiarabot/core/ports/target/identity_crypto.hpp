#pragma once

#include <cstddef>
#include <span>

#include "kiarabot/core/domain/target/identity_types.hpp"

namespace kiarabot::core::target {

// No algorithms in Core. Mature libsodium/jwt-cpp adapters use trusted configuration/key refs.
// No password/signing/key-provider calls while holding a UoW. Outcome<bool>{false} is a verified
// rejection; adapter dependency/corruption faults are Failure, never false or successful fallback.
class PasswordCryptoPort {
public:
  virtual ~PasswordCryptoPort() = default;
  virtual Outcome<PasswordVerifier> derive(SecretView password) = 0;
  virtual Outcome<bool> verify(SecretView password, const PasswordVerifier& stored) = 0;
};
class TokenCryptoPort {
public:
  virtual ~TokenCryptoPort() = default;
  // Use purpose-separated keyed verification (HMAC adapter), not raw secret/password SHA.
  virtual Outcome<TokenVerifier> make_verifier(SecretView token, TokenPurpose purpose,
                                               const CryptoKeyRef& key) = 0;
  virtual Outcome<bool> verify(SecretView token, const TokenVerifier& stored) = 0;
  // Canonical binding of typed scope, parameters and secret in the mature crypto adapter.
  // Mutable parameters are request CONTENT, not uniqueness scope. RotateCredential has no client
  // secret, but its expected_version still MUST participate in the keyed commitment.
  virtual Outcome<RequestCommitment> commit_request(const SecretReceiptScope& scope,
                                                    const SecretRequestParameters& parameters,
                                                    SecretView secret, const CryptoKeyRef& key) = 0;
};
class SecretReceiptCryptoPort {
public:
  virtual ~SecretReceiptCryptoPort() = default;
  // AEAD authenticates ALL binding fields, including request commitment/credential/rotations
  // and created/retention timestamps; changing a row cannot extend secret receipt lifetime.
  // Caller must validate current session/credential before EVERY open/replay. No plaintext store.
  virtual Outcome<SealedSecret> seal(const SecretReplayPayload& plaintext,
                                     const SecretReceiptBinding& binding,
                                     const CryptoKeyRef& key) = 0;
  virtual Outcome<SecretReplayPayload> open(const SealedSecret& sealed,
                                            const SecretReceiptBinding& binding) = 0;
};
class RandomPort {
public:
  virtual ~RandomPort() = default;
  // CSPRNG only, bounded count from trusted application policy; no weak fallback on failure.
  virtual Outcome<OwnedSecret> bytes(std::size_t count) = 0;
  // Exactly 32 bytes of CSPRNG entropy encoded as 64 lowercase hexadecimal ASCII characters.
  // Secure allocation and mature encoding remain in the adapter; the view contains no NUL.
  // Use for string bearer/bootstrap credentials, not raw bytes or a Core-owned wiping buffer.
  virtual Outcome<OwnedSecret> opaque_token() = 0;
};
class SecurityClockPort {
public:
  virtual ~SecurityClockPort() = default;
  // Do not claim trusted/low-skew without deployment evidence; not a replacement for DB UTC.
  virtual Outcome<SecurityClockSample> sample() = 0;
};
class PeerCertificatePort {
public:
  virtual ~PeerCertificatePort() = default;
  // presented_chain is real transport evidence, NEVER a request-body certificate/hash.
  // Framing: leaf-first concatenated DER X509 certificates, consumed completely by mature
  // d2i_X509. Nonempty, <=64KiB total, <=8 certs, no custom prefix or trailing bytes.
  // The adapter uses mature OpenSSL DER/X509 parsing; Core performs no ASN.1/certificate parsing.
  // RPC adapter may convert AuthContext's real PEM chain with OpenSSL; no supplied cert is a CA
  // trust anchor unless it is independently in configured trust roots.
  // Verify chain/time/SAN/service identity against the caller's authoritative expected binding.
  // Caller has read current credential/state; this crypto adapter does not query the database.
  virtual Outcome<VerifiedPeer> verify(std::span<const std::byte> presented_chain,
                                       const RegisteredPeer& expected, TimePoint now) = 0;
};
class DelegationCryptoPort {
public:
  virtual ~DelegationCryptoPort() = default;
  // Fixed Ed25519 JWS; mature jwt-cpp/libsodium adapters, no algorithm choice from token input.
  // MUST sign source_credential(id+version) and optional source_session; missing source rejects.
  virtual Outcome<OwnedSecret> sign(const DelegationClaims& claims, const CryptoKeyRef& key) = 0;
  // Reject none/downgrade/unknown key/arbitrary jku-x5u. Check iss/aud/sub/iat/nbf/exp/jti,
  // permission/resource bindings, actor chain, certificate, signed source_credential/source_session
  // and 56+2+2<=60s total validity. No implicit origin from authversion; online Identity checks the
  // actual origin session/current credential, and exchange must retain origin without widening.
  virtual Outcome<VerifiedDelegation> verify(SecretView proof, Service audience,
                                             const VerifiedPeer& direct_peer,
                                             const SecurityClockSample& clock) = 0;
  // Exchange receives a proof addressed TO this authenticated platform recipient. The parent
  // binds the previous sender's certificate, not the recipient's; do not compare those leaves
  // or pretend the recipient possesses the sender's key. Verify the same closed JWT/signature,
  // time/issuer/key rules and parent audience == recipient service (Runtime, first hop only).
  // Core must additionally verify current recipient AND previous sender registration, the
  // original credential/session/authorization, exact narrowing and unchanged origin before
  // signing hop two. Cryptographic parent verification alone never grants exchange authority.
  virtual Outcome<VerifiedDelegation> verify_exchange_parent(SecretView proof,
                                                             const VerifiedPeer& recipient,
                                                             const SecurityClockSample& clock) = 0;
};

} // namespace kiarabot::core::target
