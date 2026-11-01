#include "kiarabot/core/runtime/platform/service_config.hpp"

#include <array>
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
  explicit File(const std::string& path) : descriptor_(open(path.c_str(), O_RDONLY | O_NOFOLLOW)) {}
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
  struct stat status{};
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
bool valid_time(X509* certificate) {
  const std::unique_ptr<ASN1_TIME, decltype(&ASN1_TIME_free)> now(
      ASN1_TIME_set(nullptr, std::time(nullptr)), ASN1_TIME_free);
  return now && ASN1_TIME_compare(X509_get0_notBefore(certificate), now.get()) == -1 &&
         ASN1_TIME_compare(X509_get0_notAfter(certificate), now.get()) == 1;
}
int no_password(char*, int, int, void*) {
  return 0;
}
bool validate_tls(const TlsMaterial& material) {
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
  if (!leaf || !key || !store || !valid_time(leaf.get()) ||
      X509_check_private_key(leaf.get(), key.get()) != 1) {
    return false;
  }
  bool has_ca = false;
  for (;;) {
    const Certificate ca(PEM_read_bio_X509(ca_bio.get(), nullptr, nullptr, nullptr), X509_free);
    if (!ca) {
      break;
    }
    if (!valid_time(ca.get()) || X509_check_ca(ca.get()) <= 0 ||
        X509_STORE_add_cert(store.get(), ca.get()) != 1) {
      return false;
    }
    has_ca = true;
  }
  if (!has_ca) {
    return false;
  }
  // Keep chain ownership separate: stack only borrows certificates held by this vector.
  const auto free_stack = [](STACK_OF(X509)* stack) { sk_X509_free(stack); };
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
    if (!valid_time(certificate.get()) || sk_X509_push(chain.get(), certificate.get()) == 0) {
      return false;
    }
    intermediates.push_back(std::move(certificate));
  }
  const Context context(X509_STORE_CTX_new(), X509_STORE_CTX_free);
  return context && X509_STORE_CTX_init(context.get(), store.get(), leaf.get(), chain.get()) == 1 &&
         X509_STORE_CTX_set_purpose(context.get(), X509_PURPOSE_SSL_SERVER) == 1 &&
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
ConfigResult load_service_config(const std::string& path, target::Service service) {
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
    if (duplicate || !exact_keys(document, keys) ||
        !document["schema_version"].is_number_integer() || document["schema_version"] != 1 ||
        document["service"] != service_name(service) || !document["probe_listen"].is_string() ||
        !exact_keys(document["tls"], {"certificate_file", "private_key_file", "ca_file"})) {
      return ConfigFailure{2, "invalid_config"};
    }
    ProcessConfig config{service, document["probe_listen"].get<std::string>(), {}, {}};
    if (service != target::Service::Control) {
      config.grpc_listen = document["grpc_listen"].get<std::string>();
    }
    if (!address_valid(config.probe_listen, true) ||
        (service != target::Service::Control && !address_valid(config.grpc_listen, false))) {
      return ConfigFailure{2, "invalid_address"};
    }
    const auto certificate_path = document["tls"]["certificate_file"].get<std::string>();
    const auto key_path = document["tls"]["private_key_file"].get<std::string>();
    const auto ca_path = document["tls"]["ca_file"].get<std::string>();
    try {
      config.tls = {read_file(certificate_path), read_file(key_path, true), read_file(ca_path)};
      if (!validate_tls(config.tls)) {
        return ConfigFailure{1, "invalid_tls"};
      }
    } catch (const FileFailure&) {
      return ConfigFailure{1, "tls_file_unavailable"};
    }
    return config;
  } catch (const FileFailure&) {
    return ConfigFailure{2, "config_unavailable"};
  } catch (const nlohmann::json::exception&) {
    return ConfigFailure{2, "invalid_config"};
  }
}

} // namespace kiarabot::core::platform
