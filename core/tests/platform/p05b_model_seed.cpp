// P05b-4 test-only Model provider seeding (fixture driver, not a production write path).
//
// P05b isolates the plugin-to-provider chain; p14_native_single_host covers Control writes.
// This helper only performs the production crypto sealing (root-key XChaCha20-Poly1305 with the
// frozen AAD) and prints nonce/ciphertext as hex; the e2e applies plain INSERTs to the owned Model
// PG database.
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <openssl/rand.h>

#include "kiarabot/core/domain/target/model_gateway.hpp"
#include "kiarabot/infrastructure/identity_crypto/factories.hpp"
#include "kiarabot/infrastructure/identity_crypto/model_secret_crypto.hpp"

namespace crypto = kiarabot::infrastructure::identity_crypto;
namespace t = kiarabot::core::target;

namespace {
std::string read_line(const std::string& path) {
  std::ifstream input(path);
  std::string value;
  std::getline(input, value);
  return value;
}
std::vector<std::byte> parse_hex(const std::string& text) {
  if (text.size() != 64) {
    throw std::runtime_error("root key must be 64 hex characters");
  }
  std::vector<std::byte> bytes(32);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>(std::stoul(text.substr(index * 2, 2), nullptr, 16));
  }
  return bytes;
}
std::string to_hex(std::span<const std::byte> bytes) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    const auto value = std::to_integer<unsigned int>(byte);
    text.push_back(kHex[value >> 4U]);
    text.push_back(kHex[value & 0x0fU]);
  }
  return text;
}
} // namespace

int main(int argc, char* argv[]) {
  try {
    if (argc != 7) {
      return 2;
    }
    const t::ResourceId model_id{argv[1]};
    const std::string secret{argv[2]};
    const std::filesystem::path root_key_file{argv[3]};
    const std::uint64_t root_key_version = std::stoull(argv[4]);
    (void)argv[5];
    (void)argv[6];
    const auto key_bytes = parse_hex(read_line(root_key_file.string()));
    auto material = crypto::copy_secret(t::SecretView{std::span<const std::byte>(key_bytes)});
    if (const auto* rejected = std::get_if<t::Failure>(&material)) {
      return static_cast<int>(rejected->code);
    }
    crypto::SymmetricKey key{t::CryptoKeyRef{"model-root-v1"},
                             std::move(std::get<t::OwnedSecret>(material))};
    auto crypto_port = crypto::make_model_secret_crypto(std::move(key));
    if (const auto* rejected = std::get_if<t::Failure>(&crypto_port)) {
      return static_cast<int>(rejected->code);
    }
    std::array<std::byte, 24> nonce{};
    if (RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()),
                   static_cast<int>(nonce.size())) != 1) {
      return 1;
    }
    const std::span<const std::byte> plaintext{reinterpret_cast<const std::byte*>(secret.data()),
                                               secret.size()};
    const auto aad = t::model_secret_aad(model_id, t::ResourceVersion{1}, root_key_version);
    auto sealed = std::get<std::unique_ptr<t::ModelSecretCryptoPort>>(crypto_port)
                      ->seal(t::SecretView{plaintext}, aad, nonce);
    if (const auto* rejected = std::get_if<t::Failure>(&sealed)) {
      return static_cast<int>(rejected->code);
    }
    const auto& ciphertext = std::get<std::vector<std::byte>>(sealed);
    std::cout << "{\"nonce\":\"" << to_hex(nonce) << "\",\"sealed\":\"" << to_hex(ciphertext)
              << "\"}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "p05b model seed failed: " << error.what() << '\n';
    return 1;
  }
}
