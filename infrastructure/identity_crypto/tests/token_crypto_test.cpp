#include <cassert>
#include <variant>

#include "kiarabot/infrastructure/identity_crypto/factories.hpp"

namespace crypto = kiarabot::infrastructure::identity_crypto;
namespace target = kiarabot::core::target;
int main() {
  std::array<std::byte, 32> key_bytes{};
  auto key = crypto::copy_secret(key_bytes);
  std::vector<crypto::SymmetricKey> keys;
  keys.push_back({{"test-key"}, std::move(std::get<target::OwnedSecret>(key))});
  auto factory = crypto::make_token_crypto(std::move(keys));
  auto& port = std::get<std::unique_ptr<target::TokenCryptoPort>>(factory);
  auto access = port->make_verifier(key_bytes, target::TokenPurpose::Access, {"test-key"});
  auto verifier = std::get<target::TokenVerifier>(access);
  assert(std::get<bool>(port->verify(key_bytes, verifier)));
  verifier.purpose = target::TokenPurpose::Refresh;
  assert(!std::get<bool>(port->verify(key_bytes, verifier)));
  verifier.key.value = "unknown";
  assert(std::holds_alternative<target::Failure>(port->verify(key_bytes, verifier)));
  target::SecretReceiptScope scope = target::RefreshReceiptScope{{"peer"}, {"session"}, "key"};
  auto first =
      port->commit_request(scope, target::RefreshRequestParameters{1}, key_bytes, {"test-key"});
  auto second =
      port->commit_request(scope, target::RefreshRequestParameters{2}, key_bytes, {"test-key"});
  assert(std::get<target::RequestCommitment>(first) != std::get<target::RequestCommitment>(second));
  assert(std::holds_alternative<target::Failure>(
      port->commit_request(scope, target::LoginRequestParameters{}, key_bytes, {"test-key"})));
  auto changed = key_bytes;
  changed[0] = std::byte{1};
  auto third =
      port->commit_request(scope, target::RefreshRequestParameters{1}, changed, {"test-key"});
  assert(std::get<target::RequestCommitment>(first) != std::get<target::RequestCommitment>(third));
  assert(std::holds_alternative<target::Failure>(crypto::make_token_crypto({})));
}
