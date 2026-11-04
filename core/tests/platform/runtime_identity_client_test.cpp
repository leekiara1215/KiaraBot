#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include "kiarabot/core/runtime/platform/runtime_identity_client.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/identity.grpc.pb.h"

namespace {
namespace t = kiarabot::core::target;
namespace v1 = kiarabot::v1;
using Json = nlohmann::json;

void check(bool passed, const char* scenario) {
  if (!passed) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}
bool canonical_uuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
      continue;
    }
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}
constexpr auto kSubject = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
constexpr auto kActor = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
constexpr auto kToken = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
constexpr auto kResource = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
std::string base64url(std::string_view input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string output;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char character : input) {
    buffer = (buffer << 8U) | static_cast<unsigned char>(character);
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      output += kAlphabet[(buffer >> static_cast<unsigned>(bits)) & 0x3fU];
    }
  }
  if (bits > 0) {
    output += kAlphabet[(buffer << static_cast<unsigned>(6 - bits)) & 0x3fU];
  }
  return output;
}
t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
std::int64_t seconds(t::TimePoint value) {
  return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}
t::Sha256Digest digest() {
  t::Sha256Digest value;
  for (std::size_t index = 0; index < value.bytes.size(); ++index) {
    value.bytes[index] = static_cast<std::byte>(index);
  }
  return value;
}
std::string hex(const t::Sha256Digest& value) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : value.bytes) {
    result += kDigits[std::to_integer<unsigned>(byte) >> 4U];
    result += kDigits[std::to_integer<unsigned>(byte) & 0x0fU];
  }
  return result;
}
std::string token(t::TimePoint now, const std::string& audience, const std::string& certificate) {
  Json delegation{{"direct_actor", kActor},
                  {"actor_chain", Json::array({kActor})},
                  {"permissions", Json::array({Json::array({"1", "1", kResource})})},
                  {"client_certificate", certificate},
                  {"authorization_version", "5"},
                  {"business_hops", "0"},
                  {"source_credential", Json::array({"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", "1"})},
                  {"source_session", nullptr}};
  Json payload{{"iss", "identity"},   {"sub", kSubject},         {"aud", audience},
               {"iat", seconds(now)}, {"nbf", seconds(now) - 1}, {"exp", seconds(now) + 30},
               {"jti", kToken},       {"delegation", delegation}};
  return base64url(Json{{"alg", "EdDSA"}, {"typ", "JWT"}}.dump()) + "." +
         base64url(payload.dump()) + ".c2ln";
}
void decode_tests() {
  const auto leaf = digest();
  // Metadata decoding remains certificate/audience-bound. The observation RPC
  // separately requires the actual Control SAN and online Identity authorization.
  const auto model = kiarabot::core::platform::decode_delegation_claims(
      token(now(), "model", hex(leaf)), leaf, now(), t::Service::Model);
  check(std::holds_alternative<t::AuthenticationContext>(model),
        "Model delegation metadata rejected");
  const auto& model_context = std::get<t::AuthenticationContext>(model);
  const auto* model_peer = std::get_if<t::PlatformPeer>(&model_context.direct_peer);
  check(model_context.audience == t::Service::Model && model_peer &&
            model_peer->service == t::Service::Control && model_peer->subject.value == kActor,
        "Model delegation metadata lost audience/Control actor binding");
  for (const auto& invalid :
       {token(now(), "runtime", hex(leaf)), token(now(), "scheduler", hex(leaf)),
        token(now(), "model", std::string(64, 'f')),
        token(now() - std::chrono::minutes{2}, "model", hex(leaf))}) {
    check(std::holds_alternative<t::Failure>(kiarabot::core::platform::decode_delegation_claims(
              invalid, leaf, now(), t::Service::Model)),
          "Model metadata accepted foreign audience/certificate/expired delegation");
  }
  check(std::holds_alternative<t::Failure>(kiarabot::core::platform::decode_delegation_claims(
            token(now(), "model", hex(leaf)), leaf, now())),
        "Runtime decoder accepted a Model delegation");
  const auto proof = token(now(), "runtime", hex(leaf));
  auto decoded = kiarabot::core::platform::decode_delegation_claims(proof, leaf, now());
  check(std::holds_alternative<t::AuthenticationContext>(decoded), "valid delegation rejected");
  const auto& context = std::get<t::AuthenticationContext>(decoded);
  check(context.subject.value == kSubject && context.audience == t::Service::Runtime &&
            context.authorization_version.value == 5 && context.scopes.size() == 1 &&
            context.scopes.front().resource.id.value == kResource && context.proof == proof,
        "valid delegation projections lost");
  const auto* peer = std::get_if<t::PlatformPeer>(&context.direct_peer);
  check(peer != nullptr && peer->service == t::Service::Control && peer->subject.value == kActor,
        "delegation direct actor lost");
  check(std::holds_alternative<t::Failure>(kiarabot::core::platform::decode_delegation_claims(
            token(now(), "scheduler", hex(leaf)), leaf, now())),
        "foreign audience delegation accepted");
  check(std::holds_alternative<t::Failure>(kiarabot::core::platform::decode_delegation_claims(
            token(now(), "runtime", hex(leaf)), t::Sha256Digest{}, now())),
        "delegation accepted without matching inbound certificate");
  const auto expired = token(now() - std::chrono::seconds{120}, "runtime", hex(leaf));
  check(std::holds_alternative<t::Failure>(
            kiarabot::core::platform::decode_delegation_claims(expired, leaf, now())),
        "expired delegation accepted");
  check(std::holds_alternative<t::Failure>(
            kiarabot::core::platform::decode_delegation_claims("not-a-jwt", leaf, now())),
        "malformed delegation accepted");
}
class FakeIdentity final : public v1::IdentityService::Service {
public:
  explicit FakeIdentity(bool allow, std::chrono::milliseconds exchange_delay = {})
      : allow_(allow), exchange_delay_(exchange_delay) {}
  grpc::Status CheckAuthorization(grpc::ServerContext*,
                                  const v1::IdentityCheckAuthorizationRequest* request,
                                  v1::IdentityCheckAuthorizationResponse* response) override {
    last_proof = request->subject_proof();
    last_method = request->full_method();
    auto* decision = response->mutable_decision();
    decision->set_allowed(allow_);
    decision->set_authorization_version(9);
    const auto encoded = kiarabot::core::platform::target_codec::to_wire(now());
    if (const auto* wire = std::get_if<google::protobuf::Timestamp>(&encoded)) {
      *decision->mutable_checked_at() = *wire;
    }
    if (!allow_) {
      auto* denial = decision->mutable_denial();
      denial->set_code(v1::PERMISSION_DENIED);
      denial->set_request_id(request->context().request_id());
      denial->set_trace_id(request->context().trace_id());
      denial->set_message("denied");
    }
    return grpc::Status::OK;
  }
  grpc::Status ExchangeDelegation(grpc::ServerContext*,
                                  const v1::IdentityExchangeDelegationRequest* request,
                                  v1::IdentityExchangeDelegationResponse* response) override {
    ++exchanges;
    exchange_request = *request;
    if (exchange_delay_.count() > 0) {
      std::this_thread::sleep_for(exchange_delay_);
    }
    auto* proof = response->mutable_delegation();
    proof->set_proof("child-proof");
    proof->set_audience(request->audience());
    proof->set_business_hops(2);
    const auto encoded =
        kiarabot::core::platform::target_codec::to_wire(now() + std::chrono::seconds{30});
    if (const auto* wire = std::get_if<google::protobuf::Timestamp>(&encoded)) {
      *proof->mutable_expires_at() = *wire;
    }
    return grpc::Status::OK;
  }
  std::string last_proof;
  std::string last_method;
  int exchanges = 0;
  v1::IdentityExchangeDelegationRequest exchange_request;

private:
  bool allow_;
  std::chrono::milliseconds exchange_delay_;
};
struct Server {
  explicit Server(bool allow, std::chrono::milliseconds exchange_delay = {})
      : service(allow, exchange_delay) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
    check(server != nullptr && port > 0, "fake identity server failed");
  }
  ~Server() {
    if (server) {
      server->Shutdown();
    }
  }
  FakeIdentity service;
  int port = 0;
  std::unique_ptr<grpc::Server> server;
};
std::unique_ptr<kiarabot::core::platform::RuntimeIdentityClient> client(int port) {
  auto created = kiarabot::core::platform::RuntimeIdentityClient::create(
      {"127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials(),
       std::chrono::milliseconds{500}, std::chrono::milliseconds{1000}});
  check(std::holds_alternative<std::unique_ptr<kiarabot::core::platform::RuntimeIdentityClient>>(
            created),
        "identity client construction failed");
  return std::move(
      std::get<std::unique_ptr<kiarabot::core::platform::RuntimeIdentityClient>>(created));
}
t::RuntimeAuthorizationRequest request(std::string proof, t::TimePoint deadline) {
  t::AuthenticationContext authentication;
  authentication.subject = {kSubject};
  authentication.direct_peer = t::PlatformPeer{t::Service::Control, {kActor}};
  authentication.audience = t::Service::Runtime;
  authentication.authorization_version = {5};
  authentication.verified_at = now();
  authentication.expires_at = now() + std::chrono::seconds{30};
  authentication.proof = std::move(proof);
  return {authentication,
          t::Permission::AccountRead,
          "kiarabot.v1.RuntimeService/GetAccount",
          {t::ResourceRef{t::ResourceKind::Account, {kResource}}},
          deadline};
}
void check_tests() {
  {
    Server server(true);
    auto identity = client(server.port);
    auto decision = identity->check(
        request(token(now(), "runtime", hex(digest())), now() + std::chrono::seconds{2}));
    check(std::holds_alternative<t::RuntimeAuthorizationDecision>(decision),
          "allowed online authorization failed");
    const auto& value = std::get<t::RuntimeAuthorizationDecision>(decision);
    check(value.allowed && value.authorization_version.value == 9 &&
              value.subject.value == kSubject && value.permission == t::Permission::AccountRead &&
              value.scope.size() == 1,
          "authorization decision binding lost");
    check(!server.service.last_proof.empty() &&
              server.service.last_method == "kiarabot.v1.RuntimeService/GetAccount",
          "online check did not forward proof/method");
    const auto deadline = now() + std::chrono::seconds{2};
    check(std::holds_alternative<t::Failure>(identity->check(request({}, deadline))),
          "authorization without a delegation proof was accepted");
  }
  {
    Server server(false);
    auto identity = client(server.port);
    check(std::holds_alternative<t::Failure>(identity->check(
              request(token(now(), "runtime", hex(digest())), now() + std::chrono::seconds{2}))) &&
              std::get<t::Failure>(identity->check(request(token(now(), "runtime", hex(digest())),
                                                           now() + std::chrono::seconds{2})))
                      .code == t::ErrorCode::PermissionDenied,
          "denied online authorization was treated as a grant");
  }
  {
    Server server(true);
    auto identity = client(server.port);
    const auto exchanged =
        identity->exchange_delegation("parent-proof", t::Service::Scheduler,
                                      {t::ResourceRef{t::ResourceKind::Instance, {kResource}}},
                                      {t::Permission::TaskSchedule}, digest());
    check(std::holds_alternative<kiarabot::core::platform::ExchangedDelegation>(exchanged),
          "exchange delegation failed");
    const auto& value = std::get<kiarabot::core::platform::ExchangedDelegation>(exchanged);
    check(value.proof == "child-proof", "exchange proof mismatch");
    check(server.service.exchanges == 1 &&
              server.service.exchange_request.context().idempotency_key().size() >= 16 &&
              std::all_of(
                  server.service.exchange_request.context().idempotency_key().begin(),
                  server.service.exchange_request.context().idempotency_key().end(),
                  [](unsigned char character) { return character >= 33 && character < 127; }) &&
              canonical_uuid(server.service.exchange_request.context().request_id()) &&
              canonical_uuid(server.service.exchange_request.context().trace_id()) &&
              server.service.exchange_request.audience() == "scheduler" &&
              server.service.exchange_request.parent_proof() == "parent-proof" &&
              server.service.exchange_request.scope().resources_size() == 1 &&
              server.service.exchange_request.permissions_size() == 1 &&
              server.service.exchange_request.client_certificate_sha256().size() == 32,
          "exchange request wire contract incomplete");
    // The wire delegate lifetime must cover long business calls (Model stream deadline 30s) even
    // though the network call keeps its short budget; capping the child delegation at the network
    // budget made long streaming calls fail closed with a clipped execution deadline.
    const auto delegate_deadline =
        std::chrono::system_clock::time_point{std::chrono::seconds{
            server.service.exchange_request.context().deadline().seconds()}};
    check(delegate_deadline >= now() + std::chrono::seconds{25},
          "exchange delegation wire lifetime was clipped to the network budget");
  }
  {
    // ExchangeDelegation has its own budget: a 700ms Identity exchange exceeds the 500ms hot-path
    // check budget but must still succeed (production bug: reused 500ms budget timed out on the
    // second authorization pass and surfaced InvalidArgument/UNAUTHENTICATED).
    Server server(true, std::chrono::milliseconds{700});
    auto identity = client(server.port);
    const auto exchanged = identity->exchange_delegation(
        "parent-proof", t::Service::Model, {t::ResourceRef{t::ResourceKind::Model, {kResource}}},
        {t::Permission::ModelChat}, digest());
    check(std::holds_alternative<kiarabot::core::platform::ExchangedDelegation>(exchanged),
          "exchange delegation used hot-path budget and timed out");
    check(server.service.exchanges == 1, "slow exchange was not attempted");
  }
  {
    // Invalid delegation budgets are rejected at construction (never silently widened/unbounded).
    auto zero = kiarabot::core::platform::RuntimeIdentityClient::create(
        {"127.0.0.1:1", grpc::InsecureChannelCredentials(), std::chrono::milliseconds{500},
         std::chrono::milliseconds{1000}, std::chrono::milliseconds{0},
         std::chrono::milliseconds{5000}});
    check(std::holds_alternative<t::Failure>(zero), "zero delegation budget accepted");
    auto inverted = kiarabot::core::platform::RuntimeIdentityClient::create(
        {"127.0.0.1:1", grpc::InsecureChannelCredentials(), std::chrono::milliseconds{500},
         std::chrono::milliseconds{1000}, std::chrono::milliseconds{3000},
         std::chrono::milliseconds{2000}});
    check(std::holds_alternative<t::Failure>(inverted), "inverted delegation budget accepted");
    auto no_lifetime = kiarabot::core::platform::RuntimeIdentityClient::create(
        {"127.0.0.1:1", grpc::InsecureChannelCredentials(), std::chrono::milliseconds{500},
         std::chrono::milliseconds{1000}, std::chrono::milliseconds{2000},
         std::chrono::milliseconds{5000}, std::chrono::milliseconds{0}});
    check(std::holds_alternative<t::Failure>(no_lifetime), "zero delegation lifetime accepted");
    auto oversized_lifetime = kiarabot::core::platform::RuntimeIdentityClient::create(
        {"127.0.0.1:1", grpc::InsecureChannelCredentials(), std::chrono::milliseconds{500},
         std::chrono::milliseconds{1000}, std::chrono::milliseconds{2000},
         std::chrono::milliseconds{5000}, std::chrono::milliseconds{30001}});
    check(std::holds_alternative<t::Failure>(oversized_lifetime),
          "oversized delegation lifetime accepted");
    auto unbounded = kiarabot::core::platform::RuntimeIdentityClient::create(
        {"127.0.0.1:1", grpc::InsecureChannelCredentials(), std::chrono::milliseconds{500},
         std::chrono::milliseconds{1000}, std::chrono::milliseconds{2000},
         std::chrono::milliseconds{60000}});
    check(std::holds_alternative<t::Failure>(unbounded), "unbounded delegation budget accepted");
  }
  {
    // No listener: the owner client fails closed with an upstream failure, never an allow.
    auto identity = client(1);
    const auto result = identity->check(
        request(token(now(), "runtime", hex(digest())), now() + std::chrono::seconds{2}));
    check(std::holds_alternative<t::Failure>(result), "unreachable Identity produced an allow");
    const auto code = std::get<t::Failure>(result).code;
    check(code == t::ErrorCode::UpstreamUnavailable || code == t::ErrorCode::Internal,
          "unreachable Identity produced a non-dependency failure");
  }
}
} // namespace

int main() {
  decode_tests();
  check_tests();
  return 0;
}
