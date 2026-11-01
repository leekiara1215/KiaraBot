#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <sys/stat.h>

#include "kiarabot/core/application/target/identity_admin_application.hpp"
#include "kiarabot/core/domain/target/identity_policy.hpp"
#include "kiarabot/core/runtime/platform/service_config.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/infrastructure/identity_crypto/factories.hpp"
#include "kiarabot/infrastructure/identity_crypto/key_files.hpp"
#include "kiarabot/infrastructure/identity_crypto/peer_certificate.hpp"
#include "kiarabot/infrastructure/postgres/security_factory.hpp"

namespace {
namespace t = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace codec = platform::target_codec;
namespace crypto = kiarabot::infrastructure::identity_crypto;
namespace postgres = kiarabot::infrastructure::postgres;
using Json = nlohmann::json;
struct Rejected final : std::exception {
  explicit Rejected(t::Failure reason) : failure(std::move(reason)) {}
  t::Failure failure;
};
template <typename Value> Value take(t::Outcome<Value> result) {
  if (const auto* failure = std::get_if<t::Failure>(&result)) {
    throw Rejected{*failure};
  }
  return std::move(std::get<Value>(result));
}
void require(bool condition) {
  if (!condition) {
    throw Rejected{{t::ErrorCode::InvalidArgument,
                    t::Acceptance::NotAccepted,
                    t::Recovery::CorrectInput,
                    {},
                    {}}};
  }
}
class Descriptor {
public:
  explicit Descriptor(const std::string& path)
      : value_(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)) {}
  ~Descriptor() {
    if (value_ >= 0) {
      close(value_);
    }
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  int get() const {
    return value_;
  }

private:
  int value_;
};
// Public enrollment/certificate metadata only. Password bytes use the secure loader below.
std::string read_metadata(const std::string& path, std::size_t maximum) {
  require(!path.empty() && path.find('\0') == std::string::npos);
  Descriptor file(path);
  struct stat before {};
  require(file.get() >= 0 && fstat(file.get(), &before) == 0);
  const auto safe = [maximum](const struct stat& value) {
    return S_ISREG(value.st_mode) && (value.st_uid == geteuid() || value.st_uid == 0) &&
           (value.st_mode & 022) == 0 && value.st_size > 0 &&
           static_cast<std::uintmax_t>(value.st_size) <= maximum;
  };
  require(safe(before));
  std::string output(static_cast<std::size_t>(before.st_size) + 1, '\0');
  std::size_t offset = 0;
  while (offset < output.size()) {
    const auto count = read(file.get(), output.data() + offset, output.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    require(count >= 0);
    if (count == 0) {
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  struct stat after {};
  require(fstat(file.get(), &after) == 0);
#if defined(__APPLE__)
  const auto modified_before = before.st_mtimespec;
  const auto modified_after = after.st_mtimespec;
  const auto changed_before = before.st_ctimespec;
  const auto changed_after = after.st_ctimespec;
#else
  const auto modified_before = before.st_mtim;
  const auto modified_after = after.st_mtim;
  const auto changed_before = before.st_ctim;
  const auto changed_after = after.st_ctim;
#endif
  require(safe(after) && before.st_size == after.st_size && before.st_dev == after.st_dev &&
          before.st_ino == after.st_ino && before.st_uid == after.st_uid &&
          before.st_mode == after.st_mode && modified_before.tv_sec == modified_after.tv_sec &&
          modified_before.tv_nsec == modified_after.tv_nsec &&
          changed_before.tv_sec == changed_after.tv_sec &&
          changed_before.tv_nsec == changed_after.tv_nsec &&
          offset == static_cast<std::size_t>(before.st_size));
  output.resize(offset);
  require(output.find('\0') == std::string::npos);
  return output;
}
bool exact(const Json& object, std::initializer_list<std::string_view> keys) {
  if (!object.is_object() || object.size() != keys.size()) {
    return false;
  }
  for (const auto key : keys) {
    if (!object.contains(std::string(key))) {
      return false;
    }
  }
  return true;
}
Json enrollment(const std::string& path) {
  std::vector<std::set<std::string>> objects;
  bool duplicate = false;
  const auto callback = [&](int, Json::parse_event_t event, Json& value) {
    if (event == Json::parse_event_t::object_start) {
      objects.emplace_back();
    } else if (event == Json::parse_event_t::key) {
      duplicate = !objects.back().insert(value.get<std::string>()).second || duplicate;
    } else if (event == Json::parse_event_t::object_end) {
      objects.pop_back();
    }
    return true;
  };
  auto result = Json::parse(read_metadata(path, 64 * 1024), callback);
  require(!duplicate &&
          exact(result, {"schema_version", "administrator_id", "password_credential_id", "username",
                         "control_id", "control_credential_id", "control_certificate_chain_file",
                         "control_san", "administrator_assignments", "control_assignments"}));
  require(result["schema_version"].is_number_integer() && result["schema_version"] == 1);
  return result;
}
std::string text(const Json& object, const char* key) {
  require(object.at(key).is_string());
  auto result = object.at(key).get<std::string>();
  require(!result.empty() && result.size() <= 1024 && result.find('\0') == std::string::npos);
  return result;
}
std::string identifier(const Json& object, const char* key) {
  auto result = text(object, key);
  kiarabot::v1::ResourceRef wire;
  wire.set_kind(kiarabot::v1::RESOURCE_PRINCIPAL);
  wire.set_id(result);
  take(codec::from_wire(wire));
  return result;
}
std::vector<t::ExactAssignment> assignments(const Json& rows, const t::SubjectId& subject) {
  require(rows.is_array() && !rows.empty() && rows.size() <= 128);
  std::vector<t::ExactAssignment> result;
  for (const auto& row : rows) {
    const bool role = row.contains("role");
    require(role ? exact(row, {"assignment_id", "role", "scope"})
                 : exact(row, {"assignment_id", "permissions", "scope"}));
    t::ExactAssignment output{{identifier(row, "assignment_id")}, subject, {}, {}, {1}};
    require(row["scope"].is_array() && !row["scope"].empty() && row["scope"].size() <= 128);
    std::vector<t::ResourceRef> scope;
    for (const auto& resource : row["scope"]) {
      require(exact(resource, {"kind", "id"}));
      kiarabot::v1::ResourceKind kind;
      require(kiarabot::v1::ResourceKind_Parse(text(resource, "kind"), &kind));
      kiarabot::v1::ResourceRef wire;
      wire.set_kind(kind);
      wire.set_id(text(resource, "id"));
      scope.push_back(take(codec::from_wire(wire)));
    }
    if (role) {
      const auto name = text(row, "role");
      require(name == "admin" || name == "operator" || name == "viewer");
      const auto fixed = name == "admin"      ? t::FixedRole::Admin
                         : name == "operator" ? t::FixedRole::Operator
                                              : t::FixedRole::Viewer;
      for (const auto& resource : scope) {
        output.roles.push_back({fixed, resource});
      }
    } else {
      require(row["permissions"].is_array() && !row["permissions"].empty() &&
              row["permissions"].size() <= t::kPermissionNames.size());
      for (const auto& permission : row["permissions"]) {
        require(permission.is_string());
        const auto parsed = take(t::permission_from_name(permission.get<std::string>()));
        for (const auto& resource : scope) {
          output.explicit_permissions.push_back({parsed, resource});
        }
      }
    }
    take(t::validate_exact_assignment(output));
    result.push_back(std::move(output));
  }
  take(t::validate_assignment_collection(result, subject));
  return result;
}
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
t::TimePoint certificate_time(const ASN1_TIME* value) {
  std::unique_ptr<ASN1_TIME, decltype(&ASN1_TIME_free)> epoch(ASN1_TIME_set(nullptr, 0),
                                                              ASN1_TIME_free);
  int days = 0;
  int seconds = 0;
  require(epoch && ASN1_TIME_diff(&days, &seconds, epoch.get(), value) == 1);
  const auto total = static_cast<std::int64_t>(days) * 86400 + seconds;
  require(total > 0 && total <= std::numeric_limits<std::int64_t>::max() / 1000000000);
  return t::TimePoint{std::chrono::nanoseconds{total * 1000000000}};
}
struct EnrollmentCertificate {
  t::CertificateBinding binding;
  std::vector<std::byte> chain;
};
EnrollmentCertificate certificate(const Json& document, const t::SubjectId& control) {
  const auto input = read_metadata(text(document, "control_certificate_chain_file"), 64 * 1024);
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
      BIO_new_mem_buf(input.data(), static_cast<int>(input.size())), BIO_free);
  require(static_cast<bool>(bio));
  EnrollmentCertificate result;
  std::size_t certificates = 0;
  while (BIO_ctrl_pending(bio.get()) != 0) {
    char* remaining = nullptr;
    const auto size = BIO_get_mem_data(bio.get(), &remaining);
    require(size >= 0);
    std::string_view tail(remaining, static_cast<std::size_t>(size));
    const auto start = tail.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
      break;
    }
    require(tail.substr(start).starts_with("-----BEGIN CERTIFICATE-----") && certificates < 8);
    Certificate leaf(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free);
    require(static_cast<bool>(leaf));
    if (certificates == 0) {
      std::array<unsigned char, 32> fingerprint{};
      unsigned int length = 0;
      require(X509_digest(leaf.get(), EVP_sha256(), fingerprint.data(), &length) == 1 &&
              length == 32);
      t::Sha256Digest digest;
      for (std::size_t index = 0; index < fingerprint.size(); ++index) {
        digest.bytes[index] = static_cast<std::byte>(fingerprint[index]);
      }
      result.binding = {control,
                        digest,
                        text(document, "control_san"),
                        t::Service::Control,
                        certificate_time(X509_get0_notBefore(leaf.get())),
                        certificate_time(X509_get0_notAfter(leaf.get()))};
    }
    const auto length = i2d_X509(leaf.get(), nullptr);
    require(length > 0 && result.chain.size() + static_cast<std::size_t>(length) <= 64 * 1024);
    std::vector<unsigned char> encoded(static_cast<std::size_t>(length));
    auto* cursor = encoded.data();
    require(i2d_X509(leaf.get(), &cursor) == length);
    for (const auto byte : encoded) {
      result.chain.push_back(static_cast<std::byte>(byte));
    }
    ++certificates;
  }
  require(certificates > 0);
  ERR_clear_error();
  return result;
}
std::string random_id(t::RandomPort& random) {
  const auto bytes = take(random.bytes(16));
  require(bytes && bytes->bytes().size() == 16);
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  for (std::size_t index = 0; index < 16; ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result.push_back('-');
    }
    auto value = std::to_integer<unsigned>(bytes->bytes()[index]);
    if (index == 6) {
      value = (value & 15U) | 64U;
    } else if (index == 8) {
      value = (value & 63U) | 128U;
    }
    result.push_back(kHex[value >> 4U]);
    result.push_back(kHex[value & 15U]);
  }
  return result;
}
int execute(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "usage: kiarabot-identity-bootstrap --config CONFIG --enrollment ENROLLMENT "
                 "--password-file PASSWORD\n"
                 "       kiarabot-identity-bootstrap --config CONFIG --status\n";
    return 0;
  }
  std::string config_path;
  std::string enrollment_path;
  std::string password_path;
  bool status = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--status" && !status) {
      status = true;
      continue;
    }
    if (index + 1 >= argc) {
      return 2;
    }
    auto* destination = option == "--config"          ? &config_path
                        : option == "--enrollment"    ? &enrollment_path
                        : option == "--password-file" ? &password_path
                                                      : nullptr;
    if (!destination || !destination->empty()) {
      return 2;
    }
    *destination = argv[++index];
  }
  if (config_path.empty() || (status ? !enrollment_path.empty() || !password_path.empty()
                                     : enrollment_path.empty() || password_path.empty())) {
    return 2;
  }
  auto loaded = platform::load_service_config(config_path, t::Service::Identity);
  if (const auto* error = std::get_if<platform::ConfigFailure>(&loaded)) {
    return error->exit_code;
  }
  const auto& config = std::get<platform::ProcessConfig>(loaded);
  require(config.database.has_value());
  const auto& db = *config.database;
  auto factory = take(postgres::open_identity_security(
      {t::Service::Identity, db.host, db.port, db.name, db.runtime_role, db.password, db.ca_file,
       db.migration_role, db.runtime_role}));
  if (status) {
    auto work = take(factory->begin());
    const auto marker = take(work->bootstrap().state());
    std::cout << "operation=bootstrap_status state="
              << (marker.state == t::BootstrapState::Consumed ? "consumed" : "required")
              << " version=" << marker.version.value << '\n';
    return 0;
  }
  const auto document = enrollment(enrollment_path);
  const t::SubjectId administrator{identifier(document, "administrator_id")};
  const t::SubjectId control{identifier(document, "control_id")};
  const t::CredentialId control_credential{identifier(document, "control_credential_id")};
  auto enrolled = certificate(document, control);
  auto peer_crypto = take(crypto::make_peer_certificate_crypto({{config.tls.ca}}));
  const auto now =
      std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
  take(peer_crypto->verify(
      enrolled.chain,
      {t::PlatformPeer{t::Service::Control, control}, enrolled.binding, {control_credential, {1}}},
      now));
  auto password = take(crypto::load_bootstrap_password_file(password_path));
  auto passwords = take(crypto::make_password_crypto());
  auto verifier = take(passwords->derive(password->bytes()));
  password.reset();
  auto random = take(crypto::make_random());
  constexpr auto kPasswordLifetime = std::chrono::hours{24 * 365};
  require(now <= t::TimePoint::max() - kPasswordLifetime);
  t::OfflineIdentityBootstrap request{
      {random_id(*random), random_id(*random), {}, now + std::chrono::seconds{30}},
      {1},
      administrator,
      {identifier(document, "password_credential_id")},
      text(document, "username"),
      std::move(verifier),
      now + kPasswordLifetime,
      control,
      control_credential,
      std::move(enrolled.binding),
      assignments(document["administrator_assignments"], administrator),
      assignments(document["control_assignments"], control)};
  t::IdentityAdminApplication application(*factory, *peer_crypto);
  auto outcome = application.bootstrap_offline(request);
  if (const auto* error = std::get_if<t::Failure>(&outcome)) {
    throw Rejected{*error};
  }
  std::cout << "operation=bootstrap outcome=committed state=consumed\n";
  return 0;
}
} // namespace
int main(int argc, char* argv[]) {
  try {
    return execute(argc, argv);
  } catch (const Rejected& error) {
    const auto encoded = codec::to_wire(error.failure.code);
    const auto* code = std::get_if<kiarabot::v1::ErrorCode>(&encoded);
    std::cerr << "operation=bootstrap outcome=failed error_code="
              << (code ? kiarabot::v1::ErrorCode_Name(*code) : "INTERNAL") << " acceptance="
              << (error.failure.acceptance == t::Acceptance::Unknown ? "unknown" : "not_accepted")
              << '\n';
    return 1;
  } catch (...) {
    std::cerr << "operation=bootstrap outcome=failed error_code=INTERNAL\n";
    return 1;
  }
}
