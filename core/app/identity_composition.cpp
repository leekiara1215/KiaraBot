#include "identity_composition.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "identity_security_clock.hpp"
#include "kiarabot/core/application/target/identity_admin_application.hpp"
#include "kiarabot/core/application/target/identity_application.hpp"
#include "kiarabot/core/application/target/identity_delegation_application.hpp"
#include "kiarabot/core/application/target/identity_security_application.hpp"
#include "kiarabot/core/runtime/platform/identity_rpc.hpp"
#include "kiarabot/core/runtime/platform/service_rpc.hpp"
#include "kiarabot/infrastructure/identity_crypto/key_files.hpp"
#include "kiarabot/infrastructure/identity_crypto/peer_certificate.hpp"
#include "kiarabot/infrastructure/postgres/security_factory.hpp"
#include "postgres_service.hpp"

namespace kiarabot::core::composition {
namespace {
namespace crypto = kiarabot::infrastructure::identity_crypto;
namespace postgres = kiarabot::infrastructure::postgres;

target::Failure invalid_composition() {
  return {target::ErrorCode::InvalidArgument,
          target::Acceptance::NotAccepted,
          target::Recovery::CorrectInput,
          {},
          {}};
}
struct Dependencies {
  Dependencies(const platform::IdentityClockDeclaration& declaration,
               platform::TlsValidity validity)
      : clock(declaration, validity) {}
  // Declaration order: all borrowers and the PG factory die before the clock.
  SystemSecurityClock clock;
  std::unique_ptr<target::RandomPort> random;
  std::unique_ptr<target::PasswordCryptoPort> passwords;
  std::unique_ptr<target::TokenCryptoPort> tokens;
  std::unique_ptr<target::SecretReceiptCryptoPort> receipts;
  std::unique_ptr<target::PeerCertificatePort> certificates;
  std::unique_ptr<target::DelegationCryptoPort> delegations;
  std::unique_ptr<target::IdentitySecurityUnitOfWorkFactory> transactions;
  std::atomic<bool> database_available{false};
  std::atomic<bool> clock_reported{false};
};
class Applications final : public target::ServiceReadinessPort {
public:
  Applications(std::shared_ptr<Dependencies> dependencies,
               std::unique_ptr<target::IdentitySecurityApplication> sessions,
               const platform::IdentitySecurityMaterial& material)
      : dependencies_(std::move(dependencies)), sessions_(std::move(sessions)),
        admin_(*dependencies_->transactions, *dependencies_->certificates),
        issuer_(*sessions_, *dependencies_->transactions, *dependencies_->delegations,
                *dependencies_->random, dependencies_->clock,
                {material.issuer, {material.delegation_key.id}}) {}
  target::Service service() const noexcept override {
    return target::Service::Identity;
  }
  target::ServiceReadiness readiness() const override {
    auto result = sessions_->readiness();
    const bool database = dependencies_->database_available.load();
    const bool clock = dependencies_->clock.healthy();
    for (auto& entry : result.dependencies) {
      if (entry.dependency == target::Dependency::DatabaseSchema) {
        entry.state =
            database ? target::DependencyState::Available : target::DependencyState::Unavailable;
      }
      if (entry.dependency == target::Dependency::Credentials) {
        entry.state =
            clock ? target::DependencyState::Available : target::DependencyState::Unavailable;
      }
    }
    // The pure application still declares incomplete Identity-wide readiness. Never upgrade it.
    result.ready = result.ready && database && clock;
    return result;
  }
  target::IdentitySecurityApplication& sessions() {
    return *sessions_;
  }
  target::IdentityAdminApplication& admin() {
    return admin_;
  }
  target::IdentityDelegationApplication& issuer() {
    return issuer_;
  }

private:
  // Reverse destruction releases issuer -> admin -> sessions -> shared dependencies.
  std::shared_ptr<Dependencies> dependencies_;
  std::unique_ptr<target::IdentitySecurityApplication> sessions_;
  target::IdentityAdminApplication admin_;
  target::IdentityDelegationApplication issuer_;
};
target::Outcome<target::PasswordVerifier> dummy_password(Dependencies& dependencies) {
  auto generated = dependencies.random->bytes(32);
  if (const auto* error = std::get_if<target::Failure>(&generated)) {
    return *error;
  }
  const auto& secret = std::get<target::OwnedSecret>(generated);
  if (!secret || secret->bytes().size() != 32) {
    return detail::database_unavailable();
  }
  // Secure random buffer is destroyed/erased before returning the PHC, never persisted/logged.
  return dependencies.passwords->derive(secret->bytes());
}
target::Status maintain(const std::shared_ptr<Dependencies>& dependencies) {
  const auto clock = dependencies->clock.sample();
  if (std::holds_alternative<target::Failure>(clock) &&
      !dependencies->clock_reported.exchange(true)) {
    std::cerr << "operation=identity_clock outcome=unavailable error_code=UPSTREAM_UNAVAILABLE\n";
  }
  if (dependencies->clock.credentials_expired()) {
    return detail::database_unavailable(); // Runner stops/Waits RPC, including existing channels.
  }
  auto begun = dependencies->transactions->begin();
  if (const auto* error = std::get_if<target::Failure>(&begun)) {
    if (error->code == target::ErrorCode::Conflict &&
        error->acceptance == target::Acceptance::NotAccepted) {
      return std::monostate{}; // Bounded writer contention is not lost ownership.
    }
    dependencies->database_available.store(false);
    return *error;
  }
  auto work = std::move(std::get<std::unique_ptr<target::IdentitySecurityUnitOfWork>>(begun));
  if (!work) {
    dependencies->database_available.store(false);
    return detail::database_unavailable();
  }
  const auto now = work->current_time();
  work->rollback();
  if (const auto* error = std::get_if<target::Failure>(&now)) {
    dependencies->database_available.store(false);
    return *error;
  }
  dependencies->database_available.store(true);
  // A clock fault stays alive but not-ready; every RPC's real peer authenticator samples it and
  // rejects with dependency failure. No restart/recalibration or cached authorization is used.
  return std::monostate{};
}
target::Outcome<platform::ServiceComposition> configured(const platform::ProcessConfig& config) {
  if (config.service != target::Service::Identity || !config.database ||
      !config.identity_security || !config.tls.validity) {
    return invalid_composition();
  }
  const auto& material = *config.identity_security;
  auto dependencies = std::make_shared<Dependencies>(material.clock, *config.tls.validity);
  auto token_key =
      crypto::load_symmetric_key_file({{material.token_key.id}, material.token_key.file});
  if (const auto* error = std::get_if<target::Failure>(&token_key)) {
    return *error;
  }
  auto request_key =
      crypto::load_symmetric_key_file({{material.request_key.id}, material.request_key.file});
  if (const auto* error = std::get_if<target::Failure>(&request_key)) {
    return *error;
  }
  auto receipt_key =
      crypto::load_symmetric_key_file({{material.receipt_key.id}, material.receipt_key.file});
  if (const auto* error = std::get_if<target::Failure>(&receipt_key)) {
    return *error;
  }
  auto delegation_key =
      crypto::load_delegation_key_files({{material.delegation_key.id},
                                         material.delegation_key.public_key_file,
                                         material.delegation_key.private_key_file});
  if (const auto* error = std::get_if<target::Failure>(&delegation_key)) {
    return *error;
  }
  auto initial_clock = dependencies->clock.sample();
  if (const auto* error = std::get_if<target::Failure>(&initial_clock)) {
    return *error;
  }
  auto random = crypto::make_random();
  if (const auto* error = std::get_if<target::Failure>(&random)) {
    return *error;
  }
  dependencies->random = std::move(std::get<std::unique_ptr<target::RandomPort>>(random));
  auto passwords = crypto::make_password_crypto();
  if (const auto* error = std::get_if<target::Failure>(&passwords)) {
    return *error;
  }
  dependencies->passwords =
      std::move(std::get<std::unique_ptr<target::PasswordCryptoPort>>(passwords));
  if (!dependencies->random || !dependencies->passwords) {
    return detail::database_unavailable();
  }
  auto dummy = dummy_password(*dependencies);
  if (const auto* error = std::get_if<target::Failure>(&dummy)) {
    return *error;
  }
  std::vector<crypto::SymmetricKey> token_keys;
  token_keys.push_back(std::move(std::get<crypto::SymmetricKey>(token_key)));
  token_keys.push_back(std::move(std::get<crypto::SymmetricKey>(request_key)));
  auto tokens = crypto::make_token_crypto(std::move(token_keys));
  if (const auto* error = std::get_if<target::Failure>(&tokens)) {
    return *error;
  }
  dependencies->tokens = std::move(std::get<std::unique_ptr<target::TokenCryptoPort>>(tokens));
  std::vector<crypto::SymmetricKey> receipt_keys;
  receipt_keys.push_back(std::move(std::get<crypto::SymmetricKey>(receipt_key)));
  auto receipts = crypto::make_secret_receipt_crypto(std::move(receipt_keys));
  if (const auto* error = std::get_if<target::Failure>(&receipts)) {
    return *error;
  }
  dependencies->receipts =
      std::move(std::get<std::unique_ptr<target::SecretReceiptCryptoPort>>(receipts));
  std::vector<crypto::DelegationKey> delegation_keys;
  delegation_keys.push_back(std::move(std::get<crypto::DelegationKey>(delegation_key)));
  auto delegations = crypto::make_delegation_crypto({material.issuer, std::move(delegation_keys)},
                                                    dependencies->clock);
  if (const auto* error = std::get_if<target::Failure>(&delegations)) {
    return *error;
  }
  dependencies->delegations =
      std::move(std::get<std::unique_ptr<target::DelegationCryptoPort>>(delegations));
  auto certificates =
      crypto::make_peer_certificate_crypto({{config.tls.ca}, crypto::PeerSanKind::Uri});
  if (const auto* error = std::get_if<target::Failure>(&certificates)) {
    return *error;
  }
  dependencies->certificates =
      std::move(std::get<std::unique_ptr<target::PeerCertificatePort>>(certificates));
  if (!dependencies->tokens || !dependencies->receipts || !dependencies->delegations ||
      !dependencies->certificates) {
    return detail::database_unavailable();
  }
  // All key/clock/dummy/crypto preparation is complete before acquiring a PG writer.
  const auto& database = *config.database;
  auto opened = postgres::open_identity_security(
      {config.service, database.host, database.port, database.name, database.runtime_role,
       database.password, database.ca_file, database.migration_role, database.runtime_role});
  if (const auto* error = std::get_if<target::Failure>(&opened)) {
    return *error;
  }
  dependencies->transactions =
      std::move(std::get<std::unique_ptr<target::IdentitySecurityUnitOfWorkFactory>>(opened));
  if (!dependencies->transactions) {
    return detail::database_unavailable();
  }
  dependencies->database_available.store(true);
  target::IdentitySecurityOptions options{{material.token_key.id},
                                          {{material.token_key.id}},
                                          {material.request_key.id},
                                          {material.receipt_key.id},
                                          std::move(std::get<target::PasswordVerifier>(dummy)),
                                          std::chrono::hours{365 * 24},
                                          {10000},
                                          4096,
                                          1024,
                                          4096};
  auto sessions = target::make_identity_security_application(
      {*dependencies->transactions, *dependencies->passwords, *dependencies->tokens,
       *dependencies->receipts, *dependencies->random, dependencies->clock,
       *dependencies->delegations},
      std::move(options));
  if (const auto* error = std::get_if<target::Failure>(&sessions)) {
    return *error;
  }
  auto session_application =
      std::move(std::get<std::unique_ptr<target::IdentitySecurityApplication>>(sessions));
  if (!session_application) {
    return detail::database_unavailable();
  }
  auto applications =
      std::make_unique<Applications>(dependencies, std::move(session_application), material);
  auto* observed = applications.get(); // Composition owns it until after RPC shutdown/Wait.
  auto final_clock = dependencies->clock.sample();
  if (const auto* error = std::get_if<target::Failure>(&final_clock)) {
    return *error;
  }
  return platform::ServiceComposition{
      std::move(applications), [dependencies] { return maintain(dependencies); },
      [observed, dependencies](platform::ServiceRpc& rpc) -> target::Status {
        auto authenticate =
            platform::make_identity_peer_authenticator(observed->admin(), dependencies->clock);
        if (const auto* error = std::get_if<target::Failure>(&authenticate)) {
          return *error;
        }
        auto current_peer = std::move(std::get<platform::PeerAuthenticator>(authenticate));
        platform::PeerAuthenticator gated =
            [dependencies, current_peer = std::move(current_peer)](
                grpc::ServerContext& context) -> target::Outcome<target::VerifiedPeer> {
          const auto current = dependencies->clock.sample();
          if (const auto* error = std::get_if<target::Failure>(&current)) {
            return *error;
          }
          // Existing channels also recheck the own-chain lifetime on every operation.
          // The authenticator checks the same gated clock again after Admin returns;
          // applications borrow it for their own security/commit phase checks.
          return current_peer(context);
        };
        auto adapter = platform::make_identity_rpc(observed->sessions(), std::move(gated),
                                                   &observed->issuer());
        if (const auto* error = std::get_if<target::Failure>(&adapter)) {
          return *error;
        }
        return rpc.add_service(
            target::Service::Identity,
            std::move(std::get<std::unique_ptr<kiarabot::v1::IdentityService::Service>>(adapter)));
      }};
}

} // namespace

target::Outcome<platform::ServiceComposition>
compose_identity_service(const platform::ProcessConfig& config) {
  try {
    if (!config.identity_security) {
      return compose_database_service<target::IdentityUnitOfWork>(
          config, postgres::open_identity,
          [](auto&) { return target::make_identity_application(); });
    }
    return configured(config);
  } catch (...) {
    return detail::database_unavailable();
  }
}

} // namespace kiarabot::core::composition
