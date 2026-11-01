// clang-format off
#include "kiarabot/infrastructure/identity_crypto/factories.hpp"

#include <cassert>
#include <string_view>
#include <variant>
// clang-format on

namespace crypto = kiarabot::infrastructure::identity_crypto;
namespace target = kiarabot::core::target;

int main() try {
  constexpr std::string_view kPassword = "test-password-only";
  const auto view = std::as_bytes(std::span{kPassword.data(), kPassword.size()});
  auto factory = crypto::make_password_crypto();
  auto& password = std::get<std::unique_ptr<target::PasswordCryptoPort>>(factory);
  auto derived = password->derive(view);
  auto verifier = std::get<target::PasswordVerifier>(derived);
  assert(std::get<bool>(password->verify(view, verifier)));
  assert(!std::get<bool>(password->verify(view.first(3), verifier)));
  auto malformed = verifier;
  // A 16-byte salt has only two significant bits in its last base64 character.
  malformed.encoded[malformed.encoded.find_last_of('$') - 1] = 'B';
  auto rejected = password->verify(view, malformed);
  assert(std::get<target::Failure>(rejected).code == target::ErrorCode::Internal);
  malformed = verifier;
  malformed.encoded.back() = 'B';
  assert(std::get<target::Failure>(password->verify(view, malformed)).code ==
         target::ErrorCode::Internal);
  verifier.encoded.replace(verifier.encoded.find("65536"), 5, "99999");
  assert(std::holds_alternative<target::Failure>(password->verify(view, verifier)));
  auto random_factory = crypto::make_random();
  auto& random = std::get<std::unique_ptr<target::RandomPort>>(random_factory);
  auto first = random->bytes(32);
  auto second = random->bytes(32);
  assert(std::get<target::OwnedSecret>(first)->bytes().size() == 32);
  assert(std::get<target::OwnedSecret>(second)->bytes().size() == 32);
  assert(std::holds_alternative<target::Failure>(random->bytes(65537)));
  auto token_result = random->opaque_token();
  auto other_result = random->opaque_token();
  const auto token = std::get<target::OwnedSecret>(token_result)->bytes();
  const auto other = std::get<target::OwnedSecret>(other_result)->bytes();
  assert(token.size() == 64 && other.size() == 64);
  bool different = false;
  for (std::size_t index = 0; index < token.size(); ++index) {
    const auto value = std::to_integer<unsigned char>(token[index]);
    assert((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'));
    assert(value != 0);
    different = different || token[index] != other[index];
  }
  assert(different);
  auto copy = crypto::copy_secret(view);
  assert(std::get<target::OwnedSecret>(copy)->bytes().size() == view.size());
} catch (...) {
  return 1;
}
