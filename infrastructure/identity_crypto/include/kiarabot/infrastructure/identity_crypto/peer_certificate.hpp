#pragma once

#include <memory>
#include <string>
#include <vector>

#include "kiarabot/core/ports/target/identity_crypto.hpp"

namespace kiarabot::infrastructure::identity_crypto {

enum class PeerSanKind { Uri, Dns };
// Trusted deployment input only; no certificate/key URLs or caller-supplied trust roots.
// Exactly one SAN of the selected type must equal the authoritative registered SAN.
struct PeerTrustConfig {
  std::vector<std::string> trust_roots_pem;
  PeerSanKind san_kind = PeerSanKind::Uri;
};
// Input chain is leaf-first concatenated DER, at most eight certificates and 64KiB.
// Caller obtains it from transport authentication evidence, and supplies current registration.
core::target::Outcome<std::unique_ptr<core::target::PeerCertificatePort>>
make_peer_certificate_crypto(PeerTrustConfig config);

} // namespace kiarabot::infrastructure::identity_crypto
