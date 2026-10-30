#include "kiarabot/core/runtime/request_digest.hpp"

#include <array>
#include <stdexcept>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace kiarabot::core {
namespace {
std::string hex(const unsigned char* bytes, std::size_t count) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(count * 2);
  for (std::size_t index = 0; index < count; ++index) {
    result.push_back(digits[bytes[index] >> 4]);
    result.push_back(digits[bytes[index] & 15]);
  }
  return result;
}
} // namespace
std::string random_instance_id() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    throw std::runtime_error("instance generation failed");
  }
  return hex(bytes.data(), bytes.size());
}
std::string request_digest(const google::protobuf::MessageLite& message) {
  std::string serialized;
  {
    google::protobuf::io::StringOutputStream output(&serialized);
    google::protobuf::io::CodedOutputStream encoder(&output);
    encoder.SetSerializationDeterministic(true);
    if (!message.SerializeToCodedStream(&encoder) || encoder.HadError()) {
      throw std::runtime_error("request encoding failed");
    }
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (EVP_Digest(serialized.data(), serialized.size(), digest.data(), &size, EVP_sha256(),
                 nullptr) != 1 ||
      size != 32) {
    throw std::runtime_error("request digest failed");
  }
  return hex(digest.data(), size);
}
} // namespace kiarabot::core
