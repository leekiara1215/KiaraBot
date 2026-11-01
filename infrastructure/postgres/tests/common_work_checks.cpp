#include "common_work_checks.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

#include <pqxx/pqxx>

#include "../src/identity_security.hpp"
#include "../src/record_access.hpp"

namespace kiarabot::infrastructure::postgres::test {
namespace {
namespace target = core::target;
void check(bool condition, const char* reason) {
  if (!condition) {
    std::cerr << "P03 assertion: " << reason << '\n';
    throw std::runtime_error("common storage test rejected");
  }
}
template <typename Value>
Value take(target::Outcome<Value> result,
           const std::source_location location = std::source_location::current()) {
  if (const auto* failure = std::get_if<target::Failure>(&result)) {
    std::cerr << "P03 assertion: unexpected stable storage code " << static_cast<int>(failure->code)
              << " at line " << location.line() << '\n';
  }
  check(std::holds_alternative<Value>(result), "unexpected storage failure");
  return std::move(std::get<Value>(result));
}
template <typename Value>
void expect(const target::Outcome<Value>& result, target::ErrorCode code) {
  check(std::holds_alternative<target::Failure>(result) &&
            std::get<target::Failure>(result).code == code,
        "wrong storage rejection");
}
std::string quote(std::string_view value) {
  std::string quoted = "'";
  for (char character : value) {
    if (character == '\\' || character == '\'') {
      quoted += '\\';
    }
    quoted += character;
  }
  return quoted + '\'';
}
std::string migration_dsn(const DatabaseConfig& config) {
  return "host=" + quote(config.host) + " port=" + std::to_string(config.port) +
         " dbname=" + quote(config.database) + " user=" + quote(config.migration_role) +
         " password=" + quote(config.password) +
         " sslmode=verify-full channel_binding=require connect_timeout=3 sslrootcert=" +
         quote(config.root_certificate);
}
std::unique_ptr<target::UnitOfWork> begin(Database& database) {
  return take(database.begin());
}
void committed(target::UnitOfWork& work) {
  check(work.commit() == target::CommitResult::Committed, "common transaction did not commit");
}
void rejected(target::UnitOfWork& work) {
  check(work.commit() == target::CommitResult::RolledBack, "rejected transaction committed");
}
} // namespace

void check_identity_credentials(Database& database) {
  const auto created = target::TimePoint{std::chrono::nanoseconds{137}};
  const auto expires = target::TimePoint{std::chrono::nanoseconds{731}};
  target::CredentialVersionRecord password{
      {{"credential-password"}, {1}},
      {"credential-owner"},
      target::PendingCredentialMaterial{target::CredentialKind::Password},
      target::CredentialState::Pending,
      created,
      {},
      {target::ResourceKind::Catalog, {"catalog"}},
      {}};
  target::Sha256Digest old_fingerprint{};
  old_fingerprint.bytes[0] = std::byte{0xab};
  auto certificate = password;
  certificate.ref.id = {"credential-certificate"};
  certificate.state = target::CredentialState::Active;
  certificate.expires_at = expires;
  certificate.material =
      target::CertificateBinding{{"credential-owner"},      old_fingerprint, "registered-san",
                                 target::Service::Identity, created,         expires};
  {
    auto access = take(detail::DatabaseAccess::begin(database));
    take(detail::save_record(
        *access.work, detail::RecordKind::Principal,
        target::Principal{{"credential-owner"}, target::PrincipalKind::Human, {}, {1}}, {}));
    auto credentials = detail::credential_repository(*access.sql);
    take(credentials->save(password, {}));
    take(credentials->save(certificate, {}));
    auto workload = password;
    workload.ref.id = {"credential-workload"};
    workload.state = target::CredentialState::Active;
    workload.expires_at = expires;
    workload.material = target::WorkloadBootstrapVerifier{
        {target::TokenPurpose::WorkloadBootstrap, {"bootstrap-key"}, {}},
        workload.binding,
        {target::Service::Control, target::Service::Identity, target::Service::Control},
        expires};
    take(credentials->save(workload, {}));
    workload.ref.version = {2};
    workload.state = target::CredentialState::Revoked;
    workload.revoked_at = expires;
    take(credentials->save(workload, target::ResourceVersion{1}));
    check(std::get<target::WorkloadBootstrapVerifier>(
              take(credentials->current(workload.ref.id))->material)
                  .audiences ==
              std::get<target::WorkloadBootstrapVerifier>(workload.material).audiences,
          "workload credential audience ordering/content changed");
    take(credentials->save_login(
        {"normalized-login", {"credential-owner"}, {"credential-password"}, {1}}, {}));
    committed(*access.work);
  }
  {
    auto access = take(detail::DatabaseAccess::begin(database));
    auto credentials = detail::credential_repository(*access.sql);
    check(take(credentials->current(password.ref.id))->created_at == created,
          "credential nanoseconds changed");
    check(take(credentials->login("normalized-login"))->password_credential == password.ref.id,
          "login index did not resolve credential");
    check(!take(credentials->current({"missing"})), "missing credential not optional");
    password.ref.version = {2};
    password.state = target::CredentialState::Active;
    password.expires_at = expires;
    password.material = target::PasswordVerifier{"encoded-verifier-only", {1}};
    take(credentials->save(password, target::ResourceVersion{1}));
    take(credentials->save_login(
        {"normalized-login", {"credential-owner"}, {"credential-password"}, {2}},
        target::ResourceVersion{1}));
    check(std::holds_alternative<target::PendingCredentialMaterial>(
              take(credentials->version({password.ref.id, {1}}))->material),
          "credential update rewrote immutable history");
    certificate.ref.version = {2};
    auto new_fingerprint = old_fingerprint;
    new_fingerprint.bytes[0] = std::byte{0xac};
    std::get<target::CertificateBinding>(certificate.material).fingerprint = new_fingerprint;
    take(credentials->save(certificate, target::ResourceVersion{1}));
    check(!take(credentials->by_certificate(old_fingerprint)),
          "historical certificate resolved a different current head");
    check(take(credentials->by_certificate(new_fingerprint))->ref.version.value == 2,
          "current certificate index failed");
    check(std::get<target::CertificateBinding>(
              take(credentials->version({certificate.ref.id, {1}}))->material)
                  .fingerprint == old_fingerprint,
          "certificate history changed");
    committed(*access.work);
  }
  {
    auto access = take(detail::DatabaseAccess::begin(database));
    auto credentials = detail::credential_repository(*access.sql);
    expect(credentials->save(password, target::ResourceVersion{1}), target::ErrorCode::Conflict);
    rejected(*access.work);
  }
  {
    auto access = take(detail::DatabaseAccess::begin(database));
    auto credentials = detail::credential_repository(*access.sql);
    expect(credentials->login(std::string("normalized-login\0suffix", 23)),
           target::ErrorCode::InvalidArgument);
    rejected(*access.work);
  }
  {
    auto access = take(detail::DatabaseAccess::begin(database));
    auto credentials = detail::credential_repository(*access.sql);
    auto malformed = password;
    malformed.ref.version = {3};
    std::get<target::PasswordVerifier>(malformed.material).encoded = std::string("phc\0suffix", 10);
    expect(credentials->save(malformed, target::ResourceVersion{2}),
           target::ErrorCode::InvalidArgument);
    rejected(*access.work);
  }
  {
    auto access = take(detail::DatabaseAccess::begin(database));
    auto credentials = detail::credential_repository(*access.sql);
    take(access.sql->run([](pqxx::transaction_base& transaction) -> target::Status {
      transaction.exec("DELETE FROM kiarabot.credential_passwords WHERE id='credential-password' "
                       "AND version=2");
      return std::monostate{};
    }));
    expect(credentials->current(password.ref.id), target::ErrorCode::Internal);
    rejected(*access.work);
  }
  {
    auto access = take(detail::DatabaseAccess::begin(database));
    auto bootstrap = detail::bootstrap_repository(*access.sql);
    check(take(bootstrap->state()).state == target::BootstrapState::Required,
          "bootstrap marker not explicit Required");
    take(bootstrap->consume(
        {target::BootstrapState::Consumed, {2}, target::SubjectId{"credential-owner"}, created},
        {1}));
    check(take(bootstrap->state()).state == target::BootstrapState::Consumed,
          "bootstrap consume not visible in same transaction");
    access.work->rollback();
  }
  {
    auto access = take(detail::DatabaseAccess::begin(database));
    auto bootstrap = detail::bootstrap_repository(*access.sql);
    check(take(bootstrap->state()).state == target::BootstrapState::Required,
          "rollback consumed bootstrap");
    expect(
        bootstrap->consume(
            {target::BootstrapState::Consumed, {3}, target::SubjectId{"credential-owner"}, created},
            {2}),
        target::ErrorCode::Conflict);
    rejected(*access.work);
  }
  {
    auto access = take(detail::DatabaseAccess::begin(database));
    auto bootstrap = detail::bootstrap_repository(*access.sql);
    take(access.sql->run([](pqxx::transaction_base& transaction) -> target::Status {
      transaction.exec("DELETE FROM kiarabot.administrator_bootstrap");
      return std::monostate{};
    }));
    expect(bootstrap->state(), target::ErrorCode::Internal);
    rejected(*access.work);
  }
}

void check_common(Database& database, const DatabaseConfig& config) {
  if (config.service == target::Service::Identity) {
    check_identity_credentials(database);
  }
  constexpr auto kHigh = std::uint64_t{1} << 63;
  constexpr auto kMaximum = std::numeric_limits<std::uint64_t>::max();
  {
    auto access = take(detail::DatabaseAccess::begin(database));
    const auto sampled = take(access.work->current_time());
    target::Operation provisional{
        {config.service, {"borrowed-rollback"}, {target::ResourceKind::Catalog, {"resource"}}},
        {1},
        target::OperationState::Blocked,
        target::ExecutionOutcome::Unknown,
        target::ErrorCode::Timeout,
        sampled};
    take(access.work->operations().save(provisional, {}));
    take(access.sql->run([](pqxx::transaction_base& transaction) -> target::Status {
      check(transaction
                    .exec("SELECT count(*) FROM kiarabot.operations WHERE "
                          "id='borrowed-rollback'")
                    .one_row()[0]
                    .as<int>() == 1,
            "SQL borrow did not observe inherited repository transaction");
      return std::monostate{};
    }));
    expect(access.sql->run([](pqxx::transaction_base&) -> target::Status {
      return target::Failure{target::ErrorCode::Conflict,
                             target::Acceptance::NotAccepted,
                             target::Recovery::Diagnose,
                             {},
                             {}};
    }),
           target::ErrorCode::Conflict);
    rejected(*access.work);
    expect(access.sql->run([](pqxx::transaction_base&) -> target::Status {
      throw std::runtime_error("terminal borrow must not invoke callback");
    }),
           target::ErrorCode::UpstreamUnavailable);
  }
  auto work = begin(database);
  check(!take(work->operations().find({"borrowed-rollback"})),
        "borrow failure committed inherited repository mutation");
  const auto now = take(work->current_time());
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  check(take(work->current_time()) > now, "database clock was transaction-frozen");
  target::Operation operation{
      {config.service, {"operation"}, {target::ResourceKind::Catalog, {"resource"}}},
      {kHigh},
      target::OperationState::Blocked,
      target::ExecutionOutcome::Unknown,
      target::ErrorCode::Timeout,
      now};
  take(work->operations().save(operation, {}));
  take(work->journal().append_audit(
      {{"actor"}, operation.ref.resource, "request", "trace", "create", {}, {kMaximum}, now}));
  committed(*work);
  work = begin(database);
  check(!take(work->operations().find({"absent"})), "absence was not optional");
  check(take(work->operations().find({"operation"}))->version.value == kHigh, "2^63 truncated");
  operation.version = {kHigh + 1};
  take(work->operations().save(operation, target::ResourceVersion{kHigh}));
  committed(*work);
  work = begin(database);
  expect(work->operations().save(operation, target::ResourceVersion{kHigh}),
         target::ErrorCode::Conflict);
  rejected(*work);
  work = begin(database);
  operation.ref.id = {"maximum-operation"};
  operation.version = {kMaximum};
  take(work->operations().save(operation, {}));
  committed(*work);
  work = begin(database);
  operation.version = {0};
  expect(work->operations().save(operation, target::ResourceVersion{kMaximum}),
         target::ErrorCode::InvalidArgument);
  rejected(*work);

  target::HandoffContext handoff{config.service,
                                 {"handoff"},
                                 {target::Service::Control,
                                  {"origin-operation"},
                                  {target::ResourceKind::Catalog, {"origin-resource"}}},
                                 target::MessageId{"cause"},
                                 {target::ResourceKind::Instance, {"resource"}},
                                 {kMaximum},
                                 {"actor"},
                                 {kHigh},
                                 {},
                                 now,
                                 now + std::chrono::hours(1)};
  handoff.created_at -= target::TimePoint::duration{137};
  handoff.valid_until += target::TimePoint::duration{731};
  handoff.payload_hash.bytes[31] = std::byte{0xff};
  const std::vector<target::ReplayResult> snapshots{
      target::EmptySuccess{},
      target::ResourceMutationResult{handoff.resource, {kMaximum}},
      operation,
      target::Principal{{"principal"},
                        target::PrincipalKind::PlatformService,
                        handoff.resource,
                        {kMaximum},
                        target::LifecycleState::Disabled},
      target::LoginSession{
          {"session"}, {"principal"}, {kMaximum}, kMaximum, target::TimePoint::max(), true},
      target::ResourceGrant{{"grant"},
                            {"principal"},
                            target::FixedRole::Operator,
                            {{handoff.resource, "plugin.manage"},
                             {{target::ResourceKind::Account, {"resource"}}, "account.manage"}},
                            {kMaximum}},
      target::BotAccount{{"account"},
                         "platform",
                         "00001",
                         "机器人🙂",
                         {kMaximum},
                         target::LifecycleState::Deleting},
      target::PluginInstallation{
          {"installation"}, {"plugin-type"}, {kMaximum}, target::LifecycleState::Restoring},
      target::PluginInstance{
          {"instance"},
          {"installation"},
          {"plugin-type"},
          {kHigh},
          {kMaximum},
          target::Fence{
              {"instance"}, {"epoch"}, {"execution"}, {kMaximum}, {"connection"}, {kHigh}},
          {kMaximum},
          target::LifecycleState::Purged,
          target::ResourceId{"host"}},
      target::ScheduledTask{{{"instance"}, "task"},
                            {"installation"},
                            {{"adapter"}, {"account"}, "001", target::ConversationKey::Kind::Group},
                            {std::byte{0}, std::byte{0xff}},
                            now,
                            target::TaskState::Retrying,
                            {kMaximum},
                            {"occurrence"}},
      target::Occurrence{{"instance"},
                         {"installation"},
                         "task",
                         {kMaximum},
                         {"occurrence"},
                         now,
                         handoff,
                         {kHigh},
                         target::OperationState::Succeeded,
                         std::string("event"),
                         now},
      target::ModelIntent{{"instance"},
                          "logical-call",
                          {"model"},
                          {kMaximum},
                          handoff.payload_hash,
                          target::ModelCallState::Unknown,
                          {kHigh},
                          target::TimePoint::min(),
                          target::TimePoint::max()}};
  for (std::size_t index = 0; index < snapshots.size(); ++index) {
    work = begin(database);
    target::CommandReceipt receipt{
        {{"actor"}, "snapshot", handoff.resource, "key-" + std::to_string(index)},
        handoff.payload_hash,
        snapshots[index],
        now,
        now + std::chrono::hours(168)};
    const auto written = take(work->receipts().insert(receipt, {100}));
    check(!written.duplicate && written.receipt.result.index() == index, "snapshot kind lost");
    check(written.receipt.retain_until - written.receipt.created_at >= std::chrono::hours(168),
          "receipt below seven days");
    committed(*work);
    work = begin(database);
    const auto duplicate = take(work->receipts().insert(receipt, {0}));
    check(duplicate.duplicate && duplicate.receipt.created_at == written.receipt.created_at &&
              duplicate.receipt.retain_until == written.receipt.retain_until,
          "duplicate extended retention");
    const auto& replay = duplicate.receipt.result;
    switch (index) {
    case 1:
      check(std::get<target::ResourceMutationResult>(replay).version.value == kMaximum,
            "mutation truncated");
      break;
    case 2:
      check(std::get<target::Operation>(replay).ref == operation.ref, "operation reference lost");
      break;
    case 3:
      check(std::get<target::Principal>(replay).binding == handoff.resource,
            "principal binding lost");
      break;
    case 4:
      check(std::get<target::LoginSession>(replay).rotation == kMaximum &&
                std::get<target::LoginSession>(replay).revoked &&
                std::get<target::LoginSession>(replay).expires_at == target::TimePoint::max(),
            "rotation lost");
      break;
    case 5:
      check(std::get<target::ResourceGrant>(replay).scopes.size() == 2 &&
                std::get<target::ResourceGrant>(replay).scopes[1].resource.kind ==
                    target::ResourceKind::Account,
            "grant scope lost");
      break;
    case 6:
      check(std::get<target::BotAccount>(replay).display_name == "机器人🙂" &&
                std::get<target::BotAccount>(replay).platform_account_id == "00001",
            "platform id changed");
      break;
    case 7:
      check(std::get<target::PluginInstallation>(replay).version.value == kMaximum,
            "installation version lost");
      break;
    case 8:
      check(std::get<target::PluginInstance>(replay).current->execution_generation.value ==
                    kMaximum &&
                std::get<target::PluginInstance>(replay).host->value == "host",
            "fence lost");
      break;
    case 9:
      check(std::get<target::ScheduledTask>(replay).payload ==
                    std::get<target::ScheduledTask>(snapshots[index]).payload &&
                std::get<target::ScheduledTask>(replay).run_at == now,
            "task payload/time lost");
      break;
    case 10:
      check(std::get<target::Occurrence>(replay).handoff.operation == handoff.operation &&
                std::get<target::Occurrence>(replay).acknowledged_at == now &&
                std::get<target::Occurrence>(replay).handoff.created_at == handoff.created_at &&
                std::get<target::Occurrence>(replay).handoff.valid_until == handoff.valid_until,
            "origin/time lost");
      break;
    case 11:
      check(std::get<target::ModelIntent>(replay).request_hash == handoff.payload_hash &&
                std::get<target::ModelIntent>(replay).config_version.value == kMaximum &&
                std::get<target::ModelIntent>(replay).created_at == target::TimePoint::min() &&
                std::get<target::ModelIntent>(replay).observed_at == target::TimePoint::max(),
            "model version/hash lost");
      break;
    default:
      break;
    }
    committed(*work);
  }
  // Reject unrepresentable SQL TEXT in complete snapshots, including optional/nested fields.
  std::vector<target::ReplayResult> nul_results(snapshots.begin() + 1, snapshots.end());
  const auto nul = std::string(1, '\0') + "suffix";
  std::get<target::ResourceMutationResult>(nul_results[0]).resource.id.value += nul;
  std::get<target::Operation>(nul_results[1]).ref.resource.id.value += nul;
  std::get<target::Principal>(nul_results[2]).binding->id.value += nul;
  std::get<target::LoginSession>(nul_results[3]).subject.value += nul;
  std::get<target::ResourceGrant>(nul_results[4]).scopes[0].permission += nul;
  std::get<target::BotAccount>(nul_results[5]).display_name += nul;
  std::get<target::PluginInstallation>(nul_results[6]).plugin_type.value += nul;
  std::get<target::PluginInstance>(nul_results[7]).host->value += nul;
  std::get<target::ScheduledTask>(nul_results[8]).target.conversation_id += nul;
  std::get<target::Occurrence>(nul_results[9]).accepted_event_id->append(nul);
  std::get<target::ModelIntent>(nul_results[10]).logical_call_id += nul;
  auto nul_fence = snapshots[8];
  std::get<target::PluginInstance>(nul_fence).current->connection.value += nul;
  nul_results.push_back(std::move(nul_fence));
  for (std::size_t index = 0; index < nul_results.size(); ++index) {
    work = begin(database);
    target::CommandReceipt invalid{
        {{"actor"}, "nul-result", handoff.resource, "nul-" + std::to_string(index)},
        handoff.payload_hash,
        nul_results[index],
        now,
        now + std::chrono::hours(168)};
    expect(work->receipts().insert(invalid, {100}), target::ErrorCode::InvalidArgument);
    rejected(*work);
  }
  work = begin(database);
  target::ReceiptKey nul_key{{"actor"}, "snapshot", handoff.resource, "key-1" + nul};
  expect(work->receipts().find(nul_key), target::ErrorCode::InvalidArgument);
  rejected(*work);
  work = begin(database);
  auto nul_context = handoff;
  nul_context.causation->value += nul;
  expect(work->journal().enqueue({nul_context,
                                  {handoff.resource, {1}},
                                  target::OutboxLane::Ordinary,
                                  target::OutboxState::Pending,
                                  0,
                                  now,
                                  {},
                                  {}},
                                 {10, 10, 10}, now),
         target::ErrorCode::InvalidArgument);
  rejected(*work);
  work = begin(database);
  expect(work->journal().received({config.service, {handoff.message.value + nul}}),
         target::ErrorCode::InvalidArgument);
  rejected(*work);
  work = begin(database);
  auto invalid_operation = operation;
  invalid_operation.ref.id = {"nul-audit"};
  invalid_operation.version = {1};
  take(work->operations().save(invalid_operation, {}));
  expect(work->journal().append_audit(
             {{"actor"}, handoff.resource, "request", "trace" + nul, "write", {}, {1}, now}),
         target::ErrorCode::InvalidArgument);
  rejected(*work);
  work = begin(database);
  check(!take(work->operations().find({"nul-audit"})),
        "invalid TEXT partially committed business state");
  work->rollback();
  work = begin(database);
  target::CommandReceipt overflow{{{"actor"}, "snapshot", handoff.resource, "new-full"},
                                  handoff.payload_hash,
                                  target::EmptySuccess{},
                                  now,
                                  now + std::chrono::hours(168)};
  expect(work->receipts().insert(overflow, {0}), target::ErrorCode::RateLimited);
  rejected(*work);
  work = begin(database);
  auto changed = overflow;
  changed.key.idempotency_key = "key-0";
  changed.fingerprint.bytes[0] = std::byte{1};
  expect(work->receipts().insert(changed, {100}), target::ErrorCode::Conflict);
  rejected(*work);

  target::OutboxEntry outgoing{handoff,
                               {handoff.resource, {kMaximum}},
                               target::OutboxLane::Ordinary,
                               target::OutboxState::Pending,
                               0,
                               now,
                               {},
                               {}};
  work = begin(database);
  take(work->journal().enqueue(outgoing, {1, 1, 1}, now + std::chrono::hours(500)));
  take(work->journal().enqueue(outgoing, {0, 0, 0}, now));
  committed(*work);
  auto second = outgoing;
  second.context.message = {"second"};
  work = begin(database);
  expect(work->journal().enqueue(second, {1, 1, 1}, now), target::ErrorCode::RateLimited);
  rejected(*work);
  second.lane = target::OutboxLane::Control;
  work = begin(database);
  take(work->journal().enqueue(second, {1, 1, 1}, now));
  committed(*work);
  work = begin(database);
  auto claims = take(work->journal().claim(config.service, 1, now - std::chrono::hours(500),
                                           std::chrono::seconds(1)));
  check(claims.size() == 1 && claims[0].entry.context.operation == handoff.operation &&
            claims[0].entry.payload_reference.version.value == kMaximum,
        "claim envelope lost");
  const auto claim = claims[0];
  committed(*work);
  work = begin(database);
  expect(work->journal().confirm({config.service, claim.entry.context.message},
                                 {claim.claim_token.value + nul}, now),
         target::ErrorCode::InvalidArgument);
  rejected(*work);
  work = begin(database);
  expect(work->journal().confirm({config.service, claim.entry.context.message}, {"stale"}, now),
         target::ErrorCode::Conflict);
  rejected(*work);
  work = begin(database);
  take(work->journal().confirm({config.service, claim.entry.context.message}, claim.claim_token,
                               now + std::chrono::hours(500)));
  take(work->journal().confirm({config.service, claim.entry.context.message}, claim.claim_token,
                               now));
  committed(*work);
  work = begin(database);
  target::InboxRecord incoming{handoff, now, handoff.valid_until + std::chrono::hours(168),
                               target::ResourceMutationResult{handoff.resource, {kMaximum}}};
  const auto received = take(work->journal().record_received(incoming, {1, 1, 1}, now));
  check(!received.duplicate &&
            received.receipt.retain_until >= handoff.valid_until + std::chrono::hours(168),
        "inbox retention too short");
  committed(*work);
  work = begin(database);
  const auto duplicate = take(work->journal().record_received(incoming, {0, 0, 0}, now));
  check(duplicate.duplicate && duplicate.receipt.received_at == received.receipt.received_at &&
            duplicate.receipt.retain_until == received.receipt.retain_until,
        "inbox duplicate changed retention");
  committed(*work);
  work = begin(database);
  incoming.context.payload_hash.bytes[0] = std::byte{1};
  expect(work->journal().record_received(incoming, {1, 1, 1}, now), target::ErrorCode::Conflict);
  rejected(*work);
  work = begin(database);
  check(take(work->receipts().prune_expired(now + std::chrono::hours(500), 100)) == 0,
        "client future pruned valid receipt");
  check(take(work->journal().prune_received(now + std::chrono::hours(500), 100)) == 0,
        "client future pruned inbox");
  work->rollback();

  // Same message identity under another authenticated source has a separate inbox key.
  work = begin(database);
  auto other_source = incoming;
  other_source.context.payload_hash = handoff.payload_hash;
  other_source.context.source = config.service == target::Service::Identity
                                    ? target::Service::Runtime
                                    : target::Service::Identity;
  check(!take(work->journal().record_received(other_source, {10, 10, 10}, now)).duplicate,
        "inbox mixed source identities");
  committed(*work);
  // Resource kind is part of the receipt key even if its ID text and caller key are identical.
  work = begin(database);
  auto another_kind = overflow;
  another_kind.key.idempotency_key = "key-0";
  another_kind.key.resource.kind = target::ResourceKind::Account;
  check(!take(work->receipts().insert(another_kind, {100})).duplicate,
        "receipt mixed resource kinds");
  committed(*work);

  // A real expired lease may be reclaimed; the delayed old token cannot confirm the new claim.
  work = begin(database);
  auto first_claim = take(work->journal().claim(config.service, 1, now, std::chrono::seconds(30)));
  check(first_claim.size() == 1, "control outbox claim missing");
  const auto old_claim = first_claim[0];
  committed(*work);
  {
    pqxx::connection migration(migration_dsn(config));
    pqxx::work expired(migration);
    expired.exec(
        "UPDATE kiarabot.outbox SET lease_until=(extract(epoch FROM "
        "clock_timestamp())*1000000000)::bigint-1000 WHERE "
        "source=$1 AND message=$2",
        pqxx::params{static_cast<int>(config.service), old_claim.entry.context.message.value});
    expired.commit();
  }
  work = begin(database);
  auto reclaimed = take(work->journal().claim(config.service, 1, now, std::chrono::seconds(30)));
  check(reclaimed.size() == 1 && reclaimed[0].claim_token != old_claim.claim_token &&
            reclaimed[0].entry.attempts == 2,
        "reclaim did not fence old worker");
  const auto current_claim = reclaimed[0];
  committed(*work);
  work = begin(database);
  expect(work->journal().confirm({config.service, old_claim.entry.context.message},
                                 old_claim.claim_token, now),
         target::ErrorCode::Conflict);
  rejected(*work);
  work = begin(database);
  take(work->journal().confirm({config.service, current_claim.entry.context.message},
                               current_claim.claim_token, now));
  committed(*work);

  // The 100th durable claim is retained for attention, never discarded or auto-retried.
  auto ceiling = outgoing;
  ceiling.context.message = {"attempt-ceiling"};
  work = begin(database);
  take(work->journal().enqueue(ceiling, {1, 1, 1}, now));
  committed(*work);
  {
    pqxx::connection migration(migration_dsn(config));
    pqxx::work prepare(migration);
    prepare.exec(
        "UPDATE kiarabot.outbox SET attempts=99 WHERE source=$1 AND message='attempt-ceiling'",
        pqxx::params{static_cast<int>(config.service)});
    prepare.commit();
  }
  work = begin(database);
  auto last = take(work->journal().claim(config.service, 1, now, std::chrono::seconds(30)));
  check(last.size() == 1 && last[0].entry.attempts == 100, "100th claim not persisted");
  committed(*work);
  {
    pqxx::connection migration(migration_dsn(config));
    pqxx::work expire(migration);
    expire.exec(
        "UPDATE kiarabot.outbox SET lease_until=(extract(epoch FROM "
        "clock_timestamp())*1000000000)::bigint-1000 WHERE source=$1 AND message='attempt-ceiling'",
        pqxx::params{static_cast<int>(config.service)});
    expire.commit();
  }
  work = begin(database);
  check(take(work->journal().claim(config.service, 1, now, std::chrono::seconds(30))).empty(),
        "100-attempt obligation automatically retried");
  committed(*work);
  {
    pqxx::connection migration(migration_dsn(config));
    pqxx::read_transaction inspect(migration);
    check(inspect.exec("SELECT state FROM kiarabot.outbox WHERE source=$1 AND "
                       "message='attempt-ceiling'",
                       pqxx::params{static_cast<int>(config.service)})
                  .one_row()[0]
                  .as<int>() == static_cast<int>(target::OutboxState::NeedsAttention),
          "exhausted outbox obligation disappeared");
  }
  work = begin(database);
  auto blocked = outgoing;
  blocked.context.message = {"after-ceiling"};
  expect(work->journal().enqueue(blocked, {1, 1, 1}, now), target::ErrorCode::RateLimited);
  rejected(*work);

  // Expired storage is cleaned in a bounded batch; still-valid seven-day receipts remain.
  {
    pqxx::connection migration(migration_dsn(config));
    pqxx::work expired(migration);
    expired.exec("UPDATE kiarabot.command_receipts SET "
                 "created_at=created_at-691200000000000,retain_until=retain_until-691200000000000 "
                 "WHERE method='snapshot' "
                 "AND idempotency_key='key-0' AND resource_kind=$1",
                 pqxx::params{static_cast<int>(handoff.resource.kind)});
    expired.exec(
        "UPDATE kiarabot.inbox SET "
        "created_at=created_at-691200000000000,valid_until=valid_until-691200000000000,received_at="
        "received_at-691200000000000,retain_until=retain_until-691200000000000 WHERE source=$1",
        pqxx::params{static_cast<int>(config.service)});
    expired.commit();
  }
  work = begin(database);
  check(take(work->receipts().prune_expired(now, 1)) == 1, "expired receipt was not pruned");
  check(take(work->journal().prune_received(now, 1)) == 1, "expired inbox was not pruned");
  committed(*work);
  work = begin(database);
  auto still_valid = overflow;
  still_valid.key.idempotency_key = "key-1";
  check(take(work->receipts().find(still_valid.key)).has_value(),
        "prune evicted seven-day receipt");
  auto expired_message = incoming;
  expired_message.context.payload_hash = handoff.payload_hash;
  expired_message.context.created_at = now - std::chrono::seconds(2);
  expired_message.context.valid_until = now - std::chrono::seconds(1);
  expired_message.received_at = now;
  expired_message.retain_until = now + std::chrono::hours(168);
  expect(work->journal().record_received(expired_message, {100, 100, 100}, now),
         target::ErrorCode::Conflict);
  rejected(*work);

  // Composite state/receipt/audit/outbox must all disappear when a later capacity check rejects.
  work = begin(database);
  operation.ref.id = {"atomic-failure"};
  operation.version = {1};
  take(work->operations().save(operation, {}));
  auto atomic_receipt = overflow;
  atomic_receipt.key.idempotency_key = "atomic-failure";
  take(work->receipts().insert(atomic_receipt, {100}));
  take(work->journal().append_audit(
      {{"actor"}, handoff.resource, "atomic-failure", "trace", "create", {}, {1}, now}));
  auto atomic_outbox = outgoing;
  atomic_outbox.context.message = {"atomic-failure"};
  take(work->journal().enqueue(atomic_outbox, {10, 10, 10}, now));
  atomic_receipt.key.idempotency_key = "capacity-failure";
  expect(work->receipts().insert(atomic_receipt, {0}), target::ErrorCode::RateLimited);
  rejected(*work);
  work = begin(database);
  check(!take(work->operations().find({"atomic-failure"})), "partial operation survived rejection");
  atomic_receipt.key.idempotency_key = "atomic-failure";
  check(!take(work->receipts().find(atomic_receipt.key)), "partial receipt survived rejection");
  work->rollback();
  {
    pqxx::connection migration(migration_dsn(config));
    pqxx::read_transaction inspect(migration);
    check(
        inspect.exec(
                   "SELECT count(*) FROM kiarabot.audit_entries WHERE request_id='atomic-failure'")
                .one_row()[0]
                .as<int>() == 0,
        "partial audit survived rejection");
    check(inspect.exec("SELECT count(*) FROM kiarabot.outbox WHERE message='atomic-failure'")
                  .one_row()[0]
                  .as<int>() == 0,
          "partial outbox survived rejection");
  }
  // Real independent-session row lock must time out near 250ms and roll back prior writes.
  {
    pqxx::connection migration(migration_dsn(config));
    pqxx::work held(migration);
    held.exec("SELECT id FROM kiarabot.operations WHERE id='operation' FOR UPDATE");
    work = begin(database);
    auto locked = *take(work->operations().find({"operation"}));
    const auto expected = locked.version;
    ++locked.version.value;
    take(work->journal().append_audit(
        {{"actor"}, handoff.resource, "lock-failure", "trace", "update", {}, {1}, now}));
    const auto start = std::chrono::steady_clock::now();
    expect(work->operations().save(locked, expected), target::ErrorCode::Timeout);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    check(elapsed >= std::chrono::milliseconds(150) && elapsed < std::chrono::milliseconds(1500),
          "lock wait was not bounded at 250ms");
    rejected(*work);
  }
  {
    pqxx::connection migration(migration_dsn(config));
    pqxx::read_transaction inspect(migration);
    check(
        inspect.exec("SELECT count(*) FROM kiarabot.audit_entries WHERE request_id='lock-failure'")
                .one_row()[0]
                .as<int>() == 0,
        "timeout left partial audit");
  }
  // A missing typed child is corruption, not an absent receipt. Restore only our fixture row.
  std::int64_t corrupted_result = 0;
  {
    pqxx::connection migration(migration_dsn(config));
    pqxx::work corrupt(migration);
    corrupted_result = corrupt
                           .exec("SELECT result_id FROM kiarabot.command_receipts WHERE "
                                 "subject='actor' AND method='snapshot' AND resource_kind=$1 AND "
                                 "resource_id='resource' AND idempotency_key='key-1'",
                                 pqxx::params{static_cast<int>(handoff.resource.kind)})
                           .one_row()[0]
                           .as<std::int64_t>();
    check(corrupt.exec("DELETE FROM kiarabot.result_mutations WHERE result_id=$1",
                       pqxx::params{corrupted_result})
                  .affected_rows() == 1,
          "corruption fixture row missing");
    corrupt.commit();
  }
  work = begin(database);
  auto corrupted = overflow;
  corrupted.key.idempotency_key = "key-1";
  expect(work->receipts().find(corrupted.key), target::ErrorCode::Internal);
  rejected(*work);
  {
    pqxx::connection migration(migration_dsn(config));
    pqxx::work restore(migration);
    restore.exec("INSERT INTO kiarabot.result_mutations VALUES($1,$2,$3,$4)",
                 pqxx::params{corrupted_result, static_cast<int>(handoff.resource.kind),
                              handoff.resource.id.value, kMaximum});
    restore.commit();
  }
  work = begin(database);
  check(std::get<target::ResourceMutationResult>(take(work->receipts().find(corrupted.key))->result)
                .version.value == kMaximum,
        "restored fixture result not readable");
  work->rollback();
}

void check_dependency_fault(const DatabaseConfig& migration, const DatabaseConfig& runtime) {
  auto database = take(Database::open(runtime));
  {
    pqxx::connection injector(migration_dsn(migration));
    pqxx::work setup(injector);
    setup.exec("CREATE SCHEMA p03_fault_owned");
    setup.exec(
        "CREATE FUNCTION p03_fault_owned.fail_dependency() RETURNS trigger LANGUAGE plpgsql AS $$ "
        "BEGIN RAISE EXCEPTION USING ERRCODE='53100',MESSAGE='fixture dependency failure'; END $$");
    setup.exec("CREATE TRIGGER p03_dependency BEFORE INSERT ON kiarabot.audit_entries FOR EACH ROW "
               "EXECUTE FUNCTION p03_fault_owned.fail_dependency()");
    setup.commit();
  }
  auto work = begin(*database);
  const auto now = take(work->current_time());
  target::Operation operation{
      {runtime.service, {"dependency-failure"}, {target::ResourceKind::Instance, {"resource"}}},
      {1},
      target::OperationState::Accepted,
      target::ExecutionOutcome::NotStarted,
      {},
      {}};
  take(work->operations().save(operation, {}));
  expect(work->journal().append_audit({{"actor"},
                                       operation.ref.resource,
                                       "dependency-failure",
                                       "trace",
                                       "write",
                                       {},
                                       {1},
                                       now}),
         target::ErrorCode::UpstreamUnavailable);
  rejected(*work);
  expect(database->begin(), target::ErrorCode::UpstreamUnavailable);
  {
    pqxx::connection injector(migration_dsn(migration));
    pqxx::work cleanup(injector);
    cleanup.exec("DROP TRIGGER p03_dependency ON kiarabot.audit_entries");
    cleanup.exec("DROP FUNCTION p03_fault_owned.fail_dependency()");
    cleanup.exec("DROP SCHEMA p03_fault_owned");
    cleanup.commit();
  }
  auto recovered = take(Database::open(runtime));
  auto query = begin(*recovered);
  check(!take(query->operations().find({"dependency-failure"})),
        "dependency failure committed partial state");
  query->rollback();
}

void check_deadline(const DatabaseConfig& config) {
  auto runtime = config;
  runtime.user = config.runtime_role;
  auto database = take(Database::open(runtime));
  auto work = begin(*database);
  target::Operation operation{
      {config.service, {"deadline-write"}, {target::ResourceKind::Instance, {"resource"}}},
      {1},
      target::OperationState::Accepted,
      target::ExecutionOutcome::NotStarted,
      {},
      {}};
  take(work->operations().save(operation, {}));
  std::thread concurrent([&] { expect(database->begin(), target::ErrorCode::Conflict); });
  concurrent.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  const auto sampled = work->current_time();
  check(std::holds_alternative<target::Failure>(sampled), "expired UoW returned a clock sample");
  rejected(*work);
  expect(database->begin(), target::ErrorCode::UpstreamUnavailable);
  auto fresh = take(Database::open(runtime));
  auto query = begin(*fresh);
  check(!take(query->operations().find({"deadline-write"})), "deadline write escaped rollback");
  check(!take(query->operations().find({"killed-write"})), "killed backend write escaped rollback");
  query->rollback();
}

void check_unknown_commit(const DatabaseConfig& config, const std::filesystem::path& root,
                          std::uint16_t proxy_port) {
  auto proxied = config;
  proxied.user = config.runtime_role;
  proxied.port = proxy_port;
  auto database = take(Database::open(proxied));
  auto work = begin(*database);
  const auto now = take(work->current_time());
  target::CommandReceipt receipt{
      {{"actor"}, "lost-commit", {target::ResourceKind::Instance, {"resource"}}, "original-key"},
      {},
      target::ResourceMutationResult{{target::ResourceKind::Instance, {"resource"}},
                                     {std::numeric_limits<std::uint64_t>::max()}},
      now,
      now + std::chrono::hours(168)};
  take(work->receipts().insert(receipt, {100}));
  std::ofstream(root / "drop-commit-arm") << "arm";
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!std::filesystem::exists(root / "drop-commit-ready")) {
    check(std::chrono::steady_clock::now() < deadline, "relay did not arm");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  check(work->commit() == target::CommitResult::Unknown, "actual UoW commit was not Unknown");
  work->rollback();
  expect(database->begin(), target::ErrorCode::UpstreamUnavailable);
  check(std::filesystem::exists(root / "commit-receipt-dropped"), "relay did not drop receipt");
  auto direct = config;
  direct.user = config.runtime_role;
  auto recovered = take(Database::open(direct));
  auto query = begin(*recovered);
  const auto stored = take(query->receipts().find(receipt.key));
  check(stored && std::get<target::ResourceMutationResult>(stored->result).version.value ==
                      std::numeric_limits<std::uint64_t>::max(),
        "same identity could not recover committed result");
  query->rollback();
}
} // namespace kiarabot::infrastructure::postgres::test
