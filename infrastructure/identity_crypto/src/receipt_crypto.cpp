// clang-format off
#include "kiarabot/infrastructure/identity_crypto/factories.hpp"

#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

#include <sodium.h>
// clang-format on

namespace kiarabot::infrastructure::identity_crypto {
namespace {
namespace t = core::target;
constexpr std::size_t kMaximum = 65536;
t::Failure fail(t::ErrorCode code = t::ErrorCode::Internal) {
  return {code, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
class Secure {
public:
  Secure() : pointer_(static_cast<unsigned char*>(sodium_malloc(kMaximum))) {
    if (!pointer_) {
      throw std::bad_alloc{};
    }
  }
  ~Secure() {
    sodium_free(pointer_);
  }
  Secure(const Secure&) = delete;
  Secure& operator=(const Secure&) = delete;
  unsigned char* data() {
    return pointer_;
  }

private:
  unsigned char* pointer_;
};
class Writer {
public:
  explicit Writer(std::span<std::byte> output) : output_(output) {}
  void integer(std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
      put(std::byte{static_cast<unsigned char>(value >> (56 - index * 8))});
    }
  }
  void field(t::SecretView value) {
    integer(value.size());
    for (auto byte : value) {
      put(byte);
    }
  }
  void text(std::string_view value) {
    field(std::as_bytes(std::span{value.data(), value.size()}));
  }
  void time(t::TimePoint value) {
    integer(std::bit_cast<std::uint64_t>(value.time_since_epoch().count()));
  }
  std::size_t size() const {
    return size_;
  }

private:
  void put(std::byte value) {
    if (size_ == output_.size()) {
      throw std::length_error("receipt limit");
    }
    output_[size_++] = value;
  }
  std::span<std::byte> output_;
  std::size_t size_ = 0;
};
class Reader {
public:
  explicit Reader(t::SecretView input) : input_(input) {}
  std::uint64_t integer() {
    if (input_.size() < 8) {
      throw std::length_error("receipt encoding");
    }
    std::uint64_t result = 0;
    for (auto byte : input_.first(8)) {
      result = (result << 8) | std::to_integer<unsigned>(byte);
    }
    input_ = input_.subspan(8);
    return result;
  }
  t::SecretView field(std::size_t maximum) {
    const auto length = integer();
    if (length == 0 || length > maximum || length > input_.size()) {
      throw std::length_error("receipt field");
    }
    auto result = input_.first(static_cast<std::size_t>(length));
    input_ = input_.subspan(static_cast<std::size_t>(length));
    return result;
  }
  std::string text() {
    auto value = field(1024);
    return {reinterpret_cast<const char*>(value.data()), value.size()};
  }
  t::TimePoint time() {
    return t::TimePoint{std::chrono::nanoseconds{std::bit_cast<std::int64_t>(integer())}};
  }
  bool empty() const {
    return input_.empty();
  }

private:
  t::SecretView input_;
};
bool bounded(std::string_view value) {
  return !value.empty() && value.size() <= 1024;
}
bool valid(const t::SecretReceiptBinding& b) {
  if (b.scope.index() != b.parameters.index() || !bounded(b.subject.value) ||
      !bounded(b.credential.id.value) || b.credential.version.value == 0 ||
      !bounded(b.request.key.value) || b.retain_until <= b.created_at) {
    return false;
  }
  // Unsigned subtraction avoids overflow even for corrupted signed epoch endpoints.
  const auto duration = static_cast<std::uint64_t>(b.retain_until.time_since_epoch().count()) -
                        static_cast<std::uint64_t>(b.created_at.time_since_epoch().count());
  if (duration > static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                t::kSecretReceiptRetention)
                                                .count())) {
    return false;
  }
  return std::visit(
      [&](const auto& scope) {
        bool good = bounded(scope.idempotency_key);
        if constexpr (requires { scope.control_peer; }) {
          good =
              good && bounded(scope.control_peer.value) && b.session && bounded(b.session->value);
        } else {
          good = good && bounded(scope.actor.value) && scope.credential == b.credential.id &&
                 !b.session;
        }
        if constexpr (requires { scope.normalized_login; }) {
          good = good && bounded(scope.normalized_login);
        }
        if constexpr (requires { scope.session; }) {
          good = good && b.session && scope.session == *b.session;
        }
        return good;
      },
      b.scope);
}
void binding(Writer& w, const t::SecretReceiptBinding& b, const t::CryptoKeyRef& key) {
  w.text("kiarabot.identity.receipt.v1");
  w.text(key.value);
  w.integer(1);
  w.integer(b.scope.index());
  std::visit(
      [&](const auto& scope) {
        if constexpr (requires { scope.control_peer; }) {
          w.text(scope.control_peer.value);
        } else {
          w.text(scope.actor.value);
          w.text(scope.credential.value);
        }
        if constexpr (requires { scope.normalized_login; }) {
          w.text(scope.normalized_login);
        }
        if constexpr (requires { scope.session; }) {
          w.text(scope.session.value);
        }
        w.text(scope.idempotency_key);
      },
      b.scope);
  w.text(b.subject.value);
  w.integer(b.session.has_value());
  if (b.session) {
    w.text(b.session->value);
  }
  w.text(b.credential.id.value);
  w.integer(b.credential.version.value);
  w.integer(b.parameters.index());
  std::visit(
      [&](const auto& parameters) {
        if constexpr (requires { parameters.input_rotation; }) {
          w.integer(parameters.input_rotation);
        }
        if constexpr (requires { parameters.expected_version; }) {
          w.integer(parameters.expected_version.value);
        }
      },
      b.parameters);
  w.integer(b.result_rotation);
  w.text(b.request.key.value);
  w.field(b.request.tag.bytes);
  w.time(b.created_at);
  w.time(b.retain_until);
}
bool matches(const t::SecretReplayPayload& payload, const t::SecretReceiptBinding& b) {
  if (const auto* pair = std::get_if<t::EphemeralTokenPair>(&payload)) {
    return b.scope.index() != 2 && b.session && pair->session == *b.session &&
           pair->rotation == b.result_rotation && pair->access && pair->refresh &&
           !pair->access->bytes().empty() && pair->access->bytes().size() <= 16384 &&
           !pair->refresh->bytes().empty() && pair->refresh->bytes().size() <= 16384 &&
           pair->access_expires_at > b.created_at &&
           pair->refresh_expires_at >= pair->access_expires_at;
  }
  const auto& delivery = std::get<t::EphemeralCredentialDelivery>(payload);
  return b.scope.index() == 2 && delivery.credential == b.credential && delivery.secret &&
         !delivery.secret->bytes().empty() && delivery.secret->bytes().size() <= 16384 &&
         delivery.expires_at > b.created_at;
}
void encode(Writer& w, const t::SecretReplayPayload& payload) {
  w.integer(1);
  w.integer(payload.index());
  if (const auto* pair = std::get_if<t::EphemeralTokenPair>(&payload)) {
    w.text(pair->session.value);
    w.field(pair->access->bytes());
    w.field(pair->refresh->bytes());
    w.time(pair->access_expires_at);
    w.time(pair->refresh_expires_at);
    w.integer(pair->rotation);
  } else {
    const auto& delivery = std::get<t::EphemeralCredentialDelivery>(payload);
    w.text(delivery.credential.id.value);
    w.integer(delivery.credential.version.value);
    w.field(delivery.secret->bytes());
    w.time(delivery.expires_at);
  }
}
t::OwnedSecret owned(t::SecretView value) {
  auto copied = copy_secret(value);
  if (std::holds_alternative<t::Failure>(copied)) {
    throw std::bad_alloc{};
  }
  return std::move(std::get<t::OwnedSecret>(copied));
}
t::SecretReplayPayload decode(Reader& r) {
  if (r.integer() != 1) {
    throw std::length_error("receipt version");
  }
  const auto variant = r.integer();
  if (variant == 0) {
    t::EphemeralTokenPair pair;
    pair.session.value = r.text();
    pair.access = owned(r.field(16384));
    pair.refresh = owned(r.field(16384));
    pair.access_expires_at = r.time();
    pair.refresh_expires_at = r.time();
    pair.rotation = r.integer();
    return pair;
  }
  if (variant != 1) {
    throw std::length_error("receipt variant");
  }
  t::EphemeralCredentialDelivery delivery;
  delivery.credential.id.value = r.text();
  delivery.credential.version.value = r.integer();
  delivery.secret = owned(r.field(16384));
  delivery.expires_at = r.time();
  return delivery;
}
class Receipt final : public t::SecretReceiptCryptoPort {
public:
  explicit Receipt(std::vector<SymmetricKey> keys) : keys_(std::move(keys)) {}
  t::Outcome<t::SealedSecret> seal(const t::SecretReplayPayload& payload,
                                   const t::SecretReceiptBinding& b,
                                   const t::CryptoKeyRef& key) override {
    try {
      const auto* material = find(key);
      if (!material) {
        return fail(t::ErrorCode::UpstreamUnavailable);
      }
      if (!valid(b) || !matches(payload, b)) {
        return fail(t::ErrorCode::InvalidArgument);
      }
      Secure plaintext;
      Writer writer({reinterpret_cast<std::byte*>(plaintext.data()), kMaximum});
      encode(writer, payload);
      std::array<std::byte, 16384> aad{};
      Writer auth(aad);
      binding(auth, b, key);
      t::SealedSecret result{key, 1, std::vector<std::byte>(24),
                             std::vector<std::byte>(writer.size() + 16)};
      randombytes_buf(result.nonce.data(), result.nonce.size());
      if (crypto_aead_xchacha20poly1305_ietf_encrypt(
              reinterpret_cast<unsigned char*>(result.ciphertext.data()), nullptr, plaintext.data(),
              writer.size(), reinterpret_cast<const unsigned char*>(aad.data()), auth.size(),
              nullptr, reinterpret_cast<const unsigned char*>(result.nonce.data()),
              reinterpret_cast<const unsigned char*>(material->bytes().data())) != 0) {
        return fail();
      }
      return result;
    } catch (const std::bad_alloc&) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    } catch (const std::length_error&) {
      return fail(t::ErrorCode::InvalidArgument);
    }
  }
  t::Outcome<t::SecretReplayPayload> open(const t::SealedSecret& sealed,
                                          const t::SecretReceiptBinding& b) override {
    try {
      const auto* material = find(sealed.key);
      if (!material) {
        return fail(t::ErrorCode::UpstreamUnavailable);
      }
      if (!valid(b) || sealed.format_version != 1 || sealed.nonce.size() != 24 ||
          sealed.ciphertext.size() < 16 || sealed.ciphertext.size() > kMaximum) {
        return fail();
      }
      std::array<std::byte, 16384> aad{};
      Writer auth(aad);
      binding(auth, b, sealed.key);
      Secure plaintext;
      unsigned long long size = 0;
      if (crypto_aead_xchacha20poly1305_ietf_decrypt(
              plaintext.data(), &size, nullptr,
              reinterpret_cast<const unsigned char*>(sealed.ciphertext.data()),
              sealed.ciphertext.size(), reinterpret_cast<const unsigned char*>(aad.data()),
              auth.size(), reinterpret_cast<const unsigned char*>(sealed.nonce.data()),
              reinterpret_cast<const unsigned char*>(material->bytes().data())) != 0) {
        return fail();
      }
      Reader reader(
          {reinterpret_cast<const std::byte*>(plaintext.data()), static_cast<std::size_t>(size)});
      auto result = decode(reader);
      if (!reader.empty() || !matches(result, b)) {
        return fail();
      }
      return result;
    } catch (const std::bad_alloc&) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    } catch (const std::length_error&) {
      return fail();
    }
  }

private:
  const t::SecretBuffer* find(const t::CryptoKeyRef& key) const {
    for (const auto& entry : keys_) {
      if (entry.reference == key) {
        return entry.material.get();
      }
    }
    return nullptr;
  }
  const std::vector<SymmetricKey> keys_;
};
} // namespace
core::target::Outcome<std::unique_ptr<core::target::SecretReceiptCryptoPort>>
make_secret_receipt_crypto(std::vector<SymmetricKey> keys) {
  if (sodium_init() < 0) {
    return fail(t::ErrorCode::UpstreamUnavailable);
  }
  if (keys.empty() || keys.size() > 16) {
    return fail(t::ErrorCode::InvalidArgument);
  }
  for (std::size_t index = 0; index < keys.size(); ++index) {
    auto& key = keys[index];
    if (!bounded(key.reference.value) || !key.material || key.material->bytes().size() != 32) {
      return fail(t::ErrorCode::InvalidArgument);
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (keys[previous].reference == key.reference) {
        return fail(t::ErrorCode::InvalidArgument);
      }
    }
    auto copied = copy_secret(key.material->bytes());
    if (const auto* error = std::get_if<t::Failure>(&copied)) {
      return *error;
    }
    key.material = std::move(std::get<t::OwnedSecret>(copied));
  }
  try {
    return std::unique_ptr<t::SecretReceiptCryptoPort>{std::make_unique<Receipt>(std::move(keys))};
  } catch (const std::bad_alloc&) {
    return fail(t::ErrorCode::UpstreamUnavailable);
  }
}
} // namespace kiarabot::infrastructure::identity_crypto
