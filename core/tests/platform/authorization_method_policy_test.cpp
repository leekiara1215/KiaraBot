// clang-format off
#include "kiarabot/core/runtime/platform/authorization_method_policy.hpp"

#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>

#include "kiarabot/v1/platform.pb.h"
#include "kiarabot/v1/policy.pb.h"
// clang-format on

namespace {
namespace v1 = kiarabot::v1;
namespace t = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace pb = google::protobuf;

pb::FileDescriptorProto copy_file(const pb::DescriptorPool& from, const std::string& name) {
  const auto* file = from.FindFileByName(name);
  assert(file != nullptr);
  pb::FileDescriptorProto proto;
  file->CopyTo(&proto);
  return proto;
}
pb::MethodDescriptorProto* add_method(pb::ServiceDescriptorProto* service,
                                      const std::string& name) {
  auto* method = service->add_method();
  method->set_name(name);
  method->set_input_type(".kiarabot.v1.RequestContext");
  method->set_output_type(".kiarabot.v1.RequestContext");
  return method;
}
void annotate(pb::MethodDescriptorProto* method, v1::Permission permission, bool online,
              std::uint32_t default_deadline, std::uint32_t max_deadline,
              std::initializer_list<v1::ResourceKind> kinds) {
  auto* options = method->mutable_options();
  options->SetExtension(v1::required_permission, permission);
  options->SetExtension(v1::requires_online_authorization, online);
  options->SetExtension(v1::default_deadline_ms, default_deadline);
  options->SetExtension(v1::max_deadline_ms, max_deadline);
  for (const auto kind : kinds) {
    options->AddExtension(v1::scope_kinds, kind);
  }
}
// Dynamic fixture-only descriptor file with the canonical extension declarations copied from the
// generated contract pool. No generated/pb2 payloads are redefined here.
pb::FileDescriptorProto test_file() {
  pb::FileDescriptorProto file;
  file.set_name("kiarabot/v1/policy_resolver_fixture.proto");
  file.set_package("kiarabot.v1");
  file.add_dependency("google/protobuf/descriptor.proto");
  file.add_dependency("kiarabot/v1/common.proto");
  file.add_dependency("kiarabot/v1/platform.proto");
  file.add_dependency("kiarabot/v1/policy.proto");
  auto* service = file.add_service();
  service->set_name("ModelService");
  auto* good = add_method(service, "GoodChat");
  annotate(good, v1::PERMISSION_MODEL_CHAT, true, 1000, 2000, {v1::RESOURCE_MODEL});
  auto* unannotated = add_method(service, "Unannotated");
  static_cast<void>(unannotated);
  auto* no_scope = add_method(service, "NoScope");
  no_scope->mutable_options()->SetExtension(v1::required_permission, v1::PERMISSION_MODEL_CHAT);
  no_scope->mutable_options()->SetExtension(v1::requires_online_authorization, true);
  no_scope->mutable_options()->SetExtension(v1::default_deadline_ms, 1000);
  no_scope->mutable_options()->SetExtension(v1::max_deadline_ms, 2000);
  auto* bad_online = add_method(service, "BadOnline");
  annotate(bad_online, v1::PERMISSION_MODEL_CHAT, false, 1000, 2000, {v1::RESOURCE_MODEL});
  auto* bad_deadline = add_method(service, "BadDeadline");
  annotate(bad_deadline, v1::PERMISSION_MODEL_CHAT, true, 0, 0, {v1::RESOURCE_MODEL});
  auto* bad_kind = add_method(service, "BadKind");
  annotate(bad_kind, v1::PERMISSION_MODEL_CHAT, true, 1000, 2000, {v1::RESOURCE_KIND_UNSPECIFIED});
  auto* bad_permission = add_method(service, "BadPermission");
  annotate(bad_permission, v1::PERMISSION_UNSPECIFIED, true, 1000, 2000, {v1::RESOURCE_MODEL});
  return file;
}
std::unique_ptr<pb::DescriptorPool> fixture_pool() {
  auto pool = std::make_unique<pb::DescriptorPool>();
  const auto& generated = *pb::DescriptorPool::generated_pool();
  for (const auto& name : {"google/protobuf/descriptor.proto", "google/protobuf/timestamp.proto",
                           "google/protobuf/struct.proto", "kiarabot/v1/common.proto",
                           "kiarabot/v1/platform.proto", "kiarabot/v1/policy.proto"}) {
    assert(pool->BuildFile(copy_file(generated, name)) != nullptr);
  }
  assert(pool->BuildFile(test_file()) != nullptr);
  return pool;
}
std::optional<t::AuthorizationMethodPolicy> resolve_port(t::AuthorizationMethodPolicyPort& port,
                                                         const std::string& name) {
  auto result = port.resolve(name);
  assert(std::holds_alternative<std::optional<t::AuthorizationMethodPolicy>>(result));
  return std::get<std::optional<t::AuthorizationMethodPolicy>>(result);
}
void generated_contract_metadata() {
  auto created = platform::make_descriptor_authorization_method_policy();
  assert(std::holds_alternative<std::unique_ptr<t::AuthorizationMethodPolicyPort>>(created));
  auto resolver = std::move(std::get<std::unique_ptr<t::AuthorizationMethodPolicyPort>>(created));
  const auto chat = resolve_port(*resolver, "/kiarabot.v1.ModelService/Chat");
  assert(chat.has_value());
  assert(chat->owner == t::Service::Model);
  assert(chat->permission == t::Permission::ModelChat);
  assert(chat->scope_kinds.size() == 1 && chat->scope_kinds.front() == t::ResourceKind::Model);
  // Unannotated method, unknown method, unknown service and a bridge stream fail closed as empty.
  assert(!resolve_port(*resolver, "/kiarabot.v1.ModelService/GetCallStatus").has_value());
  assert(!resolve_port(*resolver, "/kiarabot.v1.ModelService/NotAMethod").has_value());
  assert(!resolve_port(*resolver, "/kiarabot.v1.UnknownService/Method").has_value());
  assert(!resolve_port(*resolver, "/kiarabot.v1.HostBridge/Connect").has_value());
  assert(!resolve_port(*resolver, "Chat").has_value());
  assert(!resolve_port(*resolver, "").has_value());
}
void unannotated_and_unknown_are_empty() {
  auto pool = fixture_pool();
  auto created = platform::make_descriptor_authorization_method_policy(pool.get());
  assert(std::holds_alternative<std::unique_ptr<t::AuthorizationMethodPolicyPort>>(created));
  auto resolver = std::move(std::get<std::unique_ptr<t::AuthorizationMethodPolicyPort>>(created));
  assert(!resolve_port(*resolver, "/kiarabot.v1.ModelService/Unannotated").has_value());
  assert(!resolve_port(*resolver, "/kiarabot.v1.ModelService/NoMethod").has_value());
  const auto good = resolve_port(*resolver, "/kiarabot.v1.ModelService/GoodChat");
  assert(good.has_value());
  assert(good->owner == t::Service::Model);
  assert(good->permission == t::Permission::ModelChat);
  assert(good->scope_kinds.size() == 1 && good->scope_kinds.front() == t::ResourceKind::Model);
}
void malformed_metadata_is_a_failure() {
  auto pool = fixture_pool();
  auto created = platform::make_descriptor_authorization_method_policy(pool.get());
  assert(std::holds_alternative<std::unique_ptr<t::AuthorizationMethodPolicyPort>>(created));
  auto resolver = std::move(std::get<std::unique_ptr<t::AuthorizationMethodPolicyPort>>(created));
  const auto expect_failure = [&](const std::string& name) {
    const auto result = resolver->resolve(name);
    assert(std::holds_alternative<t::Failure>(result));
    assert(std::get<t::Failure>(result).code == t::ErrorCode::Internal);
  };
  expect_failure("/kiarabot.v1.ModelService/BadOnline");
  expect_failure("/kiarabot.v1.ModelService/BadDeadline");
  expect_failure("/kiarabot.v1.ModelService/BadKind");
  expect_failure("/kiarabot.v1.ModelService/BadPermission");
}
void missing_contract_extensions_fail_startup() {
  // An empty descriptor pool has no canonical extension declarations: construction must return a
  // stable dependency failure, never a resolver that silently accepts or denies everything.
  pb::DescriptorPool empty;
  const auto created = platform::make_descriptor_authorization_method_policy(&empty);
  assert(std::holds_alternative<t::Failure>(created));
  assert(std::get<t::Failure>(created).code == t::ErrorCode::UpstreamUnavailable);
}
} // namespace

int main() {
  generated_contract_metadata();
  unannotated_and_unknown_are_empty();
  malformed_metadata_is_a_failure();
  missing_contract_extensions_fail_startup();
  return 0;
}
