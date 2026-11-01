#pragma once

#include <vector>

#include <grpcpp/security/auth_context.h>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::platform {

struct PeerCertificateEvidence {
  target::Sha256Digest leaf_fingerprint;
  // Leaf first, concatenated DER X509, at most 8 certificates / 64 KiB.
  std::vector<std::byte> der_chain;
};

// Only use the real ServerContext::auth_context(), never caller-supplied metadata or JSON.
// Extraction proves transport evidence only; current registration/credential/SAN authorization
// must still be checked by Identity and PeerCertificatePort before constructing VerifiedPeer.
target::Outcome<PeerCertificateEvidence> extract_peer_certificate(const grpc::AuthContext& context);

} // namespace kiarabot::core::platform
