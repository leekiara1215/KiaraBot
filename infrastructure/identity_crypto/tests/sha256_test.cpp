#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include <openssl/evp.h>

#include "kiarabot/infrastructure/identity_crypto/sha256.hpp"

namespace {
namespace t = kiarabot::core::target;
void require(bool condition) {
  if (!condition) {
    throw std::runtime_error("SHA-256 compatibility/failure assertion failed");
  }
}
std::string hex(const t::Sha256Digest& digest) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : digest.bytes) {
    const auto value = static_cast<unsigned char>(byte);
    result.push_back(kDigits[value >> 4U]);
    result.push_back(kDigits[value & 15U]);
  }
  return result;
}
} // namespace

int main() {
  kiarabot::infrastructure::identity_crypto::Sha256 adapter;
  const auto check = [&adapter](std::string_view input, std::string_view expected) {
    const auto result = adapter.sha256(input);
    require(std::holds_alternative<t::Sha256Digest>(result));
    require(hex(std::get<t::Sha256Digest>(result)) == expected);
  };
  // Fixed SHA-256 vectors, including byte strings that must not undergo text normalization.
  check(std::string_view{}, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  check("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  check(std::string_view("a\0b\xff", 4),
        "a37cc3026aae4d519e0b19c298fa913b4dccfdf0658cbccbb7deaa0226d5acdb");
  check(std::string(65536, 'x'),
        "1f8745f0d2d1387ec1af2211a3cf417b2e9e885e853472649c1d979d0e9370e3");
  // This test process alone disables digest lookup, then restores it. A failed mature-library
  // call must return the existing stable dependency error, never zero bytes or a success fallback.
  require(EVP_set_default_properties(nullptr, "provider=kiarabot_missing") == 1);
  const auto failed = adapter.sha256("abc");
  require(EVP_set_default_properties(nullptr, "") == 1);
  require(std::holds_alternative<t::Failure>(failed));
  const auto& error = std::get<t::Failure>(failed);
  require(error.code == t::ErrorCode::UpstreamUnavailable);
  require(error.acceptance == t::Acceptance::NotAccepted);
  require(error.recovery == t::Recovery::Backoff);
  check("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
