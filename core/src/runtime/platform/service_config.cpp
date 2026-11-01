#include "kiarabot/core/runtime/platform/service_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <ctime>
#include <fcntl.h>
#include <memory>
#include <set>
#include <stdexcept>
#include <unistd.h>
#include <vector>

#include <arpa/inet.h>
#include <nlohmann/json.hpp>
#include <openssl/pem.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#include <sys/stat.h>

namespace kiarabot::core::platform {
namespace {

constexpr std::size_t kMaxFileBytes = 1024 * 1024;
struct FileFailure {};
class File {
public:
  explicit File(const std::string& path)
      : descriptor_(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK)) {}
  ~File() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  int get() const {
    return descriptor_;
  }

private:
  int descriptor_;
};
std::string read_file(const std::string& path, bool private_key = false) {
  if (path.empty() || path.find('\0') != std::string::npos) {
    throw FileFailure{};
  }
  const File file(path);
  struct stat status {};
  if (file.get() < 0 || fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0 || static_cast<std::uintmax_t>(status.st_size) > kMaxFileBytes ||
      (private_key && ((status.st_mode & 0077) != 0 || status.st_uid != geteuid()))) {
    throw FileFailure{};
  }
  std::string content;
  std::array<char, 4096> buffer{};
  for (;;) {
    const auto count = read(file.get(), buffer.data(), buffer.size());
    if (count < 0) {
      throw FileFailure{};
    }
    if (count == 0) {
      break;
    }
    content.append(buffer.data(), static_cast<std::size_t>(count));
    if (content.size() > kMaxFileBytes) {
      throw FileFailure{};
    }
  }
  return content;
}
bool address_valid(const std::string& endpoint, bool probe) {
  const auto separator = endpoint.find(':');
  if (separator == std::string::npos) {
    return false;
  }
  const auto host = endpoint.substr(0, separator);
  const auto port = endpoint.substr(separator + 1);
  unsigned int value = 0;
  const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
  in_addr address{};
  return !port.empty() && parsed.ec == std::errc{} && parsed.ptr == port.data() + port.size() &&
         value <= 65535 && inet_pton(AF_INET, host.c_str(), &address) == 1 &&
         (!probe || host == "127.0.0.1");
}
bool exact_keys(const nlohmann::json& object, const std::set<std::string>& keys) {
  if (!object.is_object() || object.size() != keys.size()) {
    return false;
  }
  for (const auto& key : keys) {
    if (!object.contains(key)) {
      return false;
    }
  }
  return true;
}
bool database_identifier(const std::string& value) {
  return !value.empty() && value.size() <= 63 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '_';
         });
}
bool database_shape(const nlohmann::json& value) {
  if (!exact_keys(value, {"host", "port", "name", "migration_role", "runtime_role", "password_file",
                          "ca_file"}) ||
      !value["port"].is_number_unsigned() || value["port"].get<std::uint64_t>() == 0 ||
      value["port"].get<std::uint64_t>() > 65535) {
    return false;
  }
  for (const auto* key :
       {"host", "name", "migration_role", "runtime_role", "password_file", "ca_file"}) {
    if (!value[key].is_string() || value[key].get_ref<const std::string&>().empty() ||
        value[key].get_ref<const std::string&>().find('\0') != std::string::npos) {
      return false;
    }
  }
  const auto& host = value["host"].get_ref<const std::string&>();
  return host.size() <= 253 && host.front() != '/' &&
         std::none_of(host.begin(), host.end(),
                      [](unsigned char character) {
                        return character == ',' || std::isspace(character) != 0;
                      }) &&
         database_identifier(value["name"].get_ref<const std::string&>()) &&
         database_identifier(value["migration_role"].get_ref<const std::string&>()) &&
         database_identifier(value["runtime_role"].get_ref<const std::string&>()) &&
         value["migration_role"] != value["runtime_role"];
}
bool control_api_shape(const nlohmann::json& value) {
  if (!exact_keys(value, {"listen", "identity_endpoint"}) || !value["listen"].is_string() ||
      !value["identity_endpoint"].is_string() ||
      !address_valid(value["listen"].get<std::string>(), false)) {
    return false;
  }
  const auto endpoint = value["identity_endpoint"].get<std::string>();
  const auto colon = endpoint.find(':');
  if (colon == std::string::npos || colon == 0 || colon > 253) {
    return false;
  }
  const auto host = endpoint.substr(0, colon);
  const auto port = endpoint.substr(colon + 1);
  unsigned int number = 0;
  const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
  return !port.empty() && parsed.ec == std::errc{} && parsed.ptr == port.data() + port.size() &&
         number > 0 && number <= 65535 && host.front() != '.' && host.back() != '.' &&
         std::all_of(host.begin(), host.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '.';
         });
}
bool bounded_config_string(const nlohmann::json& value, std::size_t maximum) {
  if (!value.is_string()) {
    return false;
  }
  const auto& text = value.get_ref<const std::string&>();
  return !text.empty() && text.size() <= maximum && text.find('\0') == std::string::npos;
}
bool symmetric_key_file_shape(const nlohmann::json& value) {
  return exact_keys(value, {"id", "file"}) && bounded_config_string(value["id"], 1024) &&
         bounded_config_string(value["file"], 4096);
}
bool identity_security_shape(const nlohmann::json& value) {
  if (!exact_keys(value, {"token_key", "request_key", "receipt_key", "delegation_key", "issuer",
                          "clock"}) ||
      !symmetric_key_file_shape(value["token_key"]) ||
      !symmetric_key_file_shape(value["request_key"]) ||
      !symmetric_key_file_shape(value["receipt_key"]) ||
      !bounded_config_string(value["issuer"], 1024) ||
      !exact_keys(value["delegation_key"], {"id", "public_key_file", "private_key_file"}) ||
      !bounded_config_string(value["delegation_key"]["id"], 1024) ||
      !bounded_config_string(value["delegation_key"]["public_key_file"], 4096) ||
      !bounded_config_string(value["delegation_key"]["private_key_file"], 4096) ||
      !exact_keys(value["clock"], {"trusted", "maximum_known_skew_ms"}) ||
      !value["clock"]["trusted"].is_boolean() || !value["clock"]["trusted"].get<bool>()) {
    return false;
  }
  const auto& skew = value["clock"]["maximum_known_skew_ms"];
  if (!skew.is_number_integer()) {
    return false;
  }
  if (skew.is_number_unsigned()) {
    return skew.get<std::uint64_t>() <= 2000;
  }
  return skew.get<std::int64_t>() >= 0 && skew.get<std::int64_t>() <= 2000;
}
IdentitySymmetricKeyFile symmetric_key_file(const nlohmann::json& value) {
  return {value["id"].get<std::string>(), value["file"].get<std::string>()};
}
IdentitySecurityMaterial identity_security_material(const nlohmann::json& value) {
  const auto& delegation = value["delegation_key"];
  return {symmetric_key_file(value["token_key"]),
          symmetric_key_file(value["request_key"]),
          symmetric_key_file(value["receipt_key"]),
          {delegation["id"].get<std::string>(), delegation["public_key_file"].get<std::string>(),
           delegation["private_key_file"].get<std::string>()},
          value["issuer"].get<std::string>(),
          {value["clock"]["trusted"].get<bool>(),
           value["clock"]["maximum_known_skew_ms"].get<std::uint32_t>()}};
}
bool valid_time(X509* certificate, std::time_t validation_time) {
  const std::unique_ptr<ASN1_TIME, decltype(&ASN1_TIME_free)> now(
      ASN1_TIME_set(nullptr, validation_time), ASN1_TIME_free);
  if (!now) {
    return false;
  }
  const auto not_before = ASN1_TIME_compare(X509_get0_notBefore(certificate), now.get());
  return (not_before == -1 || not_before == 0) &&
         ASN1_TIME_compare(X509_get0_notAfter(certificate), now.get()) == 1;
}
int no_password(char*, int, int, void*) {
  return 0;
}
bool validate_tls(const TlsMaterial& material, std::time_t validation_time) {
  using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
  using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
  using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
  using Store = std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)>;
  using Context = std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)>;
  const Bio cert_bio(
      BIO_new_mem_buf(material.certificate.data(), static_cast<int>(material.certificate.size())),
      BIO_free);
  const Bio key_bio(
      BIO_new_mem_buf(material.private_key.data(), static_cast<int>(material.private_key.size())),
      BIO_free);
  const Bio ca_bio(BIO_new_mem_buf(material.ca.data(), static_cast<int>(material.ca.size())),
                   BIO_free);
  if (!cert_bio || !key_bio || !ca_bio) {
    return false;
  }
  const Certificate leaf(PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr), X509_free);
  const Key key(PEM_read_bio_PrivateKey(key_bio.get(), nullptr, no_password, nullptr),
                EVP_PKEY_free);
  const Store store(X509_STORE_new(), X509_STORE_free);
  if (!leaf || !key || !store || !valid_time(leaf.get(), validation_time) ||
      X509_check_private_key(leaf.get(), key.get()) != 1) {
    return false;
  }
  bool has_ca = false;
  for (;;) {
    const Certificate ca(PEM_read_bio_X509(ca_bio.get(), nullptr, nullptr, nullptr), X509_free);
    if (!ca) {
      break;
    }
    if (!valid_time(ca.get(), validation_time) || X509_check_ca(ca.get()) <= 0 ||
        X509_STORE_add_cert(store.get(), ca.get()) != 1) {
      return false;
    }
    has_ca = true;
  }
  if (!has_ca) {
    return false;
  }
  // Keep chain ownership separate: stack only borrows certificates held by this vector.
  const auto free_stack = [](STACK_OF(X509) * stack) { sk_X509_free(stack); };
  const std::unique_ptr<STACK_OF(X509), decltype(free_stack)> chain(sk_X509_new_null(), free_stack);
  std::vector<Certificate> intermediates;
  if (!chain) {
    return false;
  }
  for (;;) {
    Certificate certificate(PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr),
                            X509_free);
    if (!certificate) {
      break;
    }
    if (!valid_time(certificate.get(), validation_time) ||
        sk_X509_push(chain.get(), certificate.get()) == 0) {
      return false;
    }
    intermediates.push_back(std::move(certificate));
  }
  const Context context(X509_STORE_CTX_new(), X509_STORE_CTX_free);
  if (!context || X509_STORE_CTX_init(context.get(), store.get(), leaf.get(), chain.get()) != 1) {
    return false;
  }
  X509_VERIFY_PARAM_set_time(X509_STORE_CTX_get0_param(context.get()), validation_time);
  return X509_STORE_CTX_set_purpose(context.get(), X509_PURPOSE_SSL_SERVER) == 1 &&
         X509_verify_cert(context.get()) == 1;
}

} // namespace

std::string_view service_name(target::Service service) noexcept {
  switch (service) {
  case target::Service::Identity:
    return "identity";
  case target::Service::Runtime:
    return "runtime";
  case target::Service::Host:
    return "plugin-host";
  case target::Service::Scheduler:
    return "scheduler";
  case target::Service::Model:
    return "model";
  case target::Service::Control:
    return "control";
  }
  return "unknown";
}
ConfigResult load_service_config(const std::string& path, target::Service service,
                                 target::TimePoint now) {
  nlohmann::json document;
  try {
    std::vector<std::set<std::string>> object_keys;
    bool duplicate = false;
    const auto callback = [&](int, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
      if (event == nlohmann::json::parse_event_t::object_start) {
        object_keys.emplace_back();
      }
      if (event == nlohmann::json::parse_event_t::key &&
          !object_keys.back().insert(parsed.get<std::string>()).second) {
        duplicate = true;
      }
      if (event == nlohmann::json::parse_event_t::object_end) {
        object_keys.pop_back();
      }
      return true;
    };
    document = nlohmann::json::parse(read_file(path), callback);
    std::set<std::string> keys{"schema_version", "service", "probe_listen", "tls"};
    if (service != target::Service::Control) {
      keys.insert("grpc_listen");
    }
    if (document.contains("control_api")) {
      if (service != target::Service::Control || !control_api_shape(document["control_api"])) {
        return ConfigFailure{2, "invalid_control_api_config"};
      }
      keys.insert("control_api");
    }
    if (document.contains("identity_security")) {
      if (service != target::Service::Identity || !document.contains("database") ||
          !identity_security_shape(document["identity_security"])) {
        return ConfigFailure{2, "invalid_identity_security_config"};
      }
      keys.insert("identity_security");
    }
    if (document.contains("database")) {
      if (service == target::Service::Control || service == target::Service::Host ||
          !database_shape(document["database"])) {
        return ConfigFailure{2, "invalid_database_config"};
      }
      keys.insert("database");
    }
    if (duplicate || !exact_keys(document, keys) ||
        !document["schema_version"].is_number_integer() || document["schema_version"] != 1 ||
        document["service"] != service_name(service) || !document["probe_listen"].is_string() ||
        !exact_keys(document["tls"], {"certificate_file", "private_key_file", "ca_file"})) {
      return ConfigFailure{2, "invalid_config"};
    }
    ProcessConfig config{
        service, document["probe_listen"].get<std::string>(), {}, {}, std::nullopt};
    if (service != target::Service::Control) {
      config.grpc_listen = document["grpc_listen"].get<std::string>();
    }
    if (!address_valid(config.probe_listen, true) ||
        (service != target::Service::Control && !address_valid(config.grpc_listen, false))) {
      return ConfigFailure{2, "invalid_address"};
    }
    if (document.contains("control_api")) {
      config.control_api =
          ControlApiMaterial{document["control_api"]["listen"].get<std::string>(),
                             document["control_api"]["identity_endpoint"].get<std::string>()};
    }
    if (document.contains("identity_security")) {
      config.identity_security = identity_security_material(document["identity_security"]);
    }
    const auto certificate_path = document["tls"]["certificate_file"].get<std::string>();
    const auto key_path = document["tls"]["private_key_file"].get<std::string>();
    const auto ca_path = document["tls"]["ca_file"].get<std::string>();
    try {
      config.tls = {read_file(certificate_path), read_file(key_path, true), read_file(ca_path)};
      if (!validate_tls(
              config.tls,
              std::chrono::system_clock::to_time_t(
                  std::chrono::time_point_cast<std::chrono::system_clock::duration>(now)))) {
        return ConfigFailure{1, "invalid_tls"};
      }
    } catch (const FileFailure&) {
      return ConfigFailure{1, "tls_file_unavailable"};
    }
    if (document.contains("database")) {
      const auto& database = document["database"];
      try {
        auto password = read_file(database["password_file"].get<std::string>(), true);
        if (password.size() > 4096) {
          return ConfigFailure{2, "invalid_database_password"};
        }
        if (!password.empty() && password.back() == '\n') {
          password.pop_back();
        }
        if (password.empty() || password.find_first_of("\r\n") != std::string::npos ||
            password.find('\0') != std::string::npos) {
          return ConfigFailure{2, "invalid_database_password"};
        }
        const auto ca_file = database["ca_file"].get<std::string>();
        (void)read_file(ca_file);
        config.database = DatabaseMaterial{database["host"].get<std::string>(),
                                           database["port"].get<std::uint16_t>(),
                                           database["name"].get<std::string>(),
                                           database["migration_role"].get<std::string>(),
                                           database["runtime_role"].get<std::string>(),
                                           std::move(password),
                                           ca_file};
      } catch (const FileFailure&) {
        return ConfigFailure{1, "database_file_unavailable"};
      }
    }
    return config;
  } catch (const FileFailure&) {
    return ConfigFailure{2, "config_unavailable"};
  } catch (const nlohmann::json::exception&) {
    return ConfigFailure{2, "invalid_config"};
  }
}

} // namespace kiarabot::core::platform
