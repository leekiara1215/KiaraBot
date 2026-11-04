#include "model_application_test_support.hpp"

namespace model_application_test {

void management_and_chat() {
  Fixture fixture;
  fixture.create_model();
  require(fixture.store.configs.at(kModel).state == t::LifecycleState::Active);
  require(fixture.store.secrets.at(kModel).size() == 1);
  require(fixture.store.secrets.at(kModel).front().root_key_version == 7);
  require(fixture.crypto.seals == 1);

  auto reply = fixture.take(fixture.chat(context("chat-1")));
  require(reply.disposition == t::ModelChatDisposition::FreshResult && reply.result &&
          *reply.result == "answer");
  require(fixture.provider.calls == 1);
  require(reply.call.state == t::ModelCallState::Succeeded && reply.call.usage_known &&
          reply.call.input_tokens == 10 && reply.call.may_have_billed &&
          reply.call.secret_version.value == 1);
  require(fixture.crypto.opens == 1);
  require(fixture.provider.last.timeout_ms > 0 && fixture.provider.last.timeout_ms <= 5000);

  // Same logical call id and same content never invokes the provider twice.
  auto replay = fixture.take(fixture.chat(context("chat-2")));
  require(replay.disposition == t::ModelChatDisposition::ReplayTerminal);
  require(fixture.provider.calls == 1);
  require(replay.call.state == t::ModelCallState::Succeeded);

  // Same call id with different content is a stable conflict.
  auto conflict = fixture.chat(context("chat-3"), kCall, "different");
  require(std::holds_alternative<t::Failure>(conflict) &&
          std::get<t::Failure>(conflict).code == t::ErrorCode::Conflict);

  // Rotation pins a new secret version only for later calls and leaves configuration version.
  auto rotated = fixture.take(application_rotate(fixture));
  require(rotated.version && rotated.version->value == 2);
  require(fixture.store.configs.at(kModel).version.value == 2);
  fixture.take(fixture.chat(context("chat-4"), kCall2, "second"));
  require(fixture.store.calls.at({kOwner, kCall2}).secret_version.value == 2);
  require(fixture.store.calls.at({kOwner, kCall}).secret_version.value == 1);

  // Update replay with the same idempotency key reuses the receipt and creates no new operation.
  t::ModelConfiguration update = fixture.store.configs.at(kModel);
  update.timeout_ms = 6000;
  const auto current_version = update.version;
  const auto operations_before = fixture.store.operations.size();
  update.version = {current_version.value + 1};
  auto updated = fixture.take(
      fixture.application->update_model({control(), context("update-2"), update, current_version}));
  require(updated.version && updated.version->value == current_version.value + 1);
  auto replay_update = fixture.take(
      fixture.application->update_model({control(), context("update-2"), update, current_version}));
  require(replay_update.version && replay_update.version->value == current_version.value + 1);
  require(fixture.store.operations.size() == operations_before + 1);

  // Different content under the same idempotency key is a conflict.
  auto conflicting_update = fixture.application->update_model(
      {control(), context("update-2"), update, t::ResourceVersion{current_version.value + 2}});
  require(std::holds_alternative<t::Failure>(conflicting_update) &&
          std::get<t::Failure>(conflicting_update).code == t::ErrorCode::Conflict);
}

t::Outcome<t::ModelMutationReply> application_rotate(Fixture& fixture) {
  return fixture.application->rotate_provider_secret(
      {control(),
       context("rotate-2"),
       {kModel},
       std::vector<std::byte>{std::byte{'d'}, std::byte{'e'}, std::byte{'f'}},
       t::ResourceVersion{1}});
}

void failures_keep_unknown() {
  Fixture fixture;
  fixture.create_model();
  // A disabled model never reaches the provider.
  fixture.take(fixture.application->set_model_lifecycle(
      {control(), context("disable-1"), {kModel}, t::ResourceVersion{2}, false}));
  auto disabled = fixture.chat(context("chat-disabled"));
  require(std::holds_alternative<t::Failure>(disabled) &&
          std::get<t::Failure>(disabled).code == t::ErrorCode::CapabilityNotSupported &&
          fixture.provider.calls == 0);
  fixture.take(fixture.application->set_model_lifecycle(
      {control(), context("enable-2"), {kModel}, t::ResourceVersion{3}, true}));

  // Provider unknown result is persisted as UNKNOWN and never converted to a known failure.
  t::ModelProviderResult unknown;
  unknown.outcome = t::ModelProviderOutcome::Unknown;
  unknown.failure = {
      t::ErrorCode::Timeout, t::Acceptance::NotAccepted, t::Recovery::Backoff, {}, {}};
  fixture.provider.results.push_back(unknown);
  auto reply = fixture.chat(context("chat-unknown"));
  require(std::holds_alternative<t::Failure>(reply) &&
          std::get<t::Failure>(reply).code == t::ErrorCode::Timeout &&
          std::get<t::Failure>(reply).acceptance == t::Acceptance::Unknown);
  const auto stored = fixture.store.calls.at({kOwner, kCall});
  require(stored.state == t::ModelCallState::Unknown && stored.may_have_billed);

  auto status = fixture.take(
      fixture.application->get_call_status({plugin(), context("status-1"), kCall, std::nullopt}));
  require(status.state == t::ModelCallState::Unknown);
  auto resolved = fixture.take(fixture.application->resolve_call(
      {control(), context("resolve-1"), kCall, t::ResourceVersion{2},
       t::ModelCallDisposition::ExecutedBilled}));
  require(resolved.state == t::ModelCallState::Failed &&
          resolved.disposition == t::ModelCallDisposition::ExecutedBilled);
  auto resolved_replay = fixture.take(fixture.application->resolve_call(
      {control(), context("resolve-1"), kCall, t::ResourceVersion{2},
       t::ModelCallDisposition::ExecutedBilled}));
  require(resolved_replay.disposition == t::ModelCallDisposition::ExecutedBilled);

  // A second transaction failure after the provider call returns an unknown acceptance and leaves
  // the durable row in progress for status queries; the provider was invoked exactly once.
  Fixture second;
  second.create_model();
  second.store.fail_commits = 1; // committed intents still survive; the next commit is the txn2
  const auto calls_before = second.provider.calls;
  // fail_commits applies from the start: the first commit (txn1) fails and the provider is NOT
  // called, which is the other frozen invariant.
  auto txn1_failure = second.chat(context("chat-txn1"));
  require(std::holds_alternative<t::Failure>(txn1_failure) &&
          std::get<t::Failure>(txn1_failure).code == t::ErrorCode::UpstreamUnavailable);
  require(second.provider.calls == calls_before);
  require(second.store.calls.empty());
}

void maintenance_and_authorization() {
  Fixture fixture;
  fixture.create_model();
  // Seed an old unresolved UNKNOWN row directly (simulating a crash after provider invocation).
  t::ModelIntent stale;
  stale.owner = {kOwner};
  stale.logical_call_id = kCall2;
  stale.model = {kModel};
  stale.config_version = {1};
  stale.request_hash.bytes[0] = std::byte{0x11};
  stale.state = t::ModelCallState::Unknown;
  stale.version = {1};
  stale.created_at = now() - 31 * 24h;
  stale.observed_at = stale.created_at;
  stale.secret_version = {1};
  stale.may_have_billed = true;
  fixture.store.calls[{kOwner, kCall2}] = stale;
  require(std::holds_alternative<std::monostate>(fixture.application->maintenance()));
  const auto abandoned = fixture.store.calls.at({kOwner, kCall2});
  require(abandoned.state == t::ModelCallState::Failed &&
          abandoned.disposition == t::ModelCallDisposition::Abandoned && abandoned.may_have_billed);
  require(!fixture.store.audits.empty());

  fixture.authorization.deny = true;
  auto denied = fixture.chat(context("chat-denied"), kCall, "denied");
  require(std::holds_alternative<t::Failure>(denied) &&
          std::get<t::Failure>(denied).code == t::ErrorCode::PermissionDenied);
  auto metadata =
      fixture.application->get_model_metadata({control(), context("metadata-denied"), {kModel}});
  require(std::holds_alternative<t::Failure>(metadata));

  fixture.authorization.deny = false;
  auto status = fixture.application->get_call_status(
      {control(),
       context("status-denied"),
       kCall2,
       {t::ResourceId{"99999999-9999-4999-8999-999999999999"}}});
  require(std::holds_alternative<t::Failure>(status) &&
          std::get<t::Failure>(status).code == t::ErrorCode::NotFound);
}

#ifdef KIARABOT_TEST_MODEL_INFRASTRUCTURE
// ---------------------------------------------------------------------------------------------
// P11b integration: real libcurl provider and real root-key AEAD against a loopback fake upstream,
// with the P11a in-memory typed store to exercise the two transactions and failure persistence.
// ---------------------------------------------------------------------------------------------
std::string http_response(int status, std::string_view reason, const std::string& body,
                          std::string_view extra_headers = {}) {
  return "HTTP/1.1 " + std::to_string(status) + " " + std::string{reason} +
         "\r\nContent-Type: application/json\r\n" + std::string{extra_headers} +
         "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}
std::string upstream_success(std::string_view content = "answer") {
  return std::string{"{\"id\":\"chatcmpl-p11b\",\"choices\":[{\"message\":{\"role\":"
                     "\"assistant\",\"content\":\""} +
         std::string{content} + "\"}}],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":6}}";
}
class HttpLoopbackServer {
public:
  using Handler = std::function<std::string(const std::string& request)>;
  explicit HttpLoopbackServer(Handler handler) : handler_(std::move(handler)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    require(fd_ >= 0, "loopback socket");
    int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "loopback bind");
    require(::listen(fd_, 8) == 0, "loopback listen");
    socklen_t length = sizeof(address);
    require(::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0,
            "loopback name");
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] { serve(); });
  }
  ~HttpLoopbackServer() {
    stop_ = true;
    if (fd_ >= 0) {
      ::shutdown(fd_, SHUT_RDWR);
      ::close(fd_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  HttpLoopbackServer(const HttpLoopbackServer&) = delete;
  HttpLoopbackServer& operator=(const HttpLoopbackServer&) = delete;
  std::string endpoint() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/v1/chat/completions";
  }
  int requests() const {
    return requests_.load();
  }
  std::string last_request() const {
    std::lock_guard lock(mutex_);
    return request_;
  }

private:
  void serve() {
    while (!stop_) {
      const int client = ::accept(fd_, nullptr, nullptr);
      if (client < 0) {
        return;
      }
      std::string received;
      char buffer[4096];
      while (received.find("\r\n\r\n") == std::string::npos) {
        const auto read = ::recv(client, buffer, sizeof(buffer), 0);
        if (read <= 0) {
          break;
        }
        received.append(buffer, static_cast<std::size_t>(read));
      }
      const auto separator = received.find("\r\n\r\n");
      if (separator != std::string::npos) {
        std::size_t length = 0;
        const auto content_length = received.find("Content-Length:");
        if (content_length != std::string::npos) {
          length = static_cast<std::size_t>(
              std::strtoul(received.c_str() + content_length + 15, nullptr, 10));
        }
        while (received.size() < separator + 4 + length) {
          const auto read = ::recv(client, buffer, sizeof(buffer), 0);
          if (read <= 0) {
            break;
          }
          received.append(buffer, static_cast<std::size_t>(read));
        }
      }
      requests_.fetch_add(1);
      {
        std::lock_guard lock(mutex_);
        request_ = received;
      }
      const auto response = stop_ ? std::string{} : handler_(received);
      if (!response.empty()) {
        ::send(client, response.data(), response.size(), 0);
      }
      ::shutdown(client, SHUT_RDWR);
      ::close(client);
    }
  }
  int fd_{-1};
  unsigned short port_{0};
  Handler handler_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<int> requests_{0};
  mutable std::mutex mutex_;
  std::string request_;
};
kiarabot::infrastructure::identity_crypto::SymmetricKey root_key(std::byte seed) {
  std::array<std::byte, 32> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>(
        (static_cast<unsigned char>(seed) + static_cast<unsigned char>(index)) & 0xffU);
  }
  auto copied =
      kiarabot::infrastructure::identity_crypto::copy_secret(std::as_bytes(std::span{bytes}));
  require(std::holds_alternative<t::OwnedSecret>(copied), "root key copy");
  return kiarabot::infrastructure::identity_crypto::SymmetricKey{
      {"model-root-v1"}, std::move(std::get<t::OwnedSecret>(copied))};
}
std::vector<std::byte> provider_secret(std::string_view value) {
  std::vector<std::byte> result(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    result[index] = static_cast<std::byte>(static_cast<unsigned char>(value[index]));
  }
  return result;
}
struct HttpFixture {
  Store store;
  FakeDigest digest;
  FakeIds ids;
  FakeAuthorization authorization;
  FakeFactory factory{store};
  std::unique_ptr<t::ModelProviderPort> provider;
  std::unique_ptr<t::ModelSecretCryptoPort> crypto;
  std::unique_ptr<t::ModelGatewayApplication> application;
  void configure(std::byte root_seed) {
    auto opened =
        kiarabot::infrastructure::identity_crypto::make_model_secret_crypto(root_key(root_seed));
    require(std::holds_alternative<std::unique_ptr<t::ModelSecretCryptoPort>>(opened),
            "crypto open");
    crypto = std::move(std::get<std::unique_ptr<t::ModelSecretCryptoPort>>(opened));
    auto opened_provider = kiarabot::infrastructure::make_model_http_provider();
    require(std::holds_alternative<std::unique_ptr<t::ModelProviderPort>>(opened_provider),
            "provider open");
    provider = std::move(std::get<std::unique_ptr<t::ModelProviderPort>>(opened_provider));
    rebuild();
  }
  void rebuild() {
    application.reset();
    application = t::make_model_gateway_application(
        {factory, *provider, *crypto, digest, ids, authorization, 7});
  }
  void replace_crypto(std::byte root_seed) {
    application.reset();
    auto opened =
        kiarabot::infrastructure::identity_crypto::make_model_secret_crypto(root_key(root_seed));
    require(std::holds_alternative<std::unique_ptr<t::ModelSecretCryptoPort>>(opened),
            "crypto replacement");
    crypto = std::move(std::get<std::unique_ptr<t::ModelSecretCryptoPort>>(opened));
    rebuild();
  }
  template <typename Value> Value take(t::Outcome<Value> result) {
    if (const auto* failure = std::get_if<t::Failure>(&result)) {
      std::cerr << "P11b integration failure code " << static_cast<int>(failure->code) << '\n';
      throw std::runtime_error("P11b integration operation failed");
    }
    return std::move(std::get<Value>(result));
  }
  void create_model(const std::string& endpoint, std::string_view secret) {
    t::ModelConfiguration config;
    config.model_id = {kModel};
    config.alias = "default";
    config.endpoint = endpoint;
    config.provider_model = "gpt-test";
    config.timeout_ms = 5000;
    config.max_concurrency = 2;
    take(application->update_model({control(), context("http-update-1"), config, {}}));
    take(application->rotate_provider_secret(
        {control(), context("http-rotate-1"), {kModel}, provider_secret(secret), {}}));
    take(application->set_model_lifecycle(
        {control(), context("http-enable-1"), {kModel}, t::ResourceVersion{1}, true}));
  }
  t::Outcome<t::ModelChatReply> chat(t::ModelCallContext call_context, std::string call_id,
                                     std::string text = "hello") {
    return application->chat({plugin(),
                              std::move(call_context),
                              std::move(call_id),
                              "default",
                              {message(std::move(text))}});
  }
};

void http_success_and_pinning() {
  HttpLoopbackServer server(
      [](const std::string&) { return http_response(200, "OK", upstream_success()); });
  HttpFixture fixture;
  fixture.configure(std::byte{1});
  fixture.create_model(server.endpoint(), "secret-one");
  auto reply = fixture.take(fixture.chat(context("http-chat-1"), kCall));
  require(reply.disposition == t::ModelChatDisposition::FreshResult && reply.result &&
              *reply.result == "answer",
          "http success");
  require(server.requests() == 1, "one provider request");
  require(server.last_request().find("Authorization: Bearer secret-one") != std::string::npos,
          "real provider receives unsealed secret");
  require(reply.call.usage_known && reply.call.input_tokens == 5 && reply.call.output_tokens == 6 &&
              reply.call.secret_version.value == 1,
          "usage call facts");
  auto replay = fixture.take(fixture.chat(context("http-chat-2"), kCall));
  require(replay.disposition == t::ModelChatDisposition::ReplayTerminal && server.requests() == 1,
          "replay never re-invokes provider");
  fixture.take(fixture.application->rotate_provider_secret({control(),
                                                            context("http-rotate-2"),
                                                            {kModel},
                                                            provider_secret("secret-two"),
                                                            t::ResourceVersion{1}}));
  auto second = fixture.take(fixture.chat(context("http-chat-3"), kCall2, "second"));
  require(second.call.secret_version.value == 2, "second call pinned secret v2");
  require(server.requests() == 2 &&
              server.last_request().find("Authorization: Bearer secret-two") != std::string::npos,
          "rotated secret used by later call");
  require(fixture.store.calls.at({kOwner, kCall}).secret_version.value == 1,
          "in-flight/first call kept secret v1");
}

void http_mapping_integration() {
  {
    HttpLoopbackServer limited([](const std::string&) {
      return http_response(429, "Too Many", "{}", "Retry-After: 3\r\n");
    });
    HttpFixture fixture;
    fixture.configure(std::byte{2});
    fixture.create_model(limited.endpoint(), "secret");
    auto reply = fixture.chat(context("http-429"), kCall);
    require(std::holds_alternative<t::Failure>(reply), "429 failure");
    const auto& failure = std::get<t::Failure>(reply);
    require(failure.code == t::ErrorCode::RateLimited &&
                failure.acceptance == t::Acceptance::Accepted && failure.retry_after &&
                failure.retry_after->count() == 3000,
            "429 stable accepted failure with retry-after");
    const auto call = fixture.store.calls.at({kOwner, kCall});
    require(call.state == t::ModelCallState::Failed && !call.may_have_billed &&
                limited.requests() == 1,
            "429 known failure is not billed and not retried");
  }
  {
    HttpLoopbackServer broken(
        [](const std::string&) { return http_response(500, "Server", "{}"); });
    HttpFixture fixture;
    fixture.configure(std::byte{3});
    fixture.create_model(broken.endpoint(), "secret");
    auto reply = fixture.chat(context("http-500"), kCall);
    require(std::holds_alternative<t::Failure>(reply), "500 failure");
    const auto& failure = std::get<t::Failure>(reply);
    require(failure.code == t::ErrorCode::UpstreamUnavailable &&
                failure.acceptance == t::Acceptance::Unknown,
            "500 is unknown");
    const auto call = fixture.store.calls.at({kOwner, kCall});
    require(call.state == t::ModelCallState::Unknown && call.may_have_billed,
            "500 persists conservative unknown");
  }
}

void http_txn2_failure_keeps_unknown() {
  HttpLoopbackServer server(
      [](const std::string&) { return http_response(200, "OK", upstream_success()); });
  HttpFixture fixture;
  fixture.configure(std::byte{4});
  fixture.create_model(server.endpoint(), "secret");
  // Let the txn1 intent commit, then fail the txn2 result commit after the provider responded.
  fixture.store.skip_commits = 1;
  fixture.store.fail_commits = 1;
  auto reply = fixture.chat(context("http-txn2"), kCall);
  require(server.requests() == 1, "provider invoked before txn2 failure");
  require(std::holds_alternative<t::Failure>(reply), "txn2 failure");
  require(std::get<t::Failure>(reply).acceptance == t::Acceptance::Unknown,
          "txn2 failure keeps unknown acceptance");
  const auto call = fixture.store.calls.at({kOwner, kCall});
  require(call.state == t::ModelCallState::Started,
          "txn2 failure leaves durable intent (not a known result)");
}

void http_unseal_failure_is_closed() {
  HttpLoopbackServer server(
      [](const std::string&) { return http_response(200, "OK", upstream_success()); });
  HttpFixture fixture;
  fixture.configure(std::byte{5});
  fixture.create_model(server.endpoint(), "secret");
  // Deploy a different root key after the secret was sealed: the provider must never be called.
  fixture.replace_crypto(std::byte{6});
  auto reply = fixture.chat(context("http-unseal"), kCall);
  require(std::holds_alternative<t::Failure>(reply), "unseal failure");
  const auto& failure = std::get<t::Failure>(reply);
  require(failure.code == t::ErrorCode::UpstreamUnavailable &&
              failure.acceptance == t::Acceptance::Accepted,
          "unseal fault is a known not-billed failure");
  require(server.requests() == 0, "provider not invoked without a readable secret");
  const auto call = fixture.store.calls.at({kOwner, kCall});
  require(call.state == t::ModelCallState::Failed && !call.may_have_billed,
          "unseal fault does not mark billed");
}

#endif // KIARABOT_TEST_MODEL_INFRASTRUCTURE

} // namespace model_application_test

int main() {
  try {
    model_application_test::management_and_chat();
    model_application_test::failures_keep_unknown();
    model_application_test::maintenance_and_authorization();
#ifdef KIARABOT_TEST_MODEL_INFRASTRUCTURE
    model_application_test::http_success_and_pinning();
    model_application_test::http_mapping_integration();
    model_application_test::http_txn2_failure_keeps_unknown();
    model_application_test::http_unseal_failure_is_closed();
#endif
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
