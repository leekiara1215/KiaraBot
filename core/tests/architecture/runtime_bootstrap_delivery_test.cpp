#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include "kiarabot/core/application/target/runtime_resources/bootstrap.hpp"

namespace t = kiarabot::core::target;
using std::chrono_literals::operator""s;

namespace {
void check(bool value, const char* scenario) {
  if (!value) {
    std::cerr << "runtime bootstrap delivery assertion: " << scenario << '\n';
    std::exit(1);
  }
}
bool rejected_with(const t::Status& status, t::ErrorCode code) {
  const auto* failure = std::get_if<t::Failure>(&status);
  return failure != nullptr && failure->code == code;
}
template <typename Value> bool rejected_with(const t::Outcome<Value>& outcome, t::ErrorCode code) {
  const auto* failure = std::get_if<t::Failure>(&outcome);
  return failure != nullptr && failure->code == code;
}
t::BridgeCredentialRecord credential(std::uint64_t version, bool revoked = false) {
  t::BridgeCredentialRecord record;
  record.id = "11111111-1111-4111-8111-111111111111";
  record.resource_id = "22222222-2222-4222-8222-222222222222";
  record.created_at = t::TimePoint{std::chrono::seconds{100}};
  record.revoked = revoked;
  record.version = version;
  return record;
}
t::BootstrapDeliveryRecord delivery(t::BootstrapDeliveryState state, std::uint64_t version = 1) {
  t::BootstrapDeliveryRecord record;
  record.delivery_id = "delivery-one";
  record.resource = {t::ResourceKind::Account, {"33333333-3333-4333-8333-333333333333"}};
  record.recipient_id = "recipient-one";
  record.credential_version = version;
  record.state = state;
  record.operation = {t::Service::Runtime, {"operation-one"}, record.resource};
  record.created_at = t::TimePoint{std::chrono::seconds{100}};
  record.observed_at = record.created_at;
  if (state == t::BootstrapDeliveryState::Delivered) {
    record.receipt_id = "receipt-one";
  }
  return record;
}
t::ReportBootstrapDeliveryCommand report(t::BootstrapDeliveryState state) {
  t::ReportBootstrapDeliveryCommand command;
  command.context.method = "kiarabot.v1.RuntimeService/ReportAdapterBootstrapDelivery";
  command.resource = {t::ResourceKind::Account, {"33333333-3333-4333-8333-333333333333"}};
  command.delivery_id = "delivery-one";
  command.recipient_id = "recipient-one";
  command.credential_version = 1;
  command.state = state;
  command.receipt_id = "receipt-one";
  command.observed_at = t::TimePoint{std::chrono::seconds{120}};
  command.control_peer_authorized = true;
  return command;
}
t::TimePoint now() {
  return t::TimePoint{std::chrono::seconds{200}};
}

void identifiers() {
  using t::runtime_resources_detail::canonical_bootstrap_identifier;
  check(canonical_bootstrap_identifier("recipient-one"), "canonical recipient accepted");
  check(canonical_bootstrap_identifier(std::string(128, 'a')), "maximum length accepted");
  check(!canonical_bootstrap_identifier(""), "empty identifier rejected");
  check(!canonical_bootstrap_identifier(std::string(129, 'a')), "oversize identifier rejected");
  check(!canonical_bootstrap_identifier("."), "dot identifier rejected");
  check(!canonical_bootstrap_identifier(".."), "dotdot identifier rejected");
  check(!canonical_bootstrap_identifier("bad/slash"), "slash rejected");
  check(!canonical_bootstrap_identifier("bad space"), "space rejected");
  check(!canonical_bootstrap_identifier("bad\n"), "newline rejected");
  check(!canonical_bootstrap_identifier("路径"), "non-ascii rejected");
}

void versions() {
  using namespace t::runtime_resources_detail;
  auto first = next_bootstrap_credential_version(std::nullopt);
  check(std::holds_alternative<std::uint64_t>(first), "missing credential yields version one");
  check(std::get<std::uint64_t>(first) == 1, "first persisted version is one");
  auto second = next_bootstrap_credential_version(credential(1));
  check(std::get<std::uint64_t>(second) == 2, "reissue advances the persisted version");
  auto legacy = next_bootstrap_credential_version(credential(0));
  check(std::get<std::uint64_t>(legacy) == 2, "legacy version zero normalizes before advancing");
  auto revoked = next_bootstrap_credential_version(credential(7, true));
  check(std::get<std::uint64_t>(revoked) == 8, "revoked history still bounds the next version");
  auto almost =
      next_bootstrap_credential_version(credential(kMaximumBootstrapCredentialVersion - 1));
  check(std::holds_alternative<std::uint64_t>(almost) &&
            std::get<std::uint64_t>(almost) == kMaximumBootstrapCredentialVersion,
        "maximum stored version is reachable exactly once");
  auto exhausted =
      next_bootstrap_credential_version(credential(kMaximumBootstrapCredentialVersion));
  check(rejected_with(exhausted, t::ErrorCode::RateLimited), "exhausted version domain rejected");
  check(normalized_bootstrap_credential_version(credential(0)) == 1,
        "legacy record normalizes to the frozen bridge version");

  check(std::holds_alternative<std::monostate>(
            validate_expected_credential_version(std::nullopt, credential(3))),
        "absent expected version accepts the current identity");
  check(std::holds_alternative<std::monostate>(
            validate_expected_credential_version(3, credential(3))),
        "matching expected version accepted");
  check(
      rejected_with(validate_expected_credential_version(2, credential(3)), t::ErrorCode::Conflict),
      "stale expected version rejected");
  check(
      rejected_with(validate_expected_credential_version(1, std::nullopt), t::ErrorCode::Conflict),
      "expected version without a credential rejected");
  check(std::holds_alternative<std::monostate>(
            validate_expected_credential_version(1, credential(0))),
        "legacy version zero matches the frozen expected version");
}

void report_transitions() {
  using namespace t::runtime_resources_detail;
  const auto pending = delivery(t::BootstrapDeliveryState::Pending);
  const auto delivered = delivery(t::BootstrapDeliveryState::Delivered);
  const auto unknown = delivery(t::BootstrapDeliveryState::Unknown);
  auto revoked = delivery(t::BootstrapDeliveryState::Revoked);

  auto accepted = apply_bootstrap_delivery_report(
      pending, report(t::BootstrapDeliveryState::Delivered), 1, now());
  check(std::holds_alternative<t::BootstrapDeliveryRecord>(accepted),
        "pending delivery accepts the first delivered report");
  check(std::get<t::BootstrapDeliveryRecord>(accepted).state ==
            t::BootstrapDeliveryState::Delivered,
        "pending transitions to delivered");
  check(std::get<t::BootstrapDeliveryRecord>(accepted).receipt_id == "receipt-one",
        "delivered report persists the opaque receipt identity");

  auto pending_unknown = apply_bootstrap_delivery_report(
      pending, report(t::BootstrapDeliveryState::Unknown), 1, now());
  check(std::holds_alternative<t::BootstrapDeliveryRecord>(pending_unknown),
        "pending delivery accepts an unknown report");
  check(std::get<t::BootstrapDeliveryRecord>(pending_unknown).state ==
            t::BootstrapDeliveryState::Unknown,
        "pending transitions to unknown");
  check(std::get<t::BootstrapDeliveryRecord>(pending_unknown).receipt_id.empty(),
        "unknown report never fabricates a receipt");

  auto unknown_delivered = apply_bootstrap_delivery_report(
      unknown, report(t::BootstrapDeliveryState::Delivered), 1, now());
  check(std::holds_alternative<t::BootstrapDeliveryRecord>(unknown_delivered) &&
            std::get<t::BootstrapDeliveryRecord>(unknown_delivered).state ==
                t::BootstrapDeliveryState::Delivered,
        "a confirmed receipt converges an unknown delivery");

  auto repeated = apply_bootstrap_delivery_report(
      delivered, report(t::BootstrapDeliveryState::Delivered), 1, now());
  check(std::holds_alternative<t::BootstrapDeliveryRecord>(repeated),
        "repeated identical delivered report is idempotent");

  auto changed_receipt = report(t::BootstrapDeliveryState::Delivered);
  changed_receipt.receipt_id = "receipt-two";
  check(rejected_with(apply_bootstrap_delivery_report(delivered, changed_receipt, 1, now()),
                      t::ErrorCode::Conflict),
        "delivered report cannot switch receipt identity");

  check(rejected_with(apply_bootstrap_delivery_report(
                          delivered, report(t::BootstrapDeliveryState::Unknown), 1, now()),
                      t::ErrorCode::Conflict),
        "a confirmed delivery never downgrades to unknown");
  check(rejected_with(apply_bootstrap_delivery_report(
                          revoked, report(t::BootstrapDeliveryState::Delivered), 1, now()),
                      t::ErrorCode::Conflict),
        "revoked delivery never becomes delivered");

  check(rejected_with(apply_bootstrap_delivery_report(
                          pending, report(t::BootstrapDeliveryState::Pending), 1, now()),
                      t::ErrorCode::InvalidArgument),
        "a report cannot assert pending");
  check(rejected_with(apply_bootstrap_delivery_report(
                          pending, report(t::BootstrapDeliveryState::Revoked), 1, now()),
                      t::ErrorCode::InvalidArgument),
        "a report cannot revoke a delivery");

  auto no_receipt = report(t::BootstrapDeliveryState::Delivered);
  no_receipt.receipt_id.clear();
  check(rejected_with(apply_bootstrap_delivery_report(pending, no_receipt, 1, now()),
                      t::ErrorCode::InvalidArgument),
        "delivered report requires a receipt identity");
  no_receipt.receipt_id = "bad receipt";
  check(rejected_with(apply_bootstrap_delivery_report(pending, no_receipt, 1, now()),
                      t::ErrorCode::InvalidArgument),
        "receipt identity is a bounded opaque identifier");

  auto wrong_delivery = report(t::BootstrapDeliveryState::Delivered);
  wrong_delivery.delivery_id = "delivery-two";
  check(rejected_with(apply_bootstrap_delivery_report(pending, wrong_delivery, 1, now()),
                      t::ErrorCode::NotFound),
        "report for another delivery identity is not found");
  auto wrong_recipient = report(t::BootstrapDeliveryState::Delivered);
  wrong_recipient.recipient_id = "recipient-two";
  check(rejected_with(apply_bootstrap_delivery_report(pending, wrong_recipient, 1, now()),
                      t::ErrorCode::NotFound),
        "report for another recipient is not found");
  auto wrong_version = report(t::BootstrapDeliveryState::Delivered);
  wrong_version.credential_version = 2;
  check(rejected_with(apply_bootstrap_delivery_report(pending, wrong_version, 1, now()),
                      t::ErrorCode::Conflict),
        "report for a different credential version is a conflict");
  check(rejected_with(apply_bootstrap_delivery_report(
                          pending, report(t::BootstrapDeliveryState::Delivered), 2, now()),
                      t::ErrorCode::Conflict),
        "a superseded intent never becomes current");
  auto future = report(t::BootstrapDeliveryState::Delivered);
  future.observed_at = now() + 120s;
  check(rejected_with(apply_bootstrap_delivery_report(pending, future, 1, now()),
                      t::ErrorCode::InvalidArgument),
        "impossible future observation rejected");
  auto ancient = report(t::BootstrapDeliveryState::Delivered);
  ancient.observed_at = t::TimePoint{};
  check(rejected_with(apply_bootstrap_delivery_report(pending, ancient, 1, now()),
                      t::ErrorCode::InvalidArgument),
        "missing observation time rejected");
}

void permissions() {
  check(t::runtime_resources_detail::bootstrap_manage_permission(t::BootstrapPeerFamily::Adapter) ==
            t::Permission::AccountManage,
        "adapter bootstrap management uses account.manage");
  check(t::runtime_resources_detail::bootstrap_manage_permission(t::BootstrapPeerFamily::Plugin) ==
            t::Permission::PluginManage,
        "plugin bootstrap management uses plugin.manage");
}
} // namespace

int main() {
  identifiers();
  versions();
  report_transitions();
  permissions();
  return 0;
}
