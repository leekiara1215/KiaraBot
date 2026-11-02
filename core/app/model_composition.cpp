#include "model_composition.hpp"

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <openssl/rand.h>

#include "kiarabot/core/application/target/model_application.hpp"
#include "kiarabot/core/application/target/model_gateway_application.hpp"
#include "kiarabot/core/runtime/platform/model_rpc.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"
#include "kiarabot/core/runtime/platform/service_rpc.hpp"
#include "kiarabot/infrastructure/identity_crypto/key_files.hpp"
#include "kiarabot/infrastructure/identity_crypto/model_secret_crypto.hpp"
#include "postgres_service.hpp"

namespace kiarabot::core::composition {
namespace {
namespace target = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace crypto = kiarabot::infrastructure::identity_crypto;

// Development-only provider until P11b installs the reviewed HTTP adapter. It never touches the
// network, echoes bounded text and reports no authoritative usage; production ProcessConfig is
// rejected unless the root key file is present, and readiness stays false until P11b/P11c.
class DevelopmentModelProvider final : public target::ModelProviderPort {
public:
  target::ModelProviderResult chat(const target::ModelProviderRequest& request) override {
    target::ModelProviderResult result;
    try {
      result.outcome = target::ModelProviderOutcome::Succeeded;
      std::string text = "model-dev:";
      for (const auto& message : request.messages) {
        if (text.size() >= target::kModelMaximumResultBytes) {
          break;
        }
        text += message.text.substr(0, 256);
      }
      result.reply.text = text.substr(0, target::kModelMaximumResultBytes);
      result.reply.usage = {false, 0, 0};
    } catch (...) {
      result.outcome = target::ModelProviderOutcome::Unknown;
      result.failure = {target::ErrorCode::Internal,
                        target::Acceptance::NotAccepted,
                        target::Recovery::Diagnose,
                        {},
                        {}};
    }
    return result;
  }
};

class FailClosedModelAuthorization final : public target::ModelAuthorizationPort {
public:
  target::Outcome<target::ModelAuthorizationDecision>
  authorize(const target::ModelCaller&, target::Permission, const target::ResourceRef&,
            std::chrono::milliseconds) override {
    return target::Failure{target::ErrorCode::PermissionDenied,
                           target::Acceptance::NotAccepted,
                           target::Recovery::Authenticate,
                           {},
                           {}};
  }
};

class RandomModelIds final : public target::ModelIdPort {
public:
  target::Outcome<std::string> uuid() override {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
      return target::Failure{target::ErrorCode::UpstreamUnavailable,
                             target::Acceptance::NotAccepted,
                             target::Recovery::Backoff,
                             {},
                             {}};
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    constexpr char kDigits[] = "0123456789abcdef";
    std::string value;
    value.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      if (index == 4 || index == 6 || index == 8 || index == 10) {
        value += '-';
      }
      value += kDigits[bytes[index] >> 4U];
      value += kDigits[bytes[index] & 0x0fU];
    }
    return value;
  }
};

struct State {
  std::unique_ptr<DevelopmentModelProvider> provider;
  std::unique_ptr<target::ModelSecretCryptoPort> crypto;
  std::unique_ptr<target::ModelDigestPort> digest;
  std::unique_ptr<RandomModelIds> ids;
  std::unique_ptr<FailClosedModelAuthorization> authorization;
  std::unique_ptr<target::ModelGatewayApplication> application;
};

class Applications final : public target::ServiceReadinessPort {
public:
  explicit Applications(std::shared_ptr<State> state) : state_(std::move(state)) {}
  target::Service service() const noexcept override {
    return target::Service::Model;
  }
  target::ServiceReadiness readiness() const override {
    // P11a is not production-ready: caller verification and the HTTP provider are P11c/P11b.
    return {false,
            {{target::Dependency::BusinessImplementation, target::DependencyState::Available},
             {target::Dependency::DatabaseSchema, target::DependencyState::Unverified},
             {target::Dependency::Identity, target::DependencyState::Unverified},
             {target::Dependency::Credentials, target::DependencyState::Available}}};
  }

private:
  std::shared_ptr<State> state_;
};

target::Outcome<std::unique_ptr<target::ServiceReadinessPort>>
build_application(const platform::ProcessConfig& config, target::ModelUnitOfWorkFactory& factory,
                  std::shared_ptr<State>& state) {
  if (!config.model_security) {
    return target::Failure{target::ErrorCode::InvalidArgument,
                           target::Acceptance::NotAccepted,
                           target::Recovery::CorrectInput,
                           {},
                           {}};
  }
  const auto& material = *config.model_security;
  auto root_key = crypto::load_symmetric_key_file({material.root_key.id, material.root_key.file});
  if (const auto* failure = std::get_if<target::Failure>(&root_key)) {
    return *failure;
  }
  state = std::make_shared<State>();
  auto encryption =
      crypto::make_model_secret_crypto(std::move(std::get<crypto::SymmetricKey>(root_key)));
  if (const auto* failure = std::get_if<target::Failure>(&encryption)) {
    return *failure;
  }
  state->crypto = std::move(std::get<std::unique_ptr<target::ModelSecretCryptoPort>>(encryption));
  auto digest = crypto::make_model_digest();
  if (const auto* failure = std::get_if<target::Failure>(&digest)) {
    return *failure;
  }
  state->digest = std::move(std::get<std::unique_ptr<target::ModelDigestPort>>(digest));
  state->provider = std::make_unique<DevelopmentModelProvider>();
  state->ids = std::make_unique<RandomModelIds>();
  state->authorization = std::make_unique<FailClosedModelAuthorization>();
  state->application = target::make_model_gateway_application(
      {factory, *state->provider, *state->crypto, *state->digest, *state->ids,
       *state->authorization, material.key_version});
  return std::unique_ptr<target::ServiceReadinessPort>(std::make_unique<Applications>(state));
}

target::Outcome<platform::ModelPeerAuthenticator> fail_closed_authenticator() {
  return platform::ModelPeerAuthenticator{
      [](grpc::ServerContext&) -> target::Outcome<target::ModelCaller> {
        // P11c/P11d install the Identity/Control delegation gate before readiness can be true.
        return target::Failure{target::ErrorCode::Unauthenticated,
                               target::Acceptance::NotAccepted,
                               target::Recovery::Authenticate,
                               {},
                               {}};
      }};
}

} // namespace

target::Outcome<platform::ServiceComposition>
compose_model_service(const platform::ProcessConfig& config) {
  try {
    if (config.service != target::Service::Model) {
      return target::Failure{target::ErrorCode::InvalidArgument};
    }
    if (!config.database || !config.model_security) {
      // P02 lifecycle/smoke fixtures start every process without a database. Keep the documented
      // non-ready stub; the composed P11a service requires a database and the root key file.
      return compose_database_service<target::ModelUnitOfWork>(
          config, kiarabot::infrastructure::postgres::open_model,
          [](auto&) { return target::make_model_application(); });
    }
    auto state = std::make_shared<State>();
    auto composed = compose_database_service<target::ModelUnitOfWork>(
        config, kiarabot::infrastructure::postgres::open_model,
        [&config, &state](auto& factory) -> std::unique_ptr<target::ServiceReadinessPort> {
          auto built = build_application(config, factory, state);
          if (const auto* failure = std::get_if<target::Failure>(&built)) {
            return {};
          }
          return std::move(std::get<std::unique_ptr<target::ServiceReadinessPort>>(built));
        });
    if (const auto* failure = std::get_if<target::Failure>(&composed)) {
      return *failure;
    }
    auto result = std::move(std::get<platform::ServiceComposition>(composed));
    auto base_dependencies = std::move(result.check_dependencies);
    result.check_dependencies = [base = std::move(base_dependencies), state]() -> target::Status {
      if (base) {
        auto checked = base();
        if (std::holds_alternative<target::Failure>(checked)) {
          return checked;
        }
      }
      return state->application->maintenance();
    };
    auto base_configuration = std::move(result.configure_rpc);
    result.configure_rpc = [base = std::move(base_configuration),
                            state](platform::ServiceRpc& rpc) mutable -> target::Status {
      if (base) {
        auto configured = base(rpc);
        if (const auto* failure = std::get_if<target::Failure>(&configured)) {
          return *failure;
        }
      }
      auto authenticator = fail_closed_authenticator();
      if (const auto* failure = std::get_if<target::Failure>(&authenticator)) {
        return *failure;
      }
      auto adapter = platform::make_model_rpc(
          *state->application,
          std::move(std::get<platform::ModelPeerAuthenticator>(authenticator)));
      if (const auto* failure = std::get_if<target::Failure>(&adapter)) {
        return *failure;
      }
      return rpc.add_service(
          target::Service::Model,
          std::move(std::get<std::unique_ptr<v1::ModelService::Service>>(adapter)));
    };
    return result;
  } catch (...) {
    return target::Failure{target::ErrorCode::Internal};
  }
}

} // namespace kiarabot::core::composition
