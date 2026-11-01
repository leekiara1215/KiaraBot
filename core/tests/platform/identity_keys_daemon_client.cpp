#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>
#include <openssl/pem.h>

#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/identity.grpc.pb.h"

namespace {
namespace t = kiarabot::core::target;
namespace w = kiarabot::v1;
namespace codec = kiarabot::core::platform::target_codec;
void require(bool condition) {
  if (!condition) {
    throw std::runtime_error("production Keys client assertion; diagnostics redacted");
  }
}
std::string file(const char* path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good());
  return {std::istreambuf_iterator<char>{input}, {}};
}
std::uint64_t number(std::string_view value) {
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  require(result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed > 0);
  return parsed;
}
std::array<unsigned char, 32> public_key(const std::string& pem) {
  require(!pem.empty() && pem.size() <= 8192);
  const std::unique_ptr<BIO, decltype(&BIO_free)> input(
      BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  require(static_cast<bool>(input));
  const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
      PEM_read_bio_PUBKEY(input.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
  require(key && EVP_PKEY_is_a(key.get(), "ED25519") == 1);
  std::array<unsigned char, 32> raw{};
  auto size = raw.size();
  require(EVP_PKEY_get_raw_public_key(key.get(), raw.data(), &size) == 1 && size == raw.size());
  return raw;
}
void context(w::RequestContext* output) {
  output->set_request_id("11111111-1111-4111-8111-111111111111");
  output->set_trace_id("22222222-2222-4222-8222-222222222222");
  const t::TimePoint limit{std::chrono::duration_cast<std::chrono::nanoseconds>(
      (std::chrono::system_clock::now() + std::chrono::seconds{2}).time_since_epoch())};
  *output->mutable_deadline() = std::get<google::protobuf::Timestamp>(codec::to_wire(limit));
  require(output->delegation().empty() && output->idempotency_key().empty());
}
void rejected(const grpc::Status& status, w::ErrorCode expected,
              const w::IdentityGetVerificationKeysResponse& response) {
  require(!status.ok() && response.keys().empty() && response.version() == 0);
  const auto decoded = codec::from_grpc_failure(status);
  require(std::holds_alternative<w::RpcFailure>(decoded));
  const auto& failure = std::get<w::RpcFailure>(decoded);
  require(failure.error().code() == expected && failure.has_context());
  require(failure.error().request_id() == "11111111-1111-4111-8111-111111111111");
  require(failure.error().trace_id() == "22222222-2222-4222-8222-222222222222");
}
} // namespace

int main(int argc, char* argv[]) {
  try {
    require(argc == 11);
    grpc::SslCredentialsOptions tls;
    tls.pem_root_certs = file(argv[2]);
    tls.pem_cert_chain = file(argv[3]);
    tls.pem_private_key = file(argv[4]);
    auto stub =
        w::IdentityService::NewStub(grpc::CreateChannel(argv[1], grpc::SslCredentials(tls)));
    const auto expected = public_key(file(argv[5]));
    const std::string expected_id = argv[6];
    const auto set_version = number(argv[7]);
    const auto key_version = number(argv[8]);
    const auto expiry = number(argv[9]);
    const std::string_view mode = argv[10];
    require(set_version > 1 && set_version < std::numeric_limits<std::uint64_t>::max() &&
            expiry <= 9223372036ULL &&
            (mode == "allowed" || mode == "denied" || mode == "unregistered"));
    const std::array<std::optional<std::uint64_t>, 5> versions{std::nullopt, set_version,
                                                               set_version - 1, set_version + 1, 0};
    for (const auto known : versions) {
      if (mode == "unregistered" && known && *known == 0) {
        continue; // Authentication precedence for malformed input is not this client's contract.
      }
      w::IdentityGetVerificationKeysRequest request;
      context(request.mutable_context());
      if (known) {
        request.set_known_key_version(*known);
      }
      grpc::ClientContext transport;
      transport.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{3});
      w::IdentityGetVerificationKeysResponse response;
      const auto status = stub->GetVerificationKeys(&transport, request, &response);
      if (mode != "allowed") {
        // Invalid zero input is a boundary rejection; a future nonzero version must never
        // disclose material or override the registered caller's missing exact Catalog grant.
        const auto expected_code = known && *known == 0 ? w::INVALID_ARGUMENT
                                   : mode == "denied"   ? w::PERMISSION_DENIED
                                                        : w::UNAUTHENTICATED;
        rejected(status, expected_code, response);
      } else if (known && (*known == 0 || *known > set_version)) {
        rejected(status, w::INVALID_ARGUMENT, response);
      } else {
        require(status.ok() && response.version() == set_version && response.keys_size() == 1);
        const auto& key = response.keys(0);
        require(key.key_id() == expected_id && key.version() == key_version &&
                key.ed25519_public_key().size() == expected.size() && key.has_valid_until());
        require(key.valid_until().seconds() == static_cast<std::int64_t>(expiry) &&
                key.valid_until().nanos() == 0);
        require(std::equal(expected.begin(), expected.end(), key.ed25519_public_key().begin(),
                           [](unsigned char left, char right) {
                             return left == static_cast<unsigned char>(right);
                           }));
      }
    }
    std::cout << "Production Identity Keys mTLS current-platform/Catalog authorization and exact "
                 "raw32/version/expiry checks passed without JWT\n";
    return 0;
  } catch (...) {
    std::cerr << "P03 assertion: production Keys client failed; diagnostics redacted\n";
    return 1;
  }
}
