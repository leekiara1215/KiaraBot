#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/core/domain/target/identity_permissions.hpp"
#include "kiarabot/core/domain/target/identity_types.hpp"
#include "kiarabot/core/domain/target/model_gateway.hpp"

namespace kiarabot::core::target {

// Caller identity accepted by the Model application. The RPC boundary fills the transport evidence
// and delegation bytes; the authorization port (or its fake in P11a tests) is the only authority
// allowed to turn those into a decision. `owner` is the plugin instance represented by a Chat
// delegation and is never accepted from request fields.
struct ModelCaller {
  std::optional<SubjectId> subject;
  std::optional<InstanceId> owner;
  std::string delegation;
  Sha256Digest peer_fingerprint;
};

struct ModelAuthorizationDecision {
  ResourceVersion authorization_version{1};
};

class ModelAuthorizationPort {
public:
  virtual ~ModelAuthorizationPort() = default;
  // Exact current-authority check for one business resource and closed permission. Fail closed:
  // Implementation faults are UpstreamUnavailable, never implicit allow. The port is called
  // outside any UnitOfWork.
  virtual Outcome<ModelAuthorizationDecision> authorize(const ModelCaller& caller,
                                                        Permission permission,
                                                        const ResourceRef& resource,
                                                        std::chrono::milliseconds budget) = 0;
};

// Provider transport abstraction. P11a injects a deterministic fake; P11b replaces it with the
// reviewed HTTP adapter. The implementation must not retain the borrowed secret and must return a
// stable failure code only (no provider text/details).
class ModelProviderPort {
public:
  virtual ~ModelProviderPort() = default;
  virtual ModelProviderResult chat(const ModelProviderRequest& request) = 0;
};

// Deployment-injected root-key AEAD adapter boundary. Plaintext exists only in process memory and
// is erased after use; the sealed value and root-key version are the only persisted material.
class ModelSecretCryptoPort {
public:
  virtual ~ModelSecretCryptoPort() = default;
  virtual Outcome<std::vector<std::byte>> seal(SecretView plaintext, std::span<const std::byte> aad,
                                               std::array<std::byte, 24>& nonce) = 0;
  virtual Outcome<OwnedSecret> open(std::span<const std::byte> sealed,
                                    std::span<const std::byte> aad,
                                    const std::array<std::byte, 24>& nonce) = 0;
};

// Stable unpredictable identifiers for management operations. Trusted application code only.
class ModelIdPort {
public:
  virtual ~ModelIdPort() = default;
  virtual Outcome<std::string> uuid() = 0;
};

// Infrastructure-owned SHA-256 for the canonical request fingerprint; the domain owns the bytes.
class ModelDigestPort {
public:
  virtual ~ModelDigestPort() = default;
  virtual Outcome<Sha256Digest> sha256(std::span<const std::byte> bytes) = 0;
};

} // namespace kiarabot::core::target
