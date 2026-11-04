#include "kiarabot/core/domain/target/observation_feed_identity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <openssl/sha.h>

namespace kiarabot::core::target {
namespace {
// Fixed in-repo namespace values for the deterministic feed identity. These are contract
// constants, not secrets: never change them without a documented feed identity version bump.
constexpr std::array<unsigned char, 16> kMessageFeedNamespace{
    0x5f, 0x0e, 0x4c, 0x8a, 0x2b, 0x1d, 0x4e, 0x6f, 0x9a, 0x73, 0xc1, 0xd2, 0xe3, 0xf4, 0x05, 0x16};
constexpr std::array<unsigned char, 16> kConversationFeedNamespace{
    0x7a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x4a, 0x6b, 0x8c, 0x9d, 0x0e, 0x1f, 0x2a, 0x3b, 0x4c, 0x5d};
constexpr char kComponentSeparator = '\x1f';

std::string uuid_v5(const std::array<unsigned char, 16>& name_space, const std::string& name) {
  std::array<unsigned char, 16> namespace_and_name{};
  std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
  SHA_CTX context;
  SHA1_Init(&context);
  SHA1_Update(&context, name_space.data(), name_space.size());
  SHA1_Update(&context, name.data(), name.size());
  SHA1_Final(digest.data(), &context);
  for (std::size_t index = 0; index < namespace_and_name.size(); ++index) {
    namespace_and_name[index] = digest[index];
  }
  namespace_and_name[6] = static_cast<unsigned char>((namespace_and_name[6] & 0x0fU) | 0x50U);
  namespace_and_name[8] = static_cast<unsigned char>((namespace_and_name[8] & 0x3fU) | 0x80U);
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(36);
  for (std::size_t index = 0; index < namespace_and_name.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result += '-';
    }
    result += kHex[namespace_and_name[index] >> 4U];
    result += kHex[namespace_and_name[index] & 0x0fU];
  }
  return result;
}
std::string feed_name(std::string_view account_id, std::string_view adapter_instance_id,
                      std::string_view platform_id) {
  std::string name;
  name.reserve(account_id.size() + adapter_instance_id.size() + platform_id.size() + 2);
  name.append(account_id);
  name += kComponentSeparator;
  name.append(adapter_instance_id);
  name += kComponentSeparator;
  name.append(platform_id);
  return name;
}
} // namespace

std::string message_feed_id(std::string_view account_id, std::string_view adapter_instance_id,
                            std::string_view platform_event_id) {
  return uuid_v5(kMessageFeedNamespace,
                 feed_name(account_id, adapter_instance_id, platform_event_id));
}
std::string conversation_feed_id(std::string_view account_id, std::string_view adapter_instance_id,
                                 std::string_view platform_conversation_id) {
  return uuid_v5(kConversationFeedNamespace,
                 feed_name(account_id, adapter_instance_id, platform_conversation_id));
}

} // namespace kiarabot::core::target
