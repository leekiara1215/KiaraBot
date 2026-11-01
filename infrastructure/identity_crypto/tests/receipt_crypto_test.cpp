// clang-format off
#include "kiarabot/infrastructure/identity_crypto/factories.hpp"

#include <cassert>
#include <variant>

#include <sodium.h>
// clang-format on

namespace crypto = kiarabot::infrastructure::identity_crypto;
namespace t = kiarabot::core::target;
t::OwnedSecret secret() {
  std::array<std::byte, 32> value{};
  return std::move(std::get<t::OwnedSecret>(crypto::copy_secret(value)));
}
// Independent test encoder for the public credential-delivery AAD shape. Malformed plaintext
// contains no real secret and is encrypted with the real AEAD to exercise authenticated decoding.
std::vector<std::byte> delivery_aad(const t::SecretReceiptBinding& b) {
  std::vector<std::byte> encoded;
  auto integer = [&](std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
      encoded.push_back(std::byte{static_cast<unsigned char>(value >> (56 - index * 8))});
    }
  };
  auto field = [&](t::SecretView value) {
    integer(value.size());
    encoded.insert(encoded.end(), value.begin(), value.end());
  };
  auto text = [&](std::string_view value) {
    field(std::as_bytes(std::span{value.data(), value.size()}));
  };
  text("kiarabot.identity.receipt.v1");
  text("receipt-key");
  integer(1);
  integer(2);
  const auto& scope = std::get<t::CredentialDeliveryScope>(b.scope);
  text(scope.actor.value);
  text(scope.credential.value);
  text(scope.idempotency_key);
  text(b.subject.value);
  integer(0);
  text(b.credential.id.value);
  integer(b.credential.version.value);
  integer(2);
  integer(std::get<t::CredentialDeliveryParameters>(b.parameters).expected_version.value);
  integer(b.result_rotation);
  text(b.request.key.value);
  field(b.request.tag.bytes);
  integer(static_cast<std::uint64_t>(b.created_at.time_since_epoch().count()));
  integer(static_cast<std::uint64_t>(b.retain_until.time_since_epoch().count()));
  return encoded;
}
int main() try {
  std::vector<crypto::SymmetricKey> keys;
  keys.push_back({{"receipt-key"}, secret()});
  auto factory = crypto::make_secret_receipt_crypto(std::move(keys));
  auto& port = std::get<std::unique_ptr<t::SecretReceiptCryptoPort>>(factory);
  const t::TimePoint now{std::chrono::seconds{100}};
  t::SecretReceiptBinding binding{t::RefreshReceiptScope{{"peer"}, {"session"}, "key"},
                                  {"subject"},
                                  t::SessionId{"session"},
                                  {{"credential"}, {1}},
                                  t::RefreshRequestParameters{1},
                                  2,
                                  {{"commit-key"}, {}},
                                  now,
                                  now + t::kSecretReceiptRetention};
  t::SecretReplayPayload payload = t::EphemeralTokenPair{{"session"},
                                                         secret(),
                                                         secret(),
                                                         now + t::kAccessTokenLifetime,
                                                         now + t::kRefreshTokenLifetime,
                                                         2};
  auto sealed_result = port->seal(payload, binding, {"receipt-key"});
  auto sealed = std::get<t::SealedSecret>(sealed_result);
  auto opened = port->open(sealed, binding);
  assert(std::holds_alternative<t::SecretReplayPayload>(opened));
  const auto& pair = std::get<t::EphemeralTokenPair>(std::get<t::SecretReplayPayload>(opened));
  assert(pair.rotation == 2 && pair.access->bytes().size() == 32);
  using Mutation = void (*)(t::SecretReceiptBinding&);
  const std::vector<Mutation> mutations{
      [](t::SecretReceiptBinding& b) {
        std::get<t::RefreshReceiptScope>(b.scope).control_peer.value += "x";
      },
      [](t::SecretReceiptBinding& b) {
        std::get<t::RefreshReceiptScope>(b.scope).idempotency_key += "x";
      },
      [](t::SecretReceiptBinding& b) { b.subject.value += "x"; },
      [](t::SecretReceiptBinding& b) { b.session->value += "x"; },
      [](t::SecretReceiptBinding& b) { b.credential.id.value += "x"; },
      [](t::SecretReceiptBinding& b) { ++b.credential.version.value; },
      [](t::SecretReceiptBinding& b) {
        ++std::get<t::RefreshRequestParameters>(b.parameters).input_rotation;
      },
      [](t::SecretReceiptBinding& b) { ++b.result_rotation; },
      [](t::SecretReceiptBinding& b) { b.request.key.value += "x"; },
      [](t::SecretReceiptBinding& b) { b.request.tag.bytes[0] = std::byte{1}; },
      [](t::SecretReceiptBinding& b) { b.created_at += std::chrono::seconds{1}; },
      [](t::SecretReceiptBinding& b) { b.retain_until -= std::chrono::seconds{1}; }};
  for (const auto& mutate : mutations) {
    auto changed = binding;
    mutate(changed);
    assert(std::holds_alternative<t::Failure>(port->open(sealed, changed)));
  }
  auto tampered = sealed;
  tampered.ciphertext[0] ^= std::byte{1};
  assert(std::holds_alternative<t::Failure>(port->open(tampered, binding)));
  tampered = sealed;
  tampered.nonce[0] ^= std::byte{1};
  assert(std::holds_alternative<t::Failure>(port->open(tampered, binding)));
  tampered = sealed;
  ++tampered.format_version;
  assert(std::holds_alternative<t::Failure>(port->open(tampered, binding)));
  tampered = sealed;
  tampered.key.value = "unknown";
  assert(std::holds_alternative<t::Failure>(port->open(tampered, binding)));
  auto invalid = binding;
  invalid.retain_until += std::chrono::seconds{1};
  assert(std::holds_alternative<t::Failure>(port->seal(payload, invalid, {"receipt-key"})));
  t::SecretReplayPayload wrong =
      t::EphemeralCredentialDelivery{{{"credential"}, {1}}, secret(), now + std::chrono::hours{1}};
  assert(std::holds_alternative<t::Failure>(port->seal(wrong, binding, {"receipt-key"})));
  binding.scope = t::CredentialDeliveryScope{{"actor"}, {"credential"}, "delivery"};
  binding.parameters = t::CredentialDeliveryParameters{{1}};
  binding.session.reset();
  auto delivery = port->seal(wrong, binding, {"receipt-key"});
  assert(std::holds_alternative<t::SealedSecret>(delivery));
  assert(std::holds_alternative<t::SecretReplayPayload>(
      port->open(std::get<t::SealedSecret>(delivery), binding)));
  const auto aad = delivery_aad(binding);
  std::array<unsigned char, 32> zero_key{};
  auto authenticated = [&](const std::vector<unsigned char>& plaintext) {
    t::SealedSecret forged{{"receipt-key"},
                           1,
                           std::vector<std::byte>(24),
                           std::vector<std::byte>(plaintext.size() + 16)};
    assert(crypto_aead_xchacha20poly1305_ietf_encrypt(
               reinterpret_cast<unsigned char*>(forged.ciphertext.data()), nullptr,
               plaintext.data(), plaintext.size(),
               reinterpret_cast<const unsigned char*>(aad.data()), aad.size(), nullptr,
               reinterpret_cast<const unsigned char*>(forged.nonce.data()), zero_key.data()) == 0);
    return port->open(forged, binding);
  };
  // Truncated, unknown version, unknown variant and an impossible field length.
  std::vector<unsigned char> malformed(24, 0);
  assert(std::holds_alternative<t::Failure>(authenticated(malformed)));
  malformed[7] = 1;
  malformed[15] = 9;
  assert(std::holds_alternative<t::Failure>(authenticated(malformed)));
  malformed[15] = 1;
  malformed[16] = 255;
  assert(std::holds_alternative<t::Failure>(authenticated(malformed)));
  // Encode a complete, otherwise valid delivery and append a trailing byte.
  std::vector<unsigned char> trailing;
  auto append_integer = [&](std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
      trailing.push_back(static_cast<unsigned char>(value >> (56 - index * 8)));
    }
  };
  append_integer(1);
  append_integer(1);
  append_integer(10);
  for (char value : std::string_view{"credential"}) {
    trailing.push_back(static_cast<unsigned char>(value));
  }
  append_integer(1);
  append_integer(1);
  trailing.push_back(0);
  append_integer(
      static_cast<std::uint64_t>((now + std::chrono::hours{1}).time_since_epoch().count()));
  assert(std::holds_alternative<t::SecretReplayPayload>(authenticated(trailing)));
  trailing.push_back(0);
  assert(std::holds_alternative<t::Failure>(authenticated(trailing)));
} catch (...) {
  return 1;
}
