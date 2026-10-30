#include <array>
#include <sodium.h>
#include <stdexcept>

#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/time_util.h>

#include "kiarabot/core/application/control_service.hpp"

namespace kiarabot::core {
namespace {
void initialize() {
  if (sodium_init() < 0) {
    throw std::runtime_error("crypto initialization failed");
  }
}
std::string hex(std::string_view bytes) {
  std::string result(bytes.size() * 2 + 1, '\0');
  sodium_bin2hex(result.data(), result.size(), reinterpret_cast<const unsigned char*>(bytes.data()),
                 bytes.size());
  result.pop_back();
  return result;
}
std::string unhex(std::string_view value) {
  std::string result(value.size() / 2, '\0');
  std::size_t length = 0;
  if (value.size() % 2 ||
      sodium_hex2bin(reinterpret_cast<unsigned char*>(result.data()), result.size(), value.data(),
                     value.size(), nullptr, &length, nullptr) != 0 ||
      length != result.size()) {
    throw std::invalid_argument("invalid crypto encoding");
  }
  return result;
}
std::string key_bytes(std::string_view key) {
  initialize();
  auto bytes = unhex(key);
  if (bytes.size() != crypto_secretbox_KEYBYTES) {
    throw std::invalid_argument("invalid key size");
  }
  return bytes;
}
} // namespace
std::string control_password_hash(std::string_view password) {
  initialize();
  std::array<char, crypto_pwhash_STRBYTES> hash{};
  if (crypto_pwhash_str_alg(hash.data(), password.data(), password.size(),
                            crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE,
                            crypto_pwhash_ALG_ARGON2ID13) != 0) {
    throw std::runtime_error("password hashing failed");
  }
  return hash.data();
}
bool control_password_verify(std::string_view hash, std::string_view password) {
  initialize();
  return hash.starts_with("$argon2id$") &&
         crypto_pwhash_str_verify(std::string(hash).c_str(), password.data(), password.size()) == 0;
}
std::string control_secret() {
  initialize();
  std::string result(32, '\0');
  randombytes_buf(result.data(), result.size());
  return hex(result);
}
std::string control_digest(std::string_view value, std::string_view key) {
  const auto bytes = key_bytes(key);
  std::string result(32, '\0');
  if (crypto_generichash(reinterpret_cast<unsigned char*>(result.data()), result.size(),
                         reinterpret_cast<const unsigned char*>(value.data()), value.size(),
                         reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()) != 0) {
    throw std::runtime_error("digest failed");
  }
  return hex(result);
}
std::string control_encrypt(std::string_view value, std::string_view key) {
  const auto bytes = key_bytes(key);
  std::string result(crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + value.size(), '\0');
  auto* buffer = reinterpret_cast<unsigned char*>(result.data());
  randombytes_buf(buffer, crypto_secretbox_NONCEBYTES);
  if (crypto_secretbox_easy(buffer + crypto_secretbox_NONCEBYTES,
                            reinterpret_cast<const unsigned char*>(value.data()), value.size(),
                            buffer, reinterpret_cast<const unsigned char*>(bytes.data())) != 0) {
    throw std::runtime_error("encryption failed");
  }
  return hex(result);
}
std::string control_decrypt(std::string_view value, std::string_view key) {
  const auto bytes = key_bytes(key);
  const auto decoded = unhex(value);
  if (decoded.size() < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES) {
    throw std::runtime_error("invalid ciphertext");
  }
  std::string result(decoded.size() - crypto_secretbox_NONCEBYTES - crypto_secretbox_MACBYTES,
                     '\0');
  const auto* buffer = reinterpret_cast<const unsigned char*>(decoded.data());
  if (crypto_secretbox_open_easy(reinterpret_cast<unsigned char*>(result.data()),
                                 buffer + crypto_secretbox_NONCEBYTES,
                                 decoded.size() - crypto_secretbox_NONCEBYTES, buffer,
                                 reinterpret_cast<const unsigned char*>(bytes.data())) != 0) {
    throw std::runtime_error("authentication failed");
  }
  return result;
}
ControlJson control_json(const google::protobuf::Message& message) {
  std::string result;
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  if (!google::protobuf::util::MessageToJsonString(message, &result, options).ok()) {
    throw std::runtime_error("JSON encoding failed");
  }
  return ControlJson::parse(result);
}
google::protobuf::Struct control_struct(const ControlJson& value) {
  google::protobuf::Struct result;
  if (!google::protobuf::util::JsonStringToMessage(value.dump(), &result).ok()) {
    throw std::runtime_error("JSON conversion failed");
  }
  return result;
}
std::string control_timestamp(WallTime value) {
  return google::protobuf::util::TimeUtil::ToString(
      google::protobuf::util::TimeUtil::MillisecondsToTimestamp(
          std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count()));
}
std::vector<std::string> control_permissions(std::string_view role) {
  std::vector<std::string> result;
  for (const auto* permission :
       {"session.read", "session.revoke", "health.read", "adapter.read", "plugin.read", "bot.read",
        "model.read", "group.read", "task.read", "action.read", "plugin.manage",
        "plugin.config.read", "plugin.config.write", "log.read", "audit.read", "user.read"}) {
    if (control_role_allows(role, permission)) {
      result.emplace_back(permission);
    }
  }
  return result;
}
bool control_plugin_enabled(const RuntimeSnapshot& snapshot, const std::string& plugin_id) {
  const auto found = snapshot.namespaces.find("$control.enabled");
  if (found == snapshot.namespaces.end()) {
    return true;
  }
  const auto entry = found->second.find(plugin_id);
  return entry == found->second.end() || entry->second.value == "true";
}
} // namespace kiarabot::core
