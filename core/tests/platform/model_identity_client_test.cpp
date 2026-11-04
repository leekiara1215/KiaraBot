// P11c Model caller verification boundary tests: structural delegation decode + online Identity
// CheckAuthorization. No TLS peer is fabricated: decode is tested directly and authorize uses a
// loopback fake Identity server.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/runtime/platform/model_identity_client.hpp"
#include "kiarabot/v1/identity.grpc.pb.h"

namespace {
namespace p = kiarabot::core::platform;
namespace t = kiarabot::core::target;
using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error("P11c identity assertion: " + std::string{message});
  }
}
std::string base64url(std::string_view value) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string output;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const unsigned char character : value) {
    buffer = (buffer << 8U) | character;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      output.push_back(kAlphabet[(buffer >> static_cast<unsigned>(bits)) & 0x3fU]);
    }
  }
  if (bits > 0) {
    output.push_back(kAlphabet[(buffer << static_cast<unsigned>(6 - bits)) & 0x3fU]);
  }
  return output;
}
std::string uuid(int value) {
  char buffer[37];
  static constexpr char kHex[] = "0123456789abcdef";
  for (int index = 35; index >= 0; --index) {
    buffer[index] =
        (index == 8 || index == 13 || index == 18 || index == 23) ? '-' : kHex[value & 0xf];
    value >>= 4;
  }
  buffer[14] = '4';
  buffer[19] = '8';
  buffer[36] = 0;
  return buffer;
}
std::string hex(const t::Sha256Digest& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  for (const auto byte : digest.bytes) {
    const auto value = std::to_integer<unsigned>(byte);
    output.push_back(kHex[value >> 4U]);
    output.push_back(kHex[value & 0x0fU]);
  }
  return output;
}
t::Sha256Digest digest(std::byte seed) {
  t::Sha256Digest value;
  for (std::size_t index = 0; index < value.bytes.size(); ++index) {
    value.bytes[index] = static_cast<std::byte>(
        (static_cast<unsigned>(seed) + static_cast<unsigned>(index)) & 0xffU);
  }
  return value;
}
std::string proof(const t::Sha256Digest& leaf, std::string_view audience = "model",
                  std::string_view direct_actor = uuid(0x200000), int hops = 2,
                  bool expired = false,
                  std::optional<std::string> certificate_override = std::nullopt) {
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const auto iat = now + (expired ? -120 : -1);
  const auto exp = iat + 30;
  const auto certificate = certificate_override.value_or(hex(leaf));
  std::string chain = "\"" + std::string{hops == 2 ? uuid(0x100000) : uuid(0x200000)} + "\"";
  if (hops == 2) {
    chain += ",\"" + std::string{direct_actor} + "\"";
  }
  const std::string payload =
      "{\"iss\":\"" + uuid(0x900000) + "\",\"sub\":\"" + uuid(0x300000) + "\",\"aud\":\"" +
      std::string{audience} + "\",\"jti\":\"" + uuid(0x400000) +
      "\",\"iat\":" + std::to_string(iat) + ",\"nbf\":" + std::to_string(iat) +
      ",\"exp\":" + std::to_string(exp) + ",\"delegation\":{\"direct_actor\":\"" +
      std::string{direct_actor} + "\",\"actor_chain\":[" + chain +
      "],\"permissions\":[],\"client_certificate\":\"" + certificate +
      "\",\"authorization_version\":\"7\",\"business_hops\":" + std::to_string(hops) + "}}";
  return "eyJhbGciOiJub25lIn0." + base64url(payload) + ".";
}
t::ModelCaller runtime_caller(std::string method = "/kiarabot.v1.ModelService/Chat") {
  t::ModelCaller caller;
  caller.subject = t::SubjectId{uuid(0x300000)};
  caller.owner = t::InstanceId{uuid(0x300000)};
  caller.direct_actor = t::SubjectId{uuid(0x200000)};
  caller.runtime_origin = true;
  caller.delegation = "proof";
  caller.method = std::move(method);
  return caller;
}

class FakeIdentity final : public kiarabot::v1::IdentityService::Service {
public:
  grpc::Status
  CheckAuthorization(grpc::ServerContext*,
                     const kiarabot::v1::IdentityCheckAuthorizationRequest* request,
                     kiarabot::v1::IdentityCheckAuthorizationResponse* response) override {
    std::lock_guard lock(mutex_);
    subject_proof_ = request->subject_proof();
    full_method_ = request->full_method();
    calls_++;
    if (delay_.count() > 0) {
      std::this_thread::sleep_for(delay_);
    }
    if (unavailable_) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "");
    }
    auto* decision = response->mutable_decision();
    decision->set_authorization_version(9);
    const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    decision->mutable_checked_at()->set_seconds(stamp / 1'000'000'000);
    decision->mutable_checked_at()->set_nanos(static_cast<int>(stamp % 1'000'000'000));
    if (allowed_) {
      decision->set_allowed(true);
    } else {
      decision->set_allowed(false);
      auto* denial = decision->mutable_denial();
      denial->set_code(kiarabot::v1::ErrorCode::PERMISSION_DENIED);
      denial->set_message("denied");
    }
    return grpc::Status::OK;
  }
  int calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
  }
  std::string subject_proof() const {
    std::lock_guard lock(mutex_);
    return subject_proof_;
  }
  std::string full_method() const {
    std::lock_guard lock(mutex_);
    return full_method_;
  }
  void set_allowed(bool value) {
    allowed_ = value;
  }
  void set_unavailable(bool value) {
    unavailable_ = value;
  }
  void set_delay(std::chrono::milliseconds value) {
    delay_ = value;
  }

private:
  mutable std::mutex mutex_;
  int calls_ = 0;
  bool allowed_ = true;
  bool unavailable_ = false;
  std::chrono::milliseconds delay_{0};
  std::string subject_proof_;
  std::string full_method_;
};

struct Server {
  explicit Server(FakeIdentity& fake) {
    builder_.RegisterService(&fake);
    builder_.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    server_ = builder_.BuildAndStart();
    require(server_ != nullptr && port_ > 0, "fake identity start");
  }
  ~Server() {
    if (server_) {
      server_->Shutdown(std::chrono::system_clock::now() + 2s);
      server_->Wait();
    }
  }
  std::string endpoint() const {
    return "127.0.0.1:" + std::to_string(port_);
  }
  grpc::ServerBuilder builder_;
  std::unique_ptr<grpc::Server> server_;
  int port_ = 0;
};
std::unique_ptr<p::ModelIdentityClient> client_for(const std::string& endpoint) {
  auto created =
      p::ModelIdentityClient::create({endpoint, grpc::InsecureChannelCredentials(), 500ms, 1000ms});
  require(std::holds_alternative<std::unique_ptr<p::ModelIdentityClient>>(created),
          "client create");
  return std::move(std::get<std::unique_ptr<p::ModelIdentityClient>>(created));
}

void decode_cases() {
  const auto leaf = digest(std::byte{7});
  const auto now = t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
  const auto valid = p::decode_model_delegation(proof(leaf), leaf, now);
  require(std::holds_alternative<t::ModelCaller>(valid), "valid delegation decodes");
  const auto& caller = std::get<t::ModelCaller>(valid);
  require(caller.delegation_expires_at && *caller.delegation_expires_at > now,
          "proof expiry preserved");
  require(caller.subject && caller.owner && caller.direct_actor && caller.runtime_origin &&
              caller.delegation.find('.') != std::string::npos,
          "runtime caller derives owner");
  // Control-issued (one hop) is a management origin, never a plugin owner.
  const auto control =
      p::decode_model_delegation(proof(leaf, "model", uuid(0x200000), 1), leaf, now);
  require(std::holds_alternative<t::ModelCaller>(control), "control delegation decodes");
  require(!std::get<t::ModelCaller>(control).runtime_origin &&
              !std::get<t::ModelCaller>(control).owner,
          "control delegation has no plugin owner");
  const auto expired =
      p::decode_model_delegation(proof(leaf, "model", uuid(0x200000), 2, true), leaf, now);
  require(std::holds_alternative<t::Failure>(expired), "expired rejected");
  const auto wrong_audience =
      p::decode_model_delegation(proof(leaf, "runtime", uuid(0x200000), 2), leaf, now);
  require(std::holds_alternative<t::Failure>(wrong_audience), "wrong audience rejected");
  const auto certificate = p::decode_model_delegation(
      proof(leaf, "model", uuid(0x200000), 2, false, hex(digest(std::byte{8}))), leaf, now);
  require(std::holds_alternative<t::Failure>(certificate), "certificate binding rejected");
  const auto runtime_method = runtime_caller();
  require(runtime_method.runtime_origin && runtime_method.owner, "runtime caller shape");
}

void authorize_cases() {
  FakeIdentity fake;
  Server server(fake);
  auto client = client_for(server.endpoint());
  const t::ResourceRef resource{t::ResourceKind::Model, {uuid(0x500000)}};
  auto caller = runtime_caller();
  auto allowed = client->authorize(caller, t::Permission::ModelChat, resource, 500ms);
  if (const auto* failure = std::get_if<t::Failure>(&allowed)) {
    std::cerr << "authorize failure code=" << static_cast<int>(failure->code)
              << " recovery=" << static_cast<int>(failure->recovery) << '\n';
  }
  require(std::holds_alternative<t::ModelAuthorizationDecision>(allowed), "allowed decision");
  require(std::get<t::ModelAuthorizationDecision>(allowed).authorization_version.value == 9,
          "authorization version");
  require(fake.calls() == 1 && fake.full_method() == "/kiarabot.v1.ModelService/Chat" &&
              fake.subject_proof() == "proof",
          "authorize sends proof/method once");
  auto streaming = client->authorize(runtime_caller("/kiarabot.v1.ModelService/ChatStream"),
                                     t::Permission::ModelChat, resource, 500ms);
  require(std::holds_alternative<t::ModelAuthorizationDecision>(streaming),
          "Runtime ChatStream allowed");
  const auto before_rejected = fake.calls();
  for (const auto method :
       {"/kiarabot.v1.ModelService/GetCallStatus", "/kiarabot.v1.ModelService/ListOperationChanges",
        "/kiarabot.v1.ModelService/FutureMethod"}) {
    auto denied =
        client->authorize(runtime_caller(method), t::Permission::ResourceRead, resource, 500ms);
    require(std::holds_alternative<t::Failure>(denied) &&
                std::get<t::Failure>(denied).code == t::ErrorCode::PermissionDenied,
            "Runtime Model method set is explicit");
  }
  require(fake.calls() == before_rejected, "unsupported Runtime methods never reach Identity");
  // A Runtime-origin caller cannot invoke a management method; a nested origin cannot Chat.
  auto management = client->authorize(runtime_caller("/kiarabot.v1.ModelService/UpdateModel"),
                                      t::Permission::ModelManage, resource, 500ms);
  require(std::holds_alternative<t::Failure>(management) &&
              std::get<t::Failure>(management).code == t::ErrorCode::PermissionDenied,
          "runtime caller cannot manage");
  t::ModelCaller control;
  control.subject = t::SubjectId{uuid(0x100000)};
  control.direct_actor = t::SubjectId{uuid(0x100000)};
  control.delegation = "proof";
  control.method = "/kiarabot.v1.ModelService/UpdateModel";
  auto control_allowed = client->authorize(control, t::Permission::ModelManage, resource, 500ms);
  require(std::holds_alternative<t::ModelAuthorizationDecision>(control_allowed),
          "control management allowed");
  // Revocation: an authoritative deny is surfaced, not retried into an allow.
  fake.set_allowed(false);
  auto denied = client->authorize(caller, t::Permission::ModelChat, resource, 500ms);
  require(std::holds_alternative<t::Failure>(denied) &&
              std::get<t::Failure>(denied).code == t::ErrorCode::PermissionDenied,
          "revoked delegation denied");
  // Dependency failure is fail-closed and never becomes an allow.
  fake.set_allowed(true);
  fake.set_unavailable(true);
  auto unavailable = client->authorize(caller, t::Permission::ModelChat, resource, 500ms);
  require(std::holds_alternative<t::Failure>(unavailable) &&
              std::get<t::Failure>(unavailable).code == t::ErrorCode::UpstreamUnavailable &&
              std::get<t::Failure>(unavailable).acceptance == t::Acceptance::NotAccepted,
          "identity dependency failure closed");
  // No deadline left is a timeout, not an implicit allow.
  fake.set_unavailable(false);
  auto timeout = client->authorize(caller, t::Permission::ModelChat, resource, 0ms);
  require(std::holds_alternative<t::Failure>(timeout) &&
              std::get<t::Failure>(timeout).code == t::ErrorCode::Timeout,
          "expired budget closed");
  // Missing method/delegation cannot reach Identity at all.
  auto anonymous = caller;
  anonymous.delegation.clear();
  auto unauthenticated = client->authorize(anonymous, t::Permission::ModelChat, resource, 500ms);
  require(std::holds_alternative<t::Failure>(unauthenticated) &&
              std::get<t::Failure>(unauthenticated).code == t::ErrorCode::Unauthenticated,
          "missing proof closed");
}

} // namespace

int main() {
  try {
    decode_cases();
    authorize_cases();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "model identity client: ok\n";
  return 0;
}
