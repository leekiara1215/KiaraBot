#include "runtime_composition.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <openssl/rand.h>
#include <openssl/sha.h>

#include "kiarabot/core/application/target/runtime_application.hpp"
#include "kiarabot/core/application/target/runtime_bridge_application.hpp"
#include "kiarabot/core/application/target/runtime_readiness.hpp"
#include "kiarabot/core/application/target/runtime_resource_application.hpp"
#include "kiarabot/core/application/target/runtime_storage_application.hpp"
#include "kiarabot/core/runtime/platform/runtime_bridge.hpp"
#include "kiarabot/core/runtime/platform/runtime_identity_client.hpp"
#include "kiarabot/core/runtime/platform/runtime_rpc.hpp"
#include "kiarabot/core/runtime/platform/service_rpc.hpp"
#include "kiarabot/infrastructure/postgres/factories.hpp"
#include "postgres_service.hpp"

namespace kiarabot::core::composition {
namespace {
namespace target = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace postgres = kiarabot::infrastructure::postgres;

target::Failure invalid_composition() {
  return {target::ErrorCode::InvalidArgument,
          target::Acceptance::NotAccepted,
          target::Recovery::CorrectInput,
          {},
          {}};
}
target::Failure unavailable() {
  return {target::ErrorCode::UpstreamUnavailable,
          target::Acceptance::NotAccepted,
          target::Recovery::Backoff,
          {},
          {}};
}
// CSPRNG UUIDv4 ids for owner commands and opaque page tokens. No fallback id is generated on
// entropy failure; the caller receives UpstreamUnavailable instead.
class RandomIds final : public target::RuntimeIdPort {
public:
  target::Outcome<target::ResourceId> account_id() override {
    return typed<target::ResourceId>();
  }
  target::Outcome<target::InstallationId> installation_id() override {
    return typed<target::InstallationId>();
  }
  target::Outcome<target::InstanceId> instance_id() override {
    return typed<target::InstanceId>();
  }
  target::Outcome<target::InstanceId> adapter_instance_id() override {
    return typed<target::InstanceId>();
  }
  target::Outcome<target::ConnectionId> connection_id() override {
    return typed<target::ConnectionId>();
  }
  target::Outcome<target::OperationId> operation_id() override {
    return typed<target::OperationId>();
  }
  target::Outcome<target::RuntimeEpoch> runtime_epoch() override {
    return typed<target::RuntimeEpoch>();
  }
  target::Outcome<std::string> snapshot_id() override {
    auto value = uuid();
    if (const auto* failure = std::get_if<target::Failure>(&value)) {
      return *failure;
    }
    return std::get<std::string>(value);
  }

private:
  template <typename Identifier> target::Outcome<Identifier> typed() {
    auto value = uuid();
    if (const auto* failure = std::get_if<target::Failure>(&value)) {
      return *failure;
    }
    return Identifier{std::get<std::string>(value)};
  }
  target::Outcome<std::string> uuid() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
      return unavailable();
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      if (index == 4 || index == 6 || index == 8 || index == 10) {
        result += '-';
      }
      result += kDigits[bytes[index] >> 4U];
      result += kDigits[bytes[index] & 0x0fU];
    }
    return result;
  }
};
// Composition-owned CSPRNG bootstrap credential and one-way digest. Plaintext is returned once and
// never persisted; only the SHA-256 digest reaches the Runtime database.
class RandomSecrets final : public target::RuntimeSecretPort {
public:
  target::Outcome<std::string> bootstrap_credential() override {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
      return unavailable();
    }
    constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : bytes) {
      result += kDigits[byte >> 4U];
      result += kDigits[byte & 0x0fU];
    }
    return result;
  }
  target::Outcome<std::array<std::byte, 32>> opaque_secret() override {
    std::array<std::byte, 32> bytes{};
    if (RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()),
                   static_cast<int>(bytes.size())) != 1) {
      return unavailable();
    }
    return bytes;
  }
  target::Outcome<target::Sha256Digest> sha256(std::string_view value) override {
    target::Sha256Digest digest;
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(),
           reinterpret_cast<unsigned char*>(digest.bytes.data()));
    return digest;
  }
};
// Fail-closed until the real Identity current-authorization/delegation client is wired. It never
// allows a request, and its availability observation is false so readiness stays false.
class FailClosedAuthorization final : public target::RuntimeAuthorizationPort {
public:
  bool available() const noexcept override {
    return false;
  }
  target::Outcome<target::RuntimeAuthorizationDecision>
  check(const target::RuntimeAuthorizationRequest&) override {
    return target::Failure{target::ErrorCode::UpstreamUnavailable,
                           target::Acceptance::NotAccepted,
                           target::Recovery::Backoff,
                           {},
                           {}};
  }
};
// Fail-closed peer authorizer until the real mTLS peer + Identity delegation verification lands.
platform::RuntimePeerAuthorizer fail_closed_authorizer() {
  return [](grpc::ServerContext&,
            const v1::RequestContext&) -> target::Outcome<target::AuthenticationContext> {
    return target::Failure{target::ErrorCode::Unauthenticated,
                           target::Acceptance::NotAccepted,
                           target::Recovery::Authenticate,
                           {},
                           {}};
  };
}
struct State {
  std::unique_ptr<target::RuntimeIdPort> ids;
  std::unique_ptr<target::RuntimeSecretPort> secrets;
  std::unique_ptr<platform::RuntimeIdentityClient> identity_client;
  std::unique_ptr<target::RuntimeAuthorizationPort> fail_closed_authorization;
  target::RuntimeAuthorizationPort* authorization = nullptr;
  target::RuntimeResourceApplication* application = nullptr;
  std::unique_ptr<target::RuntimeStorageApplication> storage;
  std::unique_ptr<target::RuntimeBridgeApplication> bridge;
};
// Readiness merges the composition-observed database state with the authorization-chain observation
// and the assembled business implementation. Unobserved inputs remain Unverified and never ready.
class RuntimeReadiness final : public target::ServiceReadinessPort {
public:
  RuntimeReadiness(std::unique_ptr<target::ServiceReadinessPort> database_observed,
                   std::shared_ptr<State> state)
      : database_observed_(std::move(database_observed)), state_(std::move(state)) {}
  target::Service service() const noexcept override {
    return target::Service::Runtime;
  }
  target::ServiceReadiness readiness() const override {
    target::RuntimeReadinessObservation observation;
    if (database_observed_) {
      const auto base = database_observed_->readiness();
      for (const auto& dependency : base.dependencies) {
        if (dependency.dependency == target::Dependency::BusinessImplementation) {
          observation.business_assembled = dependency.state;
        }
        if (dependency.dependency == target::Dependency::DatabaseSchema) {
          observation.database_schema = dependency.state;
        }
      }
    }
    if (state_ && state_->authorization) {
      observation.identity_authorization = state_->authorization->available()
                                               ? target::DependencyState::Available
                                               : target::DependencyState::Unavailable;
    }
    return target::evaluate_runtime_readiness(observation);
  }

private:
  std::unique_ptr<target::ServiceReadinessPort> database_observed_;
  std::shared_ptr<State> state_;
};
} // namespace

target::Outcome<platform::ServiceComposition>
compose_runtime_service(const platform::ProcessConfig& config) {
  try {
    if (config.service != target::Service::Runtime) {
      return invalid_composition();
    }
    if (!config.database) {
      // P02 lifecycle/smoke fixtures start every process without a database. Keep the independent
      // Runtime process startable with an explicit non-ready application and no business RPC, as
      // before P05; the typed PG composition is selected only when a database is configured.
      return compose_database_service<target::RuntimeUnitOfWork>(
          config, postgres::open_runtime, [](auto&) { return target::make_runtime_application(); });
    }
    auto state = std::make_shared<State>();
    auto composition = compose_database_service<target::RuntimeUnitOfWork>(
        config, postgres::open_runtime,
        [state, &config](auto& factory) -> std::unique_ptr<target::ServiceReadinessPort> {
          state->ids = std::make_unique<RandomIds>();
          state->secrets = std::make_unique<RandomSecrets>();
          if (config.identity_endpoint) {
            grpc::SslCredentialsOptions ssl;
            ssl.pem_root_certs = config.tls.ca;
            ssl.pem_cert_chain = config.tls.certificate;
            ssl.pem_private_key = config.tls.private_key;
            auto client = platform::RuntimeIdentityClient::create(
                {*config.identity_endpoint, grpc::SslCredentials(ssl),
                 std::chrono::milliseconds{500}, std::chrono::milliseconds{1000}});
            if (const auto* failure = std::get_if<target::Failure>(&client)) {
              return {};
            }
            state->identity_client =
                std::move(std::get<std::unique_ptr<platform::RuntimeIdentityClient>>(client));
          } else {
            state->fail_closed_authorization = std::make_unique<FailClosedAuthorization>();
          }
          state->authorization =
              state->identity_client
                  ? static_cast<target::RuntimeAuthorizationPort*>(state->identity_client.get())
                  : state->fail_closed_authorization.get();
          auto built = target::make_runtime_resource_application(
              {factory,
               *state->ids,
               *state->secrets,
               *state->authorization,
               {"action.send_message.text.v1", "action.react_message.v1",
                "action.delete_message.v1", "action.mute_member.v1", "action.kick_member.v1",
                "action.approve_request.v1"}});
          if (const auto* failure = std::get_if<target::Failure>(&built)) {
            return {};
          }
          auto application =
              std::move(std::get<std::unique_ptr<target::RuntimeResourceApplication>>(built));
          state->application = application.get();
          if (config.bridge_listen) {
            auto bridge = target::make_runtime_bridge_application(
                {factory, *state->ids, *state->secrets, *state->application, {}});
            if (const auto* failure = std::get_if<target::Failure>(&bridge)) {
              return {};
            }
            state->bridge =
                std::move(std::get<std::unique_ptr<target::RuntimeBridgeApplication>>(bridge));
          }
          return application;
        });
    if (const auto* failure = std::get_if<target::Failure>(&composition)) {
      return *failure;
    }
    auto& result = std::get<platform::ServiceComposition>(composition);
    if (!state->application) {
      return unavailable();
    }
    // Bridge maintenance runs on the bounded once-per-second dependency hook: expire dispatcher
    // leases, advance retries/dead letters and claim/push due deliveries for connected plugins.
    {
      auto base = std::move(result.check_dependencies);
      result.check_dependencies = [base = std::move(base), state]() mutable -> target::Status {
        if (base) {
          auto checked = base();
          if (std::holds_alternative<target::Failure>(checked)) {
            return checked;
          }
        }
        if (!state->bridge) {
          return std::monostate{};
        }
        return state->bridge->maintenance();
      };
    }
    auto* application = state->application;
    auto configure = std::move(result.configure_rpc);
    result.configure_rpc = [configure = std::move(configure), state,
                            application](platform::ServiceRpc& rpc) mutable -> target::Status {
      if (configure) {
        auto configured = configure(rpc);
        if (const auto* failure = std::get_if<target::Failure>(&configured)) {
          return *failure;
        }
      }
      platform::RuntimeRpcOptions options;
      options.storage = state->storage.get();
      if (state->identity_client) {
        auto* client = state->identity_client.get();
        options.authorize = [client](grpc::ServerContext& transport,
                                     const v1::RequestContext& request) {
          return client->authenticate(transport, request);
        };
      } else {
        options.authorize = fail_closed_authorizer();
      }
      auto adapter = platform::make_runtime_rpc(*application, std::move(options));
      if (const auto* failure = std::get_if<target::Failure>(&adapter)) {
        return *failure;
      }
      auto registered = rpc.add_service(
          target::Service::Runtime,
          std::move(std::get<std::unique_ptr<v1::RuntimeService::Service>>(adapter)));
      if (const auto* failure = std::get_if<target::Failure>(&registered)) {
        return *failure;
      }
      if (!state->bridge) {
        return std::monostate{};
      }
      auto bridge_application = platform::make_adapter_bridge(*state->bridge);
      if (const auto* failure = std::get_if<target::Failure>(&bridge_application)) {
        return *failure;
      }
      registered = rpc.add_bridge_service(
          target::Service::Runtime,
          std::move(std::get<std::unique_ptr<v1::AdapterBridge::Service>>(bridge_application)));
      if (const auto* failure = std::get_if<target::Failure>(&registered)) {
        return *failure;
      }
      auto plugin_bridge = platform::make_plugin_bridge(*state->bridge);
      if (const auto* failure = std::get_if<target::Failure>(&plugin_bridge)) {
        return *failure;
      }
      return rpc.add_bridge_service(
          target::Service::Runtime,
          std::move(std::get<std::unique_ptr<v1::PluginBridge::Service>>(plugin_bridge)));
    };
    result.before_stop = [state]() -> target::Status {
      if (!state->bridge) {
        return std::monostate{};
      }
      auto sent = state->bridge->shutdown_notice();
      // Bounded drain: the writer threads flush the notice frames while the process releases
      // listeners; a slow peer can never extend shutdown beyond this fixed budget.
      std::this_thread::sleep_for(std::chrono::milliseconds{500});
      return sent;
    };
    result.application = std::make_unique<RuntimeReadiness>(std::move(result.application), state);
    return std::move(result);
  } catch (...) {
    return unavailable();
  }
}

} // namespace kiarabot::core::composition
