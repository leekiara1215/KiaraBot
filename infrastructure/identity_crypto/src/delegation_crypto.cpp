// clang-format off
#include "kiarabot/infrastructure/identity_crypto/delegation_crypto.hpp"

#include <charconv>
#include <set>
#include <stdexcept>
#include <string_view>

#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <openssl/pem.h>
#include <sodium.h>

#include "kiarabot/core/domain/target/identity_credential_rules.hpp"
// clang-format on

namespace kiarabot::infrastructure::identity_crypto {
namespace {
namespace t = core::target;
t::Failure failure(t::ErrorCode code = t::ErrorCode::Unauthenticated) {
  ERR_clear_error();
  return {code, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
bool text(std::string_view value) {
  return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string_view::npos;
}
template <typename Value> const Value& checked(const nlohmann::json& value) {
  if constexpr (std::is_same_v<Value, std::string>) {
    if (!value.is_string()) {
      throw std::invalid_argument("claim type");
    }
  } else if constexpr (std::is_same_v<Value, nlohmann::json::array_t>) {
    if (!value.is_array()) {
      throw std::invalid_argument("claim type");
    }
  } else {
    if (!value.is_object()) {
      throw std::invalid_argument("claim type");
    }
  }
  return value.get_ref<const Value&>();
}
std::string number(std::uint64_t value) {
  return std::to_string(value);
}
std::uint64_t integer(const nlohmann::json& value) {
  const auto& input = checked<std::string>(value);
  std::uint64_t result = 0;
  const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
  if (input.empty() || (input.size() > 1 && input[0] == '0') || parsed.ec != std::errc{} ||
      parsed.ptr != input.data() + input.size()) {
    throw std::invalid_argument("integer");
  }
  return result;
}
std::int64_t seconds(t::TimePoint value) {
  const auto nanos = value.time_since_epoch().count();
  if (nanos < 0 || nanos % 1000000000 != 0) {
    throw std::invalid_argument("time");
  }
  return nanos / 1000000000;
}
t::TimePoint timestamp(std::int64_t value) {
  if (value < 0 || value > std::numeric_limits<std::int64_t>::max() / 1000000000) {
    throw std::invalid_argument("time");
  }
  return t::TimePoint{std::chrono::seconds{value}};
}
std::string digest(const t::Sha256Digest& value) {
  char result[65]{};
  sodium_bin2hex(result, sizeof(result), reinterpret_cast<const unsigned char*>(value.bytes.data()),
                 32);
  return result;
}
t::Sha256Digest digest(std::string_view value) {
  if (value.size() != 64) {
    throw std::invalid_argument("fingerprint");
  }
  t::Sha256Digest result;
  for (char character : value) {
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      throw std::invalid_argument("fingerprint");
    }
  }
  if (sodium_hex2bin(reinterpret_cast<unsigned char*>(result.bytes.data()), 32, value.data(),
                     value.size(), nullptr, nullptr, nullptr) != 0) {
    throw std::invalid_argument("fingerprint");
  }
  return result;
}
std::string audience(t::Service value) {
  switch (value) {
  case t::Service::Identity:
    return "identity";
  case t::Service::Runtime:
    return "runtime";
  case t::Service::Host:
    return "host";
  case t::Service::Scheduler:
    return "scheduler";
  case t::Service::Model:
    return "model";
  case t::Service::Control:
    return "control";
  }
  throw std::invalid_argument("audience");
}
t::Service audience(std::string_view value) {
  for (auto service : {t::Service::Identity, t::Service::Runtime, t::Service::Host,
                       t::Service::Scheduler, t::Service::Model, t::Service::Control}) {
    if (audience(service) == value) {
      return service;
    }
  }
  throw std::invalid_argument("audience");
}
bool clock_ok(const t::SecurityClockSample& sample) {
  return sample.trusted && sample.maximum_known_skew >= std::chrono::milliseconds{0} &&
         sample.maximum_known_skew <= t::kMaximumKnownClockSkew &&
         sample.utc.time_since_epoch().count() >= 0 &&
         sample.utc <= t::TimePoint::max() - std::chrono::seconds{2};
}
bool shape(const t::DelegationClaims& c) {
  if (!text(c.issuer) || !text(c.subject.value) || !text(c.direct_actor.value) ||
      !text(c.proof_id.value) || !text(c.source_credential.id.value) ||
      !c.authorization_version.value || (c.source_session && !text(c.source_session->value)) ||
      std::holds_alternative<t::Failure>(t::validate_delegation_origin(c)) || c.business_hops > 2 ||
      c.actor_chain.empty() || c.actor_chain.size() > 3 ||
      c.actor_chain.size() != static_cast<std::size_t>(c.business_hops) + 1 ||
      c.actor_chain.back() != c.direct_actor || c.permissions.empty() ||
      c.permissions.size() > 128) {
    return false;
  }
  std::set<std::string> actors;
  for (const auto& actor : c.actor_chain) {
    if (!text(actor.value) || !actors.insert(actor.value).second) {
      return false;
    }
  }
  std::set<std::tuple<unsigned, unsigned, std::string>> permissions;
  for (const auto& permission : c.permissions) {
    const auto p = static_cast<unsigned>(permission.permission);
    const auto r = static_cast<unsigned>(permission.resource.kind);
    if (p == 0 || p > static_cast<unsigned>(t::Permission::MessageRead) ||
        r > static_cast<unsigned>(t::ResourceKind::Catalog) ||
        !text(permission.resource.id.value) ||
        !permissions.emplace(p, r, permission.resource.id.value).second) {
      return false;
    }
  }
  const auto issued = seconds(c.issued_at);
  const auto expires = seconds(c.expires_at);
  const auto before = seconds(c.not_before);
  audience(c.audience);
  return expires > issued && expires - issued <= 56 && before >= issued && before < expires;
}
struct FixedClock {
  jwt::date value;
  jwt::date now() const {
    return value;
  }
};
class TemporarySecret {
public:
  explicit TemporarySecret(t::SecretView value)
      : value_(reinterpret_cast<const char*>(value.data()), value.size()) {}
  explicit TemporarySecret(std::string&& value) : value_(std::move(value)) {}
  ~TemporarySecret() {
    sodium_memzero(value_.data(), value_.size());
  }
  TemporarySecret(const TemporarySecret&) = delete;
  TemporarySecret& operator=(const TemporarySecret&) = delete;
  const std::string& value() const {
    return value_;
  }

private:
  std::string value_;
};
nlohmann::json encode(const t::DelegationClaims& c) {
  nlohmann::json::object_t object;
  object["direct_actor"] = nlohmann::json(c.direct_actor.value);
  nlohmann::json::array_t actors;
  for (const auto& actor : c.actor_chain) {
    actors.emplace_back(actor.value);
  }
  object["actor_chain"] = nlohmann::json(actors);
  nlohmann::json::array_t permissions;
  for (const auto& item : c.permissions) {
    nlohmann::json::array_t entry{nlohmann::json(number(static_cast<unsigned>(item.permission))),
                                  nlohmann::json(number(static_cast<unsigned>(item.resource.kind))),
                                  nlohmann::json(item.resource.id.value)};
    permissions.emplace_back(entry);
  }
  object["permissions"] = nlohmann::json(permissions);
  object["client_certificate"] = nlohmann::json(digest(c.client_certificate));
  object["authorization_version"] = nlohmann::json(number(c.authorization_version.value));
  object["business_hops"] = nlohmann::json(number(c.business_hops));
  object["source_credential"] = nlohmann::json(
      nlohmann::json::array_t{nlohmann::json(c.source_credential.id.value),
                              nlohmann::json(number(c.source_credential.version.value))});
  object["source_session"] =
      c.source_session ? nlohmann::json(c.source_session->value) : nlohmann::json();
  return nlohmann::json(object);
}
t::DelegationClaims decode(const jwt::decoded_jwt<jwt::traits::nlohmann_json>& token) {
  t::DelegationClaims c;
  c.issuer = token.get_issuer();
  c.audience = audience(token.get_payload_claim("aud").as_string());
  c.subject.value = token.get_subject();
  c.proof_id.value = token.get_id();
  const auto date = [&](const char* name) {
    const auto value = token.get_payload_claim(name).to_json();
    if (!value.is_number_integer()) {
      throw std::invalid_argument("date type");
    }
    if (value.is_number_unsigned() &&
        value.get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw std::invalid_argument("date range");
    }
    return timestamp(value.get<std::int64_t>());
  };
  c.issued_at = date("iat");
  c.not_before = date("nbf");
  c.expires_at = date("exp");
  const auto object =
      checked<nlohmann::json::object_t>(token.get_payload_claim("delegation").to_json());
  if (object.size() != 8 || token.get_payload_json().size() != 8) {
    throw std::invalid_argument("claims");
  }
  c.direct_actor.value = checked<std::string>(object.at("direct_actor"));
  const auto& actors = checked<nlohmann::json::array_t>(object.at("actor_chain"));
  if (actors.size() > 2) {
    throw std::invalid_argument("actors");
  }
  for (const auto& actor : actors) {
    c.actor_chain.push_back({checked<std::string>(actor)});
  }
  const auto& permissions = checked<nlohmann::json::array_t>(object.at("permissions"));
  if (permissions.size() > 128) {
    throw std::invalid_argument("permissions");
  }
  for (const auto& item : permissions) {
    const auto& entry = checked<nlohmann::json::array_t>(item);
    if (entry.size() != 3) {
      throw std::invalid_argument("permission");
    }
    const auto p = integer(entry[0]);
    const auto r = integer(entry[1]);
    if (p == 0 || p > static_cast<unsigned>(t::Permission::MessageRead) ||
        r > static_cast<unsigned>(t::ResourceKind::Catalog)) {
      throw std::invalid_argument("permission");
    }
    c.permissions.push_back({static_cast<t::Permission>(p),
                             {static_cast<t::ResourceKind>(r), {checked<std::string>(entry[2])}}});
  }
  c.client_certificate = digest(checked<std::string>(object.at("client_certificate")));
  c.authorization_version.value = integer(object.at("authorization_version"));
  const auto hops = integer(object.at("business_hops"));
  if (hops > 2) {
    throw std::invalid_argument("hops");
  }
  c.business_hops = static_cast<std::uint32_t>(hops);
  const auto& origin = checked<nlohmann::json::array_t>(object.at("source_credential"));
  if (origin.size() != 2) {
    throw std::invalid_argument("origin");
  }
  c.source_credential = {{checked<std::string>(origin[0])}, {integer(origin[1])}};
  const auto& session = object.at("source_session");
  if (!session.is_null()) {
    c.source_session = t::SessionId{checked<std::string>(session)};
  }
  return c;
}
bool pem_complete(BIO* input) {
  char remainder[256];
  int count = 0;
  while ((count = BIO_read(input, remainder, sizeof(remainder))) > 0) {
    for (int index = 0; index < count; ++index) {
      const char value = remainder[index];
      if (value != ' ' && value != '\n' && value != '\r' && value != '\t') {
        return false;
      }
    }
  }
  return true;
}
class Delegation final : public t::DelegationCryptoPort {
public:
  Delegation(DelegationConfig config, t::SecurityClockPort& clock)
      : config_(std::move(config)), clock_(clock) {}
  t::Outcome<t::OwnedSecret> sign(const t::DelegationClaims& claims,
                                  const t::CryptoKeyRef& reference) override {
    try {
      const auto* key = find(reference);
      if (!key || !key->private_key_pem) {
        return failure(t::ErrorCode::UpstreamUnavailable);
      }
      auto sample = clock_.sample();
      if (const auto* error = std::get_if<t::Failure>(&sample)) {
        return *error;
      }
      const auto& now = std::get<t::SecurityClockSample>(sample);
      if (!clock_ok(now) || claims.issuer != config_.issuer || !shape(claims) ||
          claims.issued_at > now.utc || now.utc >= claims.expires_at ||
          now.utc - claims.issued_at > std::chrono::seconds{2}) {
        return failure(t::ErrorCode::InvalidArgument);
      }
      TemporarySecret pem(key->private_key_pem->bytes());
      auto builder = jwt::create();
      const jwt::claim type_claim(std::string{"JWT"});
      builder.set_header_claim("typ", type_claim);
      auto encoded =
          builder.set_key_id(reference.value)
              .set_issuer(claims.issuer)
              .set_audience(claims.audience == t::Service::Identity ? "identity"
                                                                    : audience(claims.audience))
              .set_subject(claims.subject.value)
              .set_id(claims.proof_id.value)
              .set_issued_at(jwt::date{std::chrono::seconds{seconds(claims.issued_at)}})
              .set_not_before(jwt::date{std::chrono::seconds{seconds(claims.not_before)}})
              .set_expires_at(jwt::date{std::chrono::seconds{seconds(claims.expires_at)}})
              .set_payload_claim("delegation", jwt::claim(encode(claims)))
              .sign(jwt::algorithm::ed25519(key->public_key_pem, pem.value()));
      TemporarySecret output(std::move(encoded));
      return copy_secret(std::as_bytes(std::span{output.value().data(), output.value().size()}));
    } catch (const std::bad_alloc&) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    } catch (const std::exception&) {
      return failure(t::ErrorCode::InvalidArgument);
    }
  }
  t::Outcome<t::VerifiedDelegation> verify(t::SecretView proof, t::Service expected_audience,
                                           const t::VerifiedPeer& peer,
                                           const t::SecurityClockSample& clock) override {
    try {
      if (proof.empty() || proof.size() > 32768 || !clock_ok(clock) ||
          peer.verified_at > clock.utc || peer.verified_at < clock.utc - std::chrono::seconds{2} ||
          peer.binding.certificate.not_before > clock.utc ||
          peer.binding.certificate.not_after <= clock.utc) {
        return failure();
      }
      TemporarySecret encoded(proof);
      auto token = jwt::decode(encoded.value());
      const auto header = token.get_header_json();
      if (header.size() != 3 || token.get_algorithm() != "EdDSA" || token.get_type() != "JWT" ||
          !text(token.get_key_id())) {
        return failure();
      }
      const auto* key = find({token.get_key_id()});
      if (!key) {
        return failure();
      }
      // Bound and type-check NumericDate before jwt-cpp's date conversion; its floating-point
      // fallback can otherwise overflow time_t on attacker-controlled numeric values.
      auto claims = decode(token);
      if (!shape(claims)) {
        return failure();
      }
      const FixedClock fixed{
          jwt::date{std::chrono::floor<std::chrono::seconds>(clock.utc.time_since_epoch())}};
      jwt::verify<FixedClock, jwt::traits::nlohmann_json>(fixed)
          .allow_algorithm(jwt::algorithm::ed25519(key->public_key_pem))
          .with_issuer(config_.issuer)
          .leeway(2)
          .verify(token);
      if (claims.audience != expected_audience ||
          claims.client_certificate != peer.binding.certificate.fingerprint ||
          claims.direct_actor != peer.binding.certificate.subject ||
          claims.issued_at > clock.utc + std::chrono::seconds{2} ||
          claims.not_before > clock.utc + std::chrono::seconds{2} ||
          claims.expires_at <= clock.utc - std::chrono::seconds{2}) {
        return failure();
      }
      const bool subject_matches = std::visit(
          [&](const auto& p) {
            if (p.subject != claims.direct_actor) {
              return false;
            }
            if constexpr (requires { p.service; }) {
              audience(p.service);
              return peer.binding.certificate.platform_service == p.service;
            } else if constexpr (requires { p.adapter; }) {
              return !peer.binding.certificate.platform_service && text(p.adapter.value);
            } else {
              return !peer.binding.certificate.platform_service && text(p.instance.value);
            }
          },
          peer.binding.peer);
      if (!subject_matches || !text(peer.binding.credential.id.value) ||
          !peer.binding.credential.version.value) {
        return failure();
      }
      return t::VerifiedDelegation{std::move(claims), key->reference};
    } catch (const std::bad_alloc&) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    } catch (const std::exception&) {
      return failure();
    }
  }

private:
  const DelegationKey* find(const t::CryptoKeyRef& reference) const {
    for (const auto& key : config_.keys) {
      if (key.reference == reference) {
        return &key;
      }
    }
    return nullptr;
  }
  const DelegationConfig config_;
  t::SecurityClockPort& clock_;
};
} // namespace
core::target::Outcome<std::unique_ptr<core::target::DelegationCryptoPort>>
make_delegation_crypto(DelegationConfig config, core::target::SecurityClockPort& clock) {
  try {
    if (sodium_init() < 0) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    if (!text(config.issuer) || config.keys.empty() || config.keys.size() > 16) {
      return failure(t::ErrorCode::InvalidArgument);
    }
    std::set<std::string> references;
    for (auto& key : config.keys) {
      if (!text(key.reference.value) || !references.insert(key.reference.value).second ||
          !key.public_key_pem.starts_with("-----BEGIN PUBLIC KEY-----\n") ||
          key.public_key_pem.find('\0') != std::string::npos || key.public_key_pem.size() > 8192) {
        return failure(t::ErrorCode::InvalidArgument);
      }
      std::unique_ptr<BIO, decltype(&BIO_free)> bio(
          BIO_new_mem_buf(key.public_key_pem.data(), static_cast<int>(key.public_key_pem.size())),
          BIO_free);
      std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> public_key(
          bio ? PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr) : nullptr, EVP_PKEY_free);
      if (!public_key || !pem_complete(bio.get()) ||
          EVP_PKEY_is_a(public_key.get(), "ED25519") != 1) {
        return failure(t::ErrorCode::InvalidArgument);
      }
      if (key.private_key_pem) {
        if (key.private_key_pem->bytes().empty() || key.private_key_pem->bytes().size() > 8192) {
          return failure(t::ErrorCode::InvalidArgument);
        }
        TemporarySecret pem(key.private_key_pem->bytes());
        if (!pem.value().starts_with("-----BEGIN PRIVATE KEY-----\n") ||
            pem.value().find('\0') != std::string::npos) {
          return failure(t::ErrorCode::InvalidArgument);
        }
        std::unique_ptr<BIO, decltype(&BIO_free)> private_bio(
            BIO_new_mem_buf(pem.value().data(), static_cast<int>(pem.value().size())), BIO_free);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> private_key(
            private_bio ? PEM_read_bio_PrivateKey(private_bio.get(), nullptr, nullptr, nullptr)
                        : nullptr,
            EVP_PKEY_free);
        if (!private_key || !pem_complete(private_bio.get()) ||
            EVP_PKEY_is_a(private_key.get(), "ED25519") != 1 ||
            EVP_PKEY_eq(public_key.get(), private_key.get()) != 1) {
          return failure(t::ErrorCode::InvalidArgument);
        }
        auto copy = copy_secret(key.private_key_pem->bytes());
        if (const auto* error = std::get_if<t::Failure>(&copy)) {
          return *error;
        }
        key.private_key_pem = std::move(std::get<t::OwnedSecret>(copy));
      }
    }
    return std::unique_ptr<t::DelegationCryptoPort>{
        std::make_unique<Delegation>(std::move(config), clock)};
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable);
  } catch (const std::exception&) {
    return failure(t::ErrorCode::InvalidArgument);
  }
}
} // namespace kiarabot::infrastructure::identity_crypto
