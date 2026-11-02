// clang-format off
#include "kiarabot/core/runtime/platform/authorization_method_policy.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>

#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/host.pb.h"
#include "kiarabot/v1/identity.pb.h"
#include "kiarabot/v1/model.pb.h"
#include "kiarabot/v1/runtime.pb.h"
#include "kiarabot/v1/scheduler.pb.h"
// clang-format on

namespace kiarabot::core::platform {
namespace {
namespace t = target;
namespace codec = target_codec;
namespace pb = google::protobuf;
namespace v1 = kiarabot::v1;

constexpr std::string_view kRequiredPermission = "kiarabot.v1.required_permission";
constexpr std::string_view kScopeKinds = "kiarabot.v1.scope_kinds";
constexpr std::string_view kOnline = "kiarabot.v1.requires_online_authorization";
constexpr std::string_view kDefaultDeadline = "kiarabot.v1.default_deadline_ms";
constexpr std::string_view kMaxDeadline = "kiarabot.v1.max_deadline_ms";
constexpr std::size_t kMaximumScopeKinds = 16;

t::Failure invalid_metadata() {
  return {t::ErrorCode::Internal, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
t::Failure unavailable() {
  return {
      t::ErrorCode::UpstreamUnavailable, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
// The generated contract descriptors must be registered in THIS binary before resolution; a
// static archive only pulls descriptor objects that some translation unit actually references.
// Never synthesize method metadata from a handwritten table when registration is missing.
void ensure_contract_descriptors() {
  static const int once = [] {
    static_cast<void>(v1::IdentityLoginRequest::descriptor());
    static_cast<void>(v1::RuntimeCreateAccountRequest::descriptor());
    static_cast<void>(v1::HostRegisterHostRequest::descriptor());
    static_cast<void>(v1::SchedulerCreateTaskRequest::descriptor());
    static_cast<void>(v1::ModelGetModelMetadataRequest::descriptor());
    static_cast<void>(v1::Permission_descriptor());
    static_cast<void>(v1::ResourceKind_descriptor());
    return 0;
  }();
  static_cast<void>(once);
}
std::optional<t::Service> service_owner(std::string_view name) {
  if (name == "IdentityService") {
    return t::Service::Identity;
  }
  if (name == "RuntimeService") {
    return t::Service::Runtime;
  }
  if (name == "HostService") {
    return t::Service::Host;
  }
  if (name == "SchedulerService") {
    return t::Service::Scheduler;
  }
  if (name == "ModelService") {
    return t::Service::Model;
  }
  return std::nullopt;
}
// Canonical gRPC name shape only: /package.Service/Method (the leading slash is optional here for
// fixtures). Anything else is an unknown method, never guessed.
bool split_method(std::string_view full_method, std::string* service, std::string* method) {
  if (!full_method.empty() && full_method.front() == '/') {
    full_method.remove_prefix(1);
  }
  const auto separator = full_method.rfind('/');
  if (separator == std::string_view::npos || separator == 0 ||
      separator + 1 >= full_method.size()) {
    return false;
  }
  *service = std::string{full_method.substr(0, separator)};
  *method = std::string{full_method.substr(separator + 1)};
  return true;
}
class Resolver final : public t::AuthorizationMethodPolicyPort {
public:
  explicit Resolver(const pb::DescriptorPool& pool) : pool_(&pool) {}
  bool valid() const {
    // The supplied pool must carry the canonical contract file list; otherwise annotations cannot
    // be interpreted and resolution would silently fail closed forever.
    return pool_ && pool_->FindFileByName("kiarabot/v1/policy.proto") &&
           pool_->FindFileByName("kiarabot/v1/platform.proto");
  }
  t::Outcome<std::optional<t::AuthorizationMethodPolicy>>
  resolve(std::string_view full_method) override {
    try {
      return resolve_impl(full_method);
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return invalid_metadata();
    }
  }

private:
  t::Outcome<std::optional<t::AuthorizationMethodPolicy>>
  resolve_impl(std::string_view full_method) {
    std::string service_name;
    std::string method_name;
    if (!split_method(full_method, &service_name, &method_name)) {
      return std::optional<t::AuthorizationMethodPolicy>{};
    }
    const auto* service = pool_->FindServiceByName(service_name);
    if (!service) {
      return std::optional<t::AuthorizationMethodPolicy>{};
    }
    const auto owner = service_owner(service->name());
    if (!owner) {
      return std::optional<t::AuthorizationMethodPolicy>{};
    }
    const auto* method = service->FindMethodByName(method_name);
    if (!method) {
      return std::optional<t::AuthorizationMethodPolicy>{};
    }
    const auto& options = method->options();
    // Unannotated methods are simply unsupported, never defaulted to a permission or scope.
    if (!options.HasExtension(v1::required_permission) ||
        options.ExtensionSize(v1::scope_kinds) == 0) {
      return std::optional<t::AuthorizationMethodPolicy>{};
    }
    if (options.GetExtension(v1::required_permission) == v1::PERMISSION_UNSPECIFIED) {
      return invalid_metadata();
    }
    if (!options.HasExtension(v1::requires_online_authorization) ||
        !options.GetExtension(v1::requires_online_authorization)) {
      return invalid_metadata();
    }
    if (static_cast<std::size_t>(options.ExtensionSize(v1::scope_kinds)) > kMaximumScopeKinds) {
      return invalid_metadata();
    }
    const auto default_deadline = options.HasExtension(v1::default_deadline_ms)
                                      ? options.GetExtension(v1::default_deadline_ms)
                                      : 0;
    const auto max_deadline =
        options.HasExtension(v1::max_deadline_ms) ? options.GetExtension(v1::max_deadline_ms) : 0;
    if (default_deadline == 0 || max_deadline < default_deadline) {
      return invalid_metadata();
    }
    const auto permission = codec::from_wire(options.GetExtension(v1::required_permission));
    if (std::holds_alternative<t::Failure>(permission)) {
      return invalid_metadata();
    }
    std::vector<t::ResourceKind> scope_kinds;
    for (int index = 0; index < options.ExtensionSize(v1::scope_kinds); ++index) {
      const auto decoded = codec::from_wire(options.GetExtension(v1::scope_kinds, index));
      if (std::holds_alternative<t::Failure>(decoded)) {
        return invalid_metadata();
      }
      const auto kind = std::get<t::ResourceKind>(decoded);
      if (std::find(scope_kinds.begin(), scope_kinds.end(), kind) == scope_kinds.end()) {
        scope_kinds.push_back(kind);
      }
    }
    if (scope_kinds.empty()) {
      return invalid_metadata();
    }
    return std::optional<t::AuthorizationMethodPolicy>{t::AuthorizationMethodPolicy{
        *owner, std::get<t::Permission>(permission), std::move(scope_kinds)}};
  }

  const pb::DescriptorPool* pool_ = nullptr;
};
} // namespace

t::Outcome<std::unique_ptr<t::AuthorizationMethodPolicyPort>>
make_descriptor_authorization_method_policy(const pb::DescriptorPool* descriptor_pool) {
  try {
    ensure_contract_descriptors();
    const auto* pool = descriptor_pool ? descriptor_pool : pb::DescriptorPool::generated_pool();
    if (!pool) {
      return unavailable();
    }
    auto resolver = std::make_unique<Resolver>(*pool);
    if (!resolver->valid()) {
      return unavailable();
    }
    return std::unique_ptr<t::AuthorizationMethodPolicyPort>{std::move(resolver)};
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return invalid_metadata();
  }
}
} // namespace kiarabot::core::platform
