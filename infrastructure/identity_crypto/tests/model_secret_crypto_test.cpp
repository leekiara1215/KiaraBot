#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/model_gateway.hpp"
#include "kiarabot/infrastructure/identity_crypto/factories.hpp"
#include "kiarabot/infrastructure/identity_crypto/model_secret_crypto.hpp"

namespace {
namespace t = kiarabot::core::target;
namespace crypto = kiarabot::infrastructure::identity_crypto;

crypto::SymmetricKey root_key(std::string id, std::byte seed) {
  std::array<std::byte, 32> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>(
        (static_cast<unsigned char>(seed) + static_cast<unsigned char>(index)) & 0xffU);
  }
  auto copied = crypto::copy_secret(std::as_bytes(std::span{bytes}));
  assert(std::holds_alternative<t::OwnedSecret>(copied));
  return crypto::SymmetricKey{{std::move(id)}, std::move(std::get<t::OwnedSecret>(copied))};
}
std::vector<std::byte> plaintext(std::string_view value) {
  std::vector<std::byte> result(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    result[index] = static_cast<std::byte>(static_cast<unsigned char>(value[index]));
  }
  return result;
}
bool same(t::SecretView left, t::SecretView right) {
  return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}
} // namespace

int main() {
  using namespace std::chrono_literals;
  const t::ResourceId model{"11111111-1111-4111-8111-111111111111"};
  const auto aad_v1 = t::model_secret_aad(model, {3}, 7);
  const auto aad_other_version = t::model_secret_aad(model, {4}, 7);
  const auto aad_other_root = t::model_secret_aad(model, {3}, 8);
  const auto secret = plaintext("provider-api-key-123");
  try {
    auto opened = crypto::make_model_secret_crypto(root_key("root-v1", std::byte{1}));
    assert(std::holds_alternative<std::unique_ptr<t::ModelSecretCryptoPort>>(opened));
    auto& adapter = *std::get<std::unique_ptr<t::ModelSecretCryptoPort>>(opened);
    std::array<std::byte, 24> nonce{};
    const auto sealed = adapter.seal(secret, aad_v1, nonce);
    assert(std::holds_alternative<std::vector<std::byte>>(sealed));
    const auto& ciphertext = std::get<std::vector<std::byte>>(sealed);
    assert(ciphertext.size() == secret.size() + 16);
    // AAD binds model id, secret version and root-key version.
    auto decrypted = adapter.open(ciphertext, aad_v1, nonce);
    assert(std::holds_alternative<t::OwnedSecret>(decrypted));
    assert(same(std::get<t::OwnedSecret>(decrypted)->bytes(), secret));
    assert(std::holds_alternative<t::Failure>(adapter.open(ciphertext, aad_other_version, nonce)));
    assert(std::holds_alternative<t::Failure>(adapter.open(ciphertext, aad_other_root, nonce)));
    // Tampering is rejected, not silently truncated.
    auto tampered = ciphertext;
    tampered.back() ^= std::byte{0x01};
    assert(std::holds_alternative<t::Failure>(adapter.open(tampered, aad_v1, nonce)));
    // Nonces are random per seal and do not repeat.
    std::array<std::byte, 24> second_nonce{};
    const auto second = adapter.seal(secret, aad_v1, second_nonce);
    assert(std::holds_alternative<std::vector<std::byte>>(second));
    assert(nonce != second_nonce);
    // A different deployment root key cannot open the sealed value.
    auto other = crypto::make_model_secret_crypto(root_key("root-v2", std::byte{9}));
    assert(std::holds_alternative<std::unique_ptr<t::ModelSecretCryptoPort>>(other));
    assert(std::holds_alternative<t::Failure>(
        std::get<std::unique_ptr<t::ModelSecretCryptoPort>>(other)->open(ciphertext, aad_v1,
                                                                         nonce)));
    // Input bounds fail closed.
    std::vector<std::byte> empty;
    std::array<std::byte, 24> unused{};
    assert(std::holds_alternative<t::Failure>(adapter.seal(empty, aad_v1, unused)));
    assert(std::holds_alternative<t::Failure>(adapter.open({}, aad_v1, nonce)));

    std::cout << "model secret crypto: ok\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
