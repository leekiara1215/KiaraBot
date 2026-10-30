#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <unistd.h>

#include "kiarabot/core/adapter_ingress.hpp"
#include "kiarabot/core/bridge_server.hpp"
#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"

namespace {
using kiarabot::core::AdapterIngress;
using kiarabot::core::AdapterPolicy;
using kiarabot::core::AdapterSession;
using kiarabot::core::BridgeCredential;
using kiarabot::core::BridgeRole;
using kiarabot::core::BridgeServer;
using kiarabot::core::InMemoryRuntimeStore;
using kiarabot::core::RuntimeOptions;
using kiarabot::core::RuntimeSnapshot;
using kiarabot::infrastructure::SqliteRuntimeStore;
using kiarabot::v1::AdapterEventAck;

void require(bool condition,
             const std::source_location& location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("assertion at line " + std::to_string(location.line()));
  }
}
kiarabot::v1::RegisterAdapterRequest registration() {
  kiarabot::v1::RegisterAdapterRequest request;
  request.set_adapter_id("adapter");
  request.set_implementation("fake");
  request.set_implementation_version("1.0");
  request.set_protocol_version("kiarabot.v1");
  auto* account = request.add_accounts();
  account->set_account_id("account");
  account->set_enabled(true);
  return request;
}
kiarabot::v1::EventEnvelope event(const AdapterSession& session, std::string id = "event",
                                  std::uint64_t sequence = 1) {
  kiarabot::v1::EventEnvelope result;
  result.set_event_id(std::move(id));
  result.set_trace_id("trace");
  result.set_schema_version("kiarabot.v1");
  result.mutable_occurred_at()->set_seconds(1000);
  result.set_delivery_mode(kiarabot::v1::AT_LEAST_ONCE);
  result.set_sequence(sequence);
  result.mutable_source()->set_adapter_id(session.identity);
  result.mutable_source()->set_adapter_instance_id(session.instance);
  result.mutable_source()->set_account_id("account");
  result.mutable_subject()->set_conversation_id("group");
  result.mutable_subject()->set_user_id("user");
  auto* payload = result.mutable_message_received();
  payload->set_message_id("message");
  payload->set_sender_id("user");
  payload->mutable_conversation()->set_id("group");
  payload->mutable_conversation()->set_account_id("account");
  payload->mutable_conversation()->set_type(kiarabot::v1::ConversationRef::GROUP);
  payload->mutable_message()->add_segments()->set_text("hello");
  payload->mutable_message()->mutable_segments(0)->set_type(kiarabot::v1::MessageSegment::TEXT);
  return result;
}
void rejected(const AdapterEventAck& result, kiarabot::v1::ErrorCode code, bool retryable = false) {
  require(result.status() == AdapterEventAck::REJECTED);
  require(result.error().code() == code && result.error().retryable() == retryable);
  require(result.retry_after_ms() == (retryable ? 1000 : 0));
}
void application_cases() {
  InMemoryRuntimeStore store;
  RuntimeOptions options;
  options.queue_limit = 1;
  AdapterIngress ingress(store, {{"adapter", {"account"}}}, options);
  auto request = registration();
  request.set_protocol_version("future");
  auto denied = ingress.register_adapter("adapter", request);
  require(!denied.response.accepted() &&
          denied.response.error().code() == kiarabot::v1::INVALID_ARGUMENT);
  request = registration();
  request.mutable_accounts(0)->set_account_id("other");
  require(ingress.register_adapter("adapter", request).response.error().code() ==
          kiarabot::v1::PERMISSION_DENIED);
  require(ingress.register_adapter("unconfigured", registration()).response.error().code() ==
          kiarabot::v1::PERMISSION_DENIED);
  auto registered = ingress.register_adapter("adapter", registration());
  require(registered.response.accepted() &&
          registered.response.assigned_instance_id().size() == 32);
  auto input = event(registered.session);
  auto invalid = input;
  invalid.mutable_source()->set_account_id("other");
  rejected(ingress.ingest(registered.session, invalid), kiarabot::v1::PERMISSION_DENIED);
  invalid = input;
  invalid.mutable_timer_triggered()->set_task_id("forged");
  rejected(ingress.ingest(registered.session, invalid), kiarabot::v1::PERMISSION_DENIED);
  invalid = input;
  invalid.mutable_message_received()->mutable_conversation()->set_id("different");
  rejected(ingress.ingest(registered.session, invalid), kiarabot::v1::INVALID_ARGUMENT);
  invalid = input;
  invalid.mutable_occurred_at()->set_nanos(1000000000);
  rejected(ingress.ingest(registered.session, invalid), kiarabot::v1::INVALID_ARGUMENT);
  store.set_fail_reads(true);
  rejected(ingress.ingest(registered.session, input), kiarabot::v1::UPSTREAM_UNAVAILABLE, true);
  store.set_fail_reads(false);
  store.set_fail_commits(true);
  rejected(ingress.ingest(registered.session, input), kiarabot::v1::UPSTREAM_UNAVAILABLE, true);
  store.set_fail_commits(false);
  auto snapshot = std::get<RuntimeSnapshot>(store.read());
  require(snapshot.events.empty() && snapshot.sessions.empty() && snapshot.audit.empty() &&
          snapshot.commands.empty());
  require(ingress.ingest(registered.session, input).status() == AdapterEventAck::PERSISTED);
  require(ingress.ingest(registered.session, input).status() == AdapterEventAck::DUPLICATE);
  snapshot = std::get<RuntimeSnapshot>(store.read());
  require(snapshot.events.size() == 1 && snapshot.audit.size() == 1 &&
          snapshot.sessions.size() == 1);
  invalid = input;
  invalid.mutable_message_received()->mutable_message()->set_plain_text("changed");
  rejected(ingress.ingest(registered.session, invalid), kiarabot::v1::CONFLICT);
  rejected(ingress.ingest(registered.session, event(registered.session, "event2", 2)),
           kiarabot::v1::RATE_LIMITED, true);
  auto new_registration = ingress.register_adapter("adapter", registration());
  require(!ingress.current(registered.session));
  rejected(ingress.ingest(registered.session, input), kiarabot::v1::CONFLICT);
  ingress.disconnect(registered.session); // Old disconnect cannot fence the new connection.
  require(ingress.current(new_registration.session));
  require(ingress.ingest(new_registration.session, event(new_registration.session)).status() ==
          AdapterEventAck::DUPLICATE);
  require(std::get<RuntimeSnapshot>(store.read()).events.size() == 1);
}

struct TempDirectory {
  std::filesystem::path path;
  TempDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() / "kiarabot-ingress-XXXXXX").string();
    const char* made = mkdtemp(pattern.data());
    if (!made) {
      throw std::runtime_error("temporary directory failed");
    }
    path = made;
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

void grpc_recovery() {
  TempDirectory directory;
  const auto path = (directory.path / "runtime.db").string();
  const std::string token(32, 'a');
  RuntimeOptions options;
  options.queue_limit = 1;
  for (int run = 0; run < 2; ++run) {
    auto opened = SqliteRuntimeStore::open(path);
    require(std::holds_alternative<std::unique_ptr<SqliteRuntimeStore>>(opened));
    auto store = std::get<std::unique_ptr<SqliteRuntimeStore>>(std::move(opened));
    auto ingress = std::make_shared<AdapterIngress>(
        *store, std::vector<AdapterPolicy>{{"adapter", {"account"}}}, options);
    BridgeServer server({{BridgeRole::Adapter, "adapter", token}}, ingress);
    require(server.start("127.0.0.1:0"));
    auto stub = kiarabot::v1::AdapterBridge::NewStub(grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials()));
    grpc::ClientContext context;
    context.AddMetadata("authorization", "Bearer " + token);
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    auto stream = stub->Connect(&context);
    kiarabot::v1::AdapterToCore request;
    *request.mutable_register_() = registration();
    require(stream->Write(request));
    kiarabot::v1::CoreToAdapter response;
    require(stream->Read(&response) && response.register_result().accepted());
    AdapterSession session{
        "adapter", response.register_result().assigned_instance_id(), {"account"}};
    request.Clear();
    request.mutable_heartbeat()->set_instance_id(session.instance);
    require(stream->Write(request) && stream->Read(&response) && response.has_heartbeat_ack());
    request.Clear();
    *request.mutable_event() = event(session);
    require(stream->Write(request));
    if (run == 0) {
      // Drop the connection after commit without consuming the acknowledgement.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      bool committed = false;
      while (std::chrono::steady_clock::now() < deadline) {
        if (std::get<RuntimeSnapshot>(store->read()).events.size() == 1) {
          committed = true;
          break;
        }
        std::this_thread::yield();
      }
      require(committed);
      context.TryCancel();
      stream->WritesDone();
      while (stream->Read(&response)) {
      }
      (void)stream->Finish();
    } else {
      require(stream->Read(&response) &&
              response.event_ack().status() == AdapterEventAck::DUPLICATE);
      require(std::get<RuntimeSnapshot>(store->read()).audit.size() == 1);
      *request.mutable_event() = event(session, "event2", 2);
      require(stream->Write(request) && stream->Read(&response));
      rejected(response.event_ack(), kiarabot::v1::RATE_LIMITED, true);
      stream->WritesDone();
      require(!stream->Read(&response) && stream->Finish().ok());
      require(std::get<RuntimeSnapshot>(store->read()).events.size() == 1);
    }
    server.stop();
  }
}
} // namespace
int main() try {
  application_cases();
  grpc_recovery();
  std::cout << "Adapter registration, durable ACK, rollback, replay and backpressure passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
