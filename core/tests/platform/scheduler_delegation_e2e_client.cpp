// P05b-5 real-service client: sealed task parent file -> Identity ExchangeDelegation
// (audience=scheduler, hops2) -> real Scheduler Create/Get/Cancel. The SDK PluginBridge ingress
// half is covered separately by p10_sdk_tasks_e2e's protocol fixture.
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/runtime/platform/instance_delegation_provider.hpp"
#include "kiarabot/core/runtime/platform/runtime_identity_client.hpp"
#include "kiarabot/core/runtime/platform/scheduler_task_client.hpp"
#include "kiarabot/core/runtime/platform/sealed_instance_delegation_source.hpp"

namespace {
namespace t = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
std::string read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream value;
  value << input.rdbuf();
  return value.str();
}
t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
std::string failure_text(const t::Failure& failure) {
  return std::to_string(static_cast<int>(failure.code));
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 9) {
    std::cerr << "usage: identity_endpoint scheduler_endpoint tls_dir sealed_file instance "
                 "installation task_id mode(create|completed)\n";
    return 2;
  }
  grpc::SslCredentialsOptions ssl;
  ssl.pem_root_certs = read_file(std::string{argv[3]} + "/ca.pem");
  ssl.pem_cert_chain = read_file(std::string{argv[3]} + "/runtime.pem");
  ssl.pem_private_key = read_file(std::string{argv[3]} + "/runtime.key");
  auto identity = platform::RuntimeIdentityClient::create({argv[1], grpc::SslCredentials(ssl),
                                                           std::chrono::milliseconds{5000},
                                                           std::chrono::milliseconds{10000}});
  if (const auto* failure = std::get_if<t::Failure>(&identity)) {
    std::cerr << "identity failure " << failure_text(*failure) << '\n';
    return 1;
  }
  auto& identity_client = std::get<std::unique_ptr<platform::RuntimeIdentityClient>>(identity);
  auto source = platform::SealedInstanceDelegationSource::create({argv[4]});
  if (const auto* failure = std::get_if<t::Failure>(&source)) {
    std::cerr << "source failure " << failure_text(*failure) << '\n';
    return 1;
  }
  std::shared_ptr<platform::InstanceParentDelegationSource> parent_source(
      std::move(std::get<std::unique_ptr<platform::SealedInstanceDelegationSource>>(source)));
  auto provider = platform::RuntimeInstanceDelegationProvider::create(
      {parent_source, ssl.pem_cert_chain, identity_client.get(), std::chrono::milliseconds{5000}});
  if (const auto* failure = std::get_if<t::Failure>(&provider)) {
    std::cerr << "provider failure " << failure_text(*failure) << '\n';
    return 1;
  }
  auto& delegation =
      std::get<std::unique_ptr<platform::RuntimeInstanceDelegationProvider>>(provider);
  auto scheduler = platform::SchedulerTaskClient::create(
      {argv[2], grpc::SslCredentials(ssl),
       [&delegation](const t::SchedulerTaskContext& context, t::Permission permission) {
         return delegation->scheduler_task_delegation(context, permission);
       },
       std::chrono::milliseconds{5000}});
  if (const auto* failure = std::get_if<t::Failure>(&scheduler)) {
    std::cerr << "scheduler failure " << failure_text(*failure) << '\n';
    return 1;
  }
  auto& client = std::get<std::unique_ptr<platform::SchedulerTaskClient>>(scheduler);
  t::SchedulerTaskContext context;
  context.owner = t::InstanceId{argv[5]};
  context.installation = t::InstallationId{argv[6]};
  context.request_id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
  context.trace_id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
  context.deadline = now() + std::chrono::seconds{5};
  context.idempotency_key = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
  t::TaskCreationInput input;
  input.task_id = argv[7];
  input.target = t::ConversationKey{{}, {}, "conversation-1", t::ConversationKey::Kind::Group};
  input.run_at = now() + std::chrono::seconds{std::string{argv[8]} == "completed" ? 1 : 60};
  const std::byte payload[] = {std::byte{0x7b}, std::byte{0x7d}};
  input.payload.assign(std::begin(payload), std::end(payload));
  auto created = client->create_task(context, input);
  if (const auto* failure = std::get_if<t::Failure>(&created)) {
    std::cerr << "create failure " << failure_text(*failure) << '\n';
    return 1;
  }
  const auto task = std::get<t::ScheduledTask>(created);
  if (task.key.owner.value != argv[5]) {
    std::cerr << "owner mismatch\n";
    return 1;
  }
  if (std::string{argv[8]} == "completed") {
    std::cout << "created owner=" << argv[5] << " state=" << static_cast<int>(task.state) << '\n';
    return 0;
  }
  if (std::string{argv[8]} == "idempotent") {
    auto replay = client->create_task(context, input);
    if (const auto* failure = std::get_if<t::Failure>(&replay)) {
      std::cerr << "replay failure " << failure_text(*failure) << '\n';
      return 1;
    }
    const auto& second = std::get<t::ScheduledTask>(replay);
    if (second.key.owner.value != task.key.owner.value || second.key.task_id != task.key.task_id ||
        second.version.value != task.version.value) {
      std::cerr << "idempotent replay mismatch\n";
      return 1;
    }
    std::cout << "idempotent owner=" << argv[5] << '\n';
    return 0;
  }
  const auto fetched = client->get_task(context, argv[7]);
  if (const auto* failure = std::get_if<t::Failure>(&fetched)) {
    std::cerr << "get failure " << failure_text(*failure) << '\n';
    return 1;
  }
  auto cancelled = client->cancel_task(context, argv[7]);
  if (const auto* failure = std::get_if<t::Failure>(&cancelled)) {
    std::cerr << "cancel failure " << failure_text(*failure) << '\n';
    return 1;
  }
  std::cout << "owner=" << argv[5] << " cancelled\n";
  return 0;
}
