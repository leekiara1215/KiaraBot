#pragma once

#include <string>
#include <vector>

#include "kiarabot/core/domain/target/identity_types.hpp"

namespace kiarabot::infrastructure::postgres::test {
struct ControlTlsEvidence {
  std::string root_pem;
  std::vector<std::byte> leaf_der;
  core::target::CertificateBinding registration;
};
// Ephemeral CA/leaf keys and a real two-sided TLS1.3 handshake over an OpenSSL BIO pair.
// Evidence comes from SSL_get1_peer_certificate on the authenticated server endpoint.
ControlTlsEvidence control_tls_evidence(const core::target::SubjectId& subject);
} // namespace kiarabot::infrastructure::postgres::test
