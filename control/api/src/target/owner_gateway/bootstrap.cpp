#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <google/protobuf/util/time_util.h>
#include <sys/stat.h>

#include "internal.hpp"
#include "kiarabot/control/application/target/secret_delivery.hpp"
#include "kiarabot/control/target/secret_file_delivery.hpp"

namespace kiarabot::control::target::owner_gateway {
namespace {

constexpr std::string_view kAccountPrefix = "/api/v1/accounts/";
constexpr std::string_view kInstancePrefix = "/api/v1/instances/";
constexpr std::string_view kDeliveries = "/bootstrap-deliveries/";
constexpr std::string_view kReissue = "/bootstrap/reissue";

// Wire resource kinds are offset by one (UNSPECIFIED=0). The delivery port only knows the two
// bootstrap families, so anything else is an invalid request, never an implicit instance target.
std::optional<core::target::ResourceRef> delivery_resource(const wire::ResourceRef& resource) {
  if (resource.id().empty()) {
    return std::nullopt;
  }
  if (resource.kind() == wire::RESOURCE_ACCOUNT) {
    return core::target::ResourceRef{core::target::ResourceKind::Account, {resource.id()}};
  }
  if (resource.kind() == wire::RESOURCE_INSTANCE) {
    return core::target::ResourceRef{core::target::ResourceKind::Instance, {resource.id()}};
  }
  return std::nullopt;
}

// The Control gateway never owns delivery state: it forwards a no-secret report to Runtime after
// the sink stored the fixed-recipient file. A report failure leaves the Runtime intent PENDING
// (explicit reissue/recovery), never a locally invented DELIVERED.
std::optional<wire::RuntimeGetBootstrapDeliveryResponse>
report_delivery(const RequestContext& context, const Ports& ports, std::string_view delegation,
                bool adapter, const std::string& resource_id,
                const wire::BootstrapDeliveryReport& report) {
  if (ports.resources_ == nullptr) {
    return std::nullopt;
  }
  if (adapter) {
    wire::RuntimeReportAdapterBootstrapDeliveryRequest input;
    input.set_account_id(resource_id);
    *input.mutable_report() = report;
    if (!fill_context(input.mutable_context(), context.request_id_, context.trace_id_,
                      context.request_)) {
      return std::nullopt;
    }
    auto result = ports.resources_->report_adapter_bootstrap_delivery(input, delegation);
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      (void)failure;
      return std::nullopt;
    }
    return std::get<wire::RuntimeGetBootstrapDeliveryResponse>(result);
  }
  wire::RuntimeReportPluginBootstrapDeliveryRequest input;
  input.set_instance_id(resource_id);
  *input.mutable_report() = report;
  if (!fill_context(input.mutable_context(), context.request_id_, context.trace_id_,
                    context.request_)) {
    return std::nullopt;
  }
  auto result = ports.resources_->report_plugin_bootstrap_delivery(input, delegation);
  if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
    (void)failure;
    return std::nullopt;
  }
  return std::get<wire::RuntimeGetBootstrapDeliveryResponse>(result);
}

} // namespace

// Applies the Control delivery sink exactly once to a first-issue/reissue owner response, reports
// the no-secret evidence to Runtime and strips the internal plaintext from the REST body. The
// plaintext lives only in this stack frame; it is never logged, audited, receipted or persisted by
// Control. A failed store/report keeps the Runtime intent authoritative (PENDING/UNKNOWN) and the
// ordinary response never contains the secret.

namespace {

core::target::Failure invalid_registry() {
  return core::target::Failure{core::target::ErrorCode::InvalidArgument,
                               core::target::Acceptance::NotAccepted,
                               core::target::Recovery::CorrectInput,
                               {},
                               {}};
}

class FixedRecipients final : public BootstrapRecipientsPort {
public:
  explicit FixedRecipients(std::vector<SecretFileRecipient> recipients)
      : recipients_(std::move(recipients)) {}

  bool known(std::string_view recipient_id, const wire::ResourceRef& resource) const override {
    for (const auto& recipient : recipients_) {
      if (recipient.recipient_id != recipient_id) {
        continue;
      }
      if (resource.kind() == wire::RESOURCE_ACCOUNT) {
        if (recipient.resource.kind != core::target::ResourceKind::Account) {
          return false;
        }
      } else if (resource.kind() == wire::RESOURCE_INSTANCE) {
        if (recipient.resource.kind != core::target::ResourceKind::Instance) {
          return false;
        }
      } else {
        return false;
      }
      return recipient.resource.id.value.empty() || recipient.resource.id.value == resource.id();
    }
    return false;
  }

private:
  std::vector<SecretFileRecipient> recipients_;
};

bool bounded_identifier(std::string_view value) {
  if (value.empty() || value.size() > 128) {
    return false;
  }
  for (const unsigned char character : value) {
    const bool accepted = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') || character == '-' ||
                          character == '_' || character == '.';
    if (!accepted) {
      return false;
    }
  }
  return value != "." && value != "..";
}

std::optional<std::string> read_registry(const std::string& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    return std::nullopt;
  }
  struct stat information {};
  if (::fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode) ||
      information.st_uid != ::geteuid() || (information.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      information.st_size < 1 || information.st_size > 65536) {
    (void)::close(descriptor);
    return std::nullopt;
  }
  std::string content;
  std::array<char, 4096> buffer{};
  while (true) {
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      (void)::close(descriptor);
      return std::nullopt;
    }
    if (count == 0) {
      break;
    }
    content.append(buffer.data(), static_cast<std::size_t>(count));
    if (content.size() > 65536) {
      (void)::close(descriptor);
      return std::nullopt;
    }
  }
  (void)::close(descriptor);
  return content;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> owner_identity(const Json& value) {
  if (!value.is_string()) {
    return std::nullopt;
  }
  const auto& text = value.get_ref<const std::string&>();
  const auto separator = text.find(':');
  if (separator == std::string::npos || separator == 0 || separator + 1 == text.size()) {
    return std::nullopt;
  }
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  const auto& user = text.substr(0, separator);
  const auto& group = text.substr(separator + 1);
  const auto parsed_user = std::from_chars(user.data(), user.data() + user.size(), uid);
  const auto parsed_group = std::from_chars(group.data(), group.data() + group.size(), gid);
  if (parsed_user.ec != std::errc{} || parsed_user.ptr != user.data() + user.size() ||
      parsed_group.ec != std::errc{} || parsed_group.ptr != group.data() + group.size()) {
    return std::nullopt;
  }
  return std::make_pair(uid, gid);
}

} // namespace

core::target::Outcome<ControlBootstrapDelivery>
make_control_bootstrap_delivery_impl(const std::string& registry_path) {
  try {
    auto content = read_registry(registry_path);
    if (!content) {
      return core::target::Failure{core::target::ErrorCode::InvalidArgument,
                                   core::target::Acceptance::NotAccepted,
                                   core::target::Recovery::CorrectInput,
                                   {},
                                   {}};
    }
    const auto document = Json::parse(*content, nullptr, false);
    if (document.is_discarded() || !document.is_object() || document.value("schema", 0) != 1 ||
        !document.contains("recipients") || !document["recipients"].is_array() ||
        document["recipients"].empty() || document["recipients"].size() > 127) {
      return core::target::Failure{core::target::ErrorCode::InvalidArgument,
                                   core::target::Acceptance::NotAccepted,
                                   core::target::Recovery::CorrectInput,
                                   {},
                                   {}};
    }
    std::vector<SecretFileRecipient> recipients;
    for (const auto& item : document["recipients"]) {
      if (!item.is_object() || !item.contains("recipient_id") ||
          !item["recipient_id"].is_string() || !item.contains("resource_kind") ||
          !item["resource_kind"].is_string() || !item.contains("directory") ||
          !item["directory"].is_string() || !item.contains("owner")) {
        return core::target::Failure{core::target::ErrorCode::InvalidArgument,
                                     core::target::Acceptance::NotAccepted,
                                     core::target::Recovery::CorrectInput,
                                     {},
                                     {}};
      }
      const auto recipient_id = item["recipient_id"].get<std::string>();
      const auto directory = item["directory"].get<std::string>();
      const auto kind = item["resource_kind"].get<std::string>();
      auto owner = owner_identity(item["owner"]);
      std::string resource_id;
      if (item.contains("resource_id") && !item["resource_id"].is_null()) {
        if (!item["resource_id"].is_string()) {
          return invalid_registry();
        }
        resource_id = item["resource_id"].get<std::string>();
      }
      std::string peer_id;
      if (item.contains("peer_id") && !item["peer_id"].is_null()) {
        if (!item["peer_id"].is_string()) {
          return invalid_registry();
        }
        peer_id = item["peer_id"].get<std::string>();
      }
      const bool kind_level = resource_id.empty();
      if (!bounded_identifier(recipient_id) || directory.empty() || directory.front() != '/' ||
          !owner) {
        return core::target::Failure{core::target::ErrorCode::InvalidArgument,
                                     core::target::Acceptance::NotAccepted,
                                     core::target::Recovery::CorrectInput,
                                     {},
                                     {}};
      }
      core::target::ResourceKind resource_kind = core::target::ResourceKind::Account;
      if (kind == "instance") {
        resource_kind = core::target::ResourceKind::Instance;
      } else if (kind != "account") {
        return core::target::Failure{core::target::ErrorCode::InvalidArgument,
                                     core::target::Acceptance::NotAccepted,
                                     core::target::Recovery::CorrectInput,
                                     {},
                                     {}};
      }
      if (kind_level) {
        if (!peer_id.empty()) {
          return invalid_registry();
        }
      } else if (!bounded_identifier(resource_id) || !bounded_identifier(peer_id)) {
        return invalid_registry();
      }
      recipients.push_back(SecretFileRecipient{recipient_id,
                                               {resource_kind, {resource_id}},
                                               peer_id,
                                               directory,
                                               owner->first,
                                               owner->second});
    }
    auto recipient_identities = recipients;
    auto port = make_secret_file_delivery(std::move(recipients));
    if (const auto* failure = std::get_if<core::target::Failure>(&port)) {
      return *failure;
    }
    ControlBootstrapDelivery composition;
    composition.recipients = std::make_unique<FixedRecipients>(std::move(recipient_identities));
    composition.port = std::move(std::get<std::unique_ptr<SecretDeliveryPort>>(port));
    if (!composition.port) {
      return core::target::Failure{core::target::ErrorCode::Internal,
                                   core::target::Acceptance::NotAccepted,
                                   core::target::Recovery::Diagnose,
                                   {},
                                   {}};
    }
    composition.application = std::make_unique<SecretDeliveryApplication>(*composition.port);
    return composition;
  } catch (...) {
    return core::target::Failure{core::target::ErrorCode::Internal,
                                 core::target::Acceptance::NotAccepted,
                                 core::target::Recovery::Diagnose,
                                 {},
                                 {}};
  }
}

OwnerHttpResult deliver_bootstrap_response(const RequestContext& context, const Ports& ports,
                                           std::string_view delegation, bool adapter,
                                           Json& response_value) {
  std::string plaintext;
  if (response_value.contains("bootstrap_credential")) {
    const auto& credential = response_value["bootstrap_credential"];
    if (!credential.is_string()) {
      return context.error(wire::INTERNAL);
    }
    plaintext = credential.get<std::string>();
    response_value.erase("bootstrap_credential");
  }
  if (!response_value.contains("delivery") || !response_value["delivery"].is_object()) {
    // Omitted-recipient legacy path: no delivery intent and no deployment side effect.
    return OwnerHttpResult{200, response_value.dump()};
  }
  wire::BootstrapDelivery issued;
  if (!json_request(response_value["delivery"].dump(), issued) || !issued.has_resource()) {
    return context.error(wire::INTERNAL);
  }
  const auto resource = delivery_resource(issued.resource());
  if (!resource || issued.delivery_id().empty() || issued.recipient_id().empty() ||
      issued.credential_version() == 0) {
    return context.error(wire::INTERNAL);
  }
  if (ports.secret_delivery_ == nullptr || ports.bootstrap_recipients_ == nullptr) {
    // A delivery intent exists but this deployment has no controlled sink: fail closed rather than
    // pretending the secret was installed.
    return context.error(wire::UPSTREAM_UNAVAILABLE, true);
  }
  if (!ports.bootstrap_recipients_->known(issued.recipient_id(), issued.resource())) {
    response_value.erase("delivery");
    return context.error(wire::INVALID_ARGUMENT, false);
  }
  if (plaintext.empty()) {
    // Replay/rotation path without a new plaintext: runtime metadata stays authoritative.
    return OwnerHttpResult{200, response_value.dump()};
  }
  // A plugin report needs the exact instance management scope; the create call was authorized on
  // the parent installation, so obtain the report delegation from the same caller token now.
  std::string report_delegation{delegation};
  if (!adapter) {
    auto issued = context.delegate(ports.delegation_, "runtime", wire::PERMISSION_PLUGIN_MANAGE,
                                   instance_scope(resource->id.value));
    if (const auto* failure = std::get_if<OwnerFailure>(&issued)) {
      return context.error(failure->code, failure->retryable);
    }
    report_delegation = std::get<std::string>(issued);
  }
  const SecretDeliveryIdentity identity{issued.delivery_id(), *resource, issued.recipient_id(),
                                        issued.credential_version()};
  const SecretDeliveryRequest request{identity, issued.credential_version() - 1, plaintext};
  wire::BootstrapDeliveryState reported_state = wire::BOOTSTRAP_DELIVERY_UNKNOWN;
  std::string receipt_id;
  auto stored = ports.secret_delivery_->store(request);
  if (const auto* receipt = std::get_if<SecretDeliveryReceipt>(&stored)) {
    reported_state = receipt->state == SecretDeliveryState::Delivered
                         ? wire::BOOTSTRAP_DELIVERY_DELIVERED
                         : wire::BOOTSTRAP_DELIVERY_UNKNOWN;
    receipt_id = receipt->receipt_id;
  }
  wire::BootstrapDeliveryReport report;
  report.set_delivery_id(identity.delivery_id);
  report.set_recipient_id(identity.recipient_id);
  report.set_credential_version(identity.credential_version);
  report.set_state(reported_state);
  if (!receipt_id.empty()) {
    report.set_receipt_id(receipt_id);
  }
  *report.mutable_observed_at() = google::protobuf::util::TimeUtil::GetCurrentTime();
  auto reported =
      report_delivery(context, ports, report_delegation, adapter, resource->id.value, report);
  if (reported && reported->has_delivery()) {
    response_value["delivery"] = json_request_value(json_response(reported->delivery()));
  }
  return OwnerHttpResult{200, response_value.dump()};
}

std::optional<OwnerHttpResult> handle_bootstrap(const RequestContext& context, const Ports& ports) {
  const auto& path = context.path_;
  const auto method =
      context.request_.method == "HEAD" ? std::string{"GET"} : context.request_.method;
  const auto split_delivery =
      [&](std::string_view prefix) -> std::optional<std::pair<std::string, std::string>> {
    if (!path.starts_with(prefix)) {
      return std::nullopt;
    }
    const auto rest = path.substr(prefix.size());
    const auto marker = rest.find(kDeliveries);
    if (marker == std::string_view::npos || marker == 0) {
      return std::nullopt;
    }
    const auto resource = std::string{rest.substr(0, marker)};
    const auto delivery = std::string{rest.substr(marker + kDeliveries.size())};
    if (resource.empty() || resource.find('/') != std::string::npos || delivery.empty() ||
        delivery.find('/') != std::string::npos) {
      return std::nullopt;
    }
    return std::make_pair(resource, delivery);
  };
  const auto split_reissue = [&](std::string_view prefix) -> std::optional<std::string> {
    if (!path.starts_with(prefix) || !path.ends_with(kReissue)) {
      return std::nullopt;
    }
    const auto resource =
        std::string{path.substr(prefix.size(), path.size() - prefix.size() - kReissue.size())};
    if (resource.empty() || resource.find('/') != std::string::npos) {
      return std::nullopt;
    }
    return resource;
  };

  if (const auto account = split_delivery(kAccountPrefix)) {
    if (method != "GET") {
      return context.method_not_allowed();
    }
    if (ports.resources_ == nullptr) {
      return context.error(wire::UPSTREAM_UNAVAILABLE, true);
    }
    auto delegation = context.delegate(ports.delegation_, "runtime", wire::PERMISSION_RESOURCE_READ,
                                       account_scope(account->first));
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.error(failure->code, failure->retryable);
    }
    wire::RuntimeGetBootstrapDeliveryRequest input;
    input.mutable_resource()->set_kind(wire::RESOURCE_ACCOUNT);
    input.mutable_resource()->set_id(account->first);
    input.set_delivery_id(account->second);
    if (!fill_context(input.mutable_context(), context.request_id_, context.trace_id_,
                      context.request_)) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    auto result =
        ports.resources_->get_bootstrap_delivery(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.error(failure->code, failure->retryable);
    }
    return OwnerHttpResult{
        200, json_response(std::get<wire::RuntimeGetBootstrapDeliveryResponse>(result))};
  }
  if (const auto instance = split_delivery(kInstancePrefix)) {
    if (method != "GET") {
      return context.method_not_allowed();
    }
    if (ports.resources_ == nullptr) {
      return context.error(wire::UPSTREAM_UNAVAILABLE, true);
    }
    auto delegation = context.delegate(ports.delegation_, "runtime", wire::PERMISSION_RESOURCE_READ,
                                       instance_scope(instance->first));
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.error(failure->code, failure->retryable);
    }
    wire::RuntimeGetBootstrapDeliveryRequest input;
    input.mutable_resource()->set_kind(wire::RESOURCE_INSTANCE);
    input.mutable_resource()->set_id(instance->first);
    input.set_delivery_id(instance->second);
    if (!fill_context(input.mutable_context(), context.request_id_, context.trace_id_,
                      context.request_)) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    auto result =
        ports.resources_->get_bootstrap_delivery(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.error(failure->code, failure->retryable);
    }
    return OwnerHttpResult{
        200, json_response(std::get<wire::RuntimeGetBootstrapDeliveryResponse>(result))};
  }
  if (const auto account = split_reissue(kAccountPrefix)) {
    if (method != "POST") {
      return context.method_not_allowed();
    }
    if (ports.resources_ == nullptr) {
      return context.error(wire::UPSTREAM_UNAVAILABLE, true);
    }
    auto parsed = Json::parse(context.request_.body, nullptr, false);
    wire::RuntimeReissueAdapterBootstrapRequest request;
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("recipient_id") ||
        !parsed["recipient_id"].is_string() || !parsed.contains("expected_credential_version") ||
        !parsed["expected_credential_version"].is_string()) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    request.set_account_id(*account);
    request.set_recipient_id(parsed["recipient_id"].get<std::string>());
    std::uint64_t expected = 0;
    const auto& text = parsed["expected_credential_version"].get_ref<const std::string&>();
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), expected);
    if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() ||
        expected == 0) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    request.set_expected_credential_version(expected);
    if (!fill_context(request.mutable_context(), context.request_id_, context.trace_id_,
                      context.request_)) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    auto delegation = context.delegate(ports.delegation_, "runtime",
                                       wire::PERMISSION_ACCOUNT_MANAGE, account_scope(*account));
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.error(failure->code, failure->retryable);
    }
    auto result =
        ports.resources_->reissue_adapter_bootstrap(request, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.error(failure->code, failure->retryable);
    }
    auto value = json_response(std::get<wire::RuntimeReissueBootstrapResponse>(result));
    auto response = json_request_value(value);
    return deliver_bootstrap_response(context, ports, std::get<std::string>(delegation), true,
                                      response);
  }
  if (const auto instance = split_reissue(kInstancePrefix)) {
    if (method != "POST") {
      return context.method_not_allowed();
    }
    if (ports.resources_ == nullptr) {
      return context.error(wire::UPSTREAM_UNAVAILABLE, true);
    }
    auto parsed = Json::parse(context.request_.body, nullptr, false);
    wire::RuntimeReissuePluginBootstrapRequest request;
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("recipient_id") ||
        !parsed["recipient_id"].is_string() || !parsed.contains("expected_credential_version") ||
        !parsed["expected_credential_version"].is_string()) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    request.set_instance_id(*instance);
    request.set_recipient_id(parsed["recipient_id"].get<std::string>());
    std::uint64_t expected = 0;
    const auto& text = parsed["expected_credential_version"].get_ref<const std::string&>();
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), expected);
    if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() ||
        expected == 0) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    request.set_expected_credential_version(expected);
    if (!fill_context(request.mutable_context(), context.request_id_, context.trace_id_,
                      context.request_)) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    auto delegation = context.delegate(ports.delegation_, "runtime", wire::PERMISSION_PLUGIN_MANAGE,
                                       instance_scope(*instance));
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.error(failure->code, failure->retryable);
    }
    auto result =
        ports.resources_->reissue_plugin_bootstrap(request, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.error(failure->code, failure->retryable);
    }
    auto value = json_response(std::get<wire::RuntimeReissueBootstrapResponse>(result));
    auto response = json_request_value(value);
    return deliver_bootstrap_response(context, ports, std::get<std::string>(delegation), false,
                                      response);
  }
  return std::nullopt;
}

} // namespace kiarabot::control::target::owner_gateway

namespace kiarabot::control::target {

// Public composition entry declared in owner_gateway.hpp; the implementation lives with the
// owner-gateway registry parsing it already uses.
core::target::Outcome<ControlBootstrapDelivery>
make_control_bootstrap_delivery(const std::string& registry_path) {
  return owner_gateway::make_control_bootstrap_delivery_impl(registry_path);
}

} // namespace kiarabot::control::target
