#include <atomic>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>

#include "kiarabot/core/application/adapter_ingress.hpp"
#include "kiarabot/core/application/control_service.hpp"
#include "kiarabot/core/application/plugin_hub.hpp"
#include "kiarabot/core/application/runtime.h"
#include "kiarabot/core/runtime/bridge_server.hpp"
#include "kiarabot/core/runtime/in_memory_runtime_store.hpp"
#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"

namespace {
using kiarabot::core::AdapterIngress;
using kiarabot::core::BridgeRole;
using kiarabot::core::BridgeServer;
using kiarabot::core::DeliveryState;
using kiarabot::core::InMemoryRuntimeStore;
using kiarabot::core::PluginHub;
using kiarabot::core::PluginPolicy;
using kiarabot::core::PluginSession;
using kiarabot::core::RuntimeSnapshot;
using kiarabot::core::RuntimeTimePoint;
using kiarabot::infrastructure::SqliteRuntimeStore;
void require(bool value, const std::source_location& location = std::source_location::current()) {
  if (!value) {
    throw std::runtime_error("Plugin hub assertion line " + std::to_string(location.line()));
  }
}
template <typename T, typename E> T value(std::variant<T, E> outcome) {
  require(std::holds_alternative<T>(outcome));
  return std::get<T>(std::move(outcome));
}
kiarabot::v1::RegisterPluginRequest registration(const std::string& id) {
  kiarabot::v1::RegisterPluginRequest request;
  request.set_instance_id(id + "-instance");
  auto* manifest = request.mutable_manifest();
  manifest->set_id(id);
  manifest->set_name(id);
  manifest->set_version("1.0.0");
  manifest->set_api_version("kiarabot.plugin.v1");
  manifest->set_runtime("python-rpc");
  manifest->set_isolation("process");
  manifest->set_entrypoint("example:create");
  manifest->set_ordered(true);
  manifest->add_permissions("message.read");
  manifest->add_capabilities("event.message.received");
  return request;
}
kiarabot::v1::PluginEventAck ack(const std::string& id, bool accepted = true) {
  kiarabot::v1::PluginEventAck result;
  result.set_event_id(id);
  result.set_accepted(accepted);
  if (!accepted) {
    result.mutable_error()->set_code(kiarabot::v1::INVALID_ARGUMENT);
  }
  return result;
}
kiarabot::v1::EventEnvelope event(const std::string& id) {
  kiarabot::v1::EventEnvelope result;
  result.set_event_id(id);
  result.set_trace_id("trace");
  result.mutable_message_received()->set_message_id(id);
  return result;
}
void insert(kiarabot::core::RuntimeStore& store, const std::shared_ptr<PluginHub>& hub,
            const std::string& id, std::uint64_t sequence) {
  AdapterIngress ingress(store, {{"adapter", {"account"}}}, {}, hub);
  kiarabot::v1::RegisterAdapterRequest request;
  request.set_adapter_id("adapter");
  request.set_protocol_version("kiarabot.v1");
  request.set_implementation("fake");
  request.set_implementation_version("1.0");
  request.add_accounts()->set_account_id("account");
  request.mutable_accounts(0)->set_enabled(true);
  const auto registered = ingress.register_adapter("adapter", request);
  require(registered.response.accepted());
  auto payload = event(id);
  payload.set_schema_version("kiarabot.v1");
  payload.set_delivery_mode(kiarabot::v1::AT_LEAST_ONCE);
  payload.mutable_occurred_at()->set_seconds(1000);
  payload.set_sequence(sequence);
  payload.mutable_source()->set_adapter_id("adapter");
  payload.mutable_source()->set_account_id("account");
  payload.mutable_source()->set_adapter_instance_id(registered.session.instance);
  payload.mutable_subject()->set_conversation_id("session");
  payload.mutable_subject()->set_user_id("user");
  auto* message = payload.mutable_message_received();
  message->set_sender_id("user");
  message->mutable_conversation()->set_id("session");
  message->mutable_conversation()->set_account_id("account");
  message->mutable_conversation()->set_type(kiarabot::v1::ConversationRef::PRIVATE);
  require(ingress.ingest(registered.session, payload).status() ==
          kiarabot::v1::AdapterEventAck::PERSISTED);
}
void configuration_gate() {
  InMemoryRuntimeStore store;
  auto delivery =
      std::make_shared<kiarabot::core::PluginConfigDelivery>(store, std::string(64, 'a'));
  auto hub =
      std::make_shared<PluginHub>(store, std::vector<PluginPolicy>{{"one", {"message.read"}}});
  hub->set_configuration(delivery);
  auto request = registration("one");
  request.add_supported_features("plugin.config.v1");
  auto registered = hub->attach("one", request);
  require(registered.response.accepted());
  insert(store, hub, "waiting-for-config", 1);
  auto config = *value(hub->next_config(registered.session));
  require(!value(hub->next(registered.session)));
  kiarabot::v1::PluginConfigAck confirmation;
  confirmation.set_version(config.version());
  confirmation.set_delivery_id(config.delivery_id());
  confirmation.mutable_error()->set_code(kiarabot::v1::INVALID_ARGUMENT);
  value(hub->acknowledge_config(registered.session, confirmation));
  require(!value(hub->next(registered.session)));
  require(value(store.read()).deliveries.at("waiting-for-config").at("one").state !=
          DeliveryState::dispatched);
  value(hub->detach(registered.session));
  registered = hub->attach("one", request);
  require(registered.response.accepted());
  config = *value(hub->next_config(registered.session));
  confirmation.Clear();
  confirmation.set_version(config.version());
  confirmation.set_delivery_id(config.delivery_id());
  confirmation.set_accepted(true);
  value(hub->acknowledge_config(registered.session, confirmation));
  require(value(hub->next(registered.session))->event_id() == "waiting-for-config");
  value(hub->acknowledge(registered.session, ack("waiting-for-config")));
  value(hub->detach(registered.session));
}

void application() {
  InMemoryRuntimeStore store;
  auto now = RuntimeTimePoint{} + std::chrono::hours(1);
  auto hub = std::make_shared<PluginHub>(
      store, std::vector<PluginPolicy>{{"one", {"message.read"}}, {"two", {"message.read"}}},
      [&] { return now; });
  auto denied_request = registration("one");
  denied_request.mutable_manifest()->add_permissions("message.send");
  require(hub->attach("one", denied_request).response.error().code() ==
          kiarabot::v1::PERMISSION_DENIED);
  require(value(store.read()).plugins.empty());
  denied_request = registration("one");
  denied_request.mutable_manifest()->set_api_version("future");
  require(hub->attach("one", denied_request).response.error().code() ==
          kiarabot::v1::INVALID_ARGUMENT);
  const auto first = hub->attach("one", registration("one"));
  const auto second = hub->attach("two", registration("two"));
  require(first.response.accepted() && second.response.accepted());
  require(hub->attach("one", registration("one")).response.error().retryable());
  insert(store, hub, "event1", 1);
  insert(store, hub, "event2", 2);
  require(value(hub->next(first.session))->event_id() == "event1");
  require(value(hub->next(second.session))->event_id() == "event1");
  require(!value(hub->next(first.session)) && !value(hub->next(second.session)));
  store.set_fail_commits(true);
  require(std::get<kiarabot::v1::Error>(hub->acknowledge(first.session, ack("event1"))).code() ==
          kiarabot::v1::UPSTREAM_UNAVAILABLE);
  store.set_fail_commits(false);
  require(value(store.read()).deliveries.at("event1").at("one").state == DeliveryState::dispatched);
  value(hub->acknowledge(first.session, ack("event1")));
  const auto audit_count = value(store.read()).audit.size();
  value(hub->acknowledge(first.session, ack("event1")));
  require(value(store.read()).audit.size() == audit_count);
  require(value(hub->next(first.session))->event_id() == "event2");
  require(!value(hub->next(second.session)));
  value(hub->acknowledge(second.session, ack("event1", false)));
  require(value(hub->next(second.session))->event_id() == "event2");
  value(hub->acknowledge(first.session, ack("event2")));
  value(hub->acknowledge(second.session, ack("event2")));
  kiarabot::core::Runtime runtime(store, {}, [&] { return now; });
  require(value(runtime.replay_plugin_dead_letter("event1", "two", "replay-trace")));
  require(value(hub->next(second.session))->trace_id() == "replay-trace");
  require(!value(hub->next(first.session)));
  value(hub->acknowledge(second.session, ack("event1")));
  insert(store, hub, "event3", 3);
  require(value(hub->next(first.session))->event_id() == "event3");
  value(hub->detach(first.session));
  require(value(store.read()).deliveries.at("event3").at("one").state == DeliveryState::retrying);
  const auto replacement = hub->attach("one", registration("one"));
  require(replacement.response.accepted());
  require(std::get<kiarabot::v1::Error>(hub->acknowledge(first.session, ack("event3"))).code() ==
          kiarabot::v1::PERMISSION_DENIED);
  now += std::chrono::seconds(2);
  require(value(hub->next(replacement.session))->event_id() == "event3");
  // Simulate a process loss with a persisted in-flight delivery.
  hub.reset();
  auto recovered = std::make_shared<PluginHub>(
      store, std::vector<PluginPolicy>{{"one", {"message.read"}}}, [&] { return now; });
  const auto recovered_session = recovered->attach("one", registration("one"));
  require(recovered_session.response.accepted());
  now += std::chrono::seconds(5);
  require(value(recovered->next(recovered_session.session))->event_id() == "event3");
}

class Connection {
public:
  Connection(kiarabot::v1::PluginBridge::Stub& stub, const std::string& id,
             const std::string& token) {
    context_.AddMetadata("authorization", "Bearer " + token);
    context_.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    stream_ = stub.Connect(&context_);
    kiarabot::v1::PluginToCore request;
    *request.mutable_register_() = registration(id);
    require(stream_->Write(request));
    auto response = read();
    accepted = response.register_result().accepted();
  }
  ~Connection() {
    close();
  }
  kiarabot::v1::CoreToPlugin read() {
    kiarabot::v1::CoreToPlugin response;
    require(stream_->Read(&response));
    return response;
  }
  void confirm(const std::string& id) {
    kiarabot::v1::PluginToCore request;
    *request.mutable_event_ack() = ack(id);
    require(stream_->Write(request));
  }
  void close() {
    if (stream_) {
      context_.TryCancel();
      stream_->WritesDone();
      kiarabot::v1::CoreToPlugin ignored;
      while (stream_->Read(&ignored)) {
      }
      (void)stream_->Finish();
      stream_.reset();
    }
  }
  bool accepted{false};

private:
  grpc::ClientContext context_;
  std::unique_ptr<grpc::ClientReaderWriter<kiarabot::v1::PluginToCore, kiarabot::v1::CoreToPlugin>>
      stream_;
};

void transport() {
  auto store = value(SqliteRuntimeStore::open(":memory:"));
  std::atomic<int> seconds{3600};
  auto hub = std::make_shared<PluginHub>(
      *store, std::vector<PluginPolicy>{{"one", {"message.read"}}, {"two", {"message.read"}}},
      [&] { return RuntimeTimePoint{} + std::chrono::seconds(seconds.load()); });
  const std::string one_token(32, 'a'), two_token(32, 'b');
  BridgeServer server(
      {{BridgeRole::Plugin, "one", one_token}, {BridgeRole::Plugin, "two", two_token}}, {}, hub);
  require(server.start("127.0.0.1:0"));
  auto stub = kiarabot::v1::PluginBridge::NewStub(grpc::CreateChannel(
      "127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials()));
  Connection one(*stub, "one", one_token), two(*stub, "two", two_token);
  require(one.accepted && two.accepted);
  {
    Connection overlapping(*stub, "one", one_token);
    require(!overlapping.accepted);
  }
  insert(*store, hub, "network-event", 1);
  require(one.read().event().event_id() == "network-event");
  require(two.read().event().event_id() == "network-event");
  two.confirm("network-event");
  one.close();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto snapshot = value(store->read());
    if (snapshot.deliveries.at("network-event").at("one").state == DeliveryState::retrying &&
        snapshot.deliveries.at("network-event").at("two").state == DeliveryState::acked) {
      break;
    }
    std::this_thread::yield();
  }
  require(value(store->read()).deliveries.at("network-event").at("two").state ==
          DeliveryState::acked);
  seconds += 2;
  Connection replacement(*stub, "one", one_token);
  require(replacement.accepted);
  require(replacement.read().event().event_id() == "network-event");
  replacement.confirm("network-event");
  replacement.close();
  two.confirm("unknown-event");
  require(two.read().error().code() == kiarabot::v1::NOT_FOUND);
  two.close();
  // Local cancellation completes before the remote handler necessarily joins.
  const auto stopped_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (value(store->read()).plugins.at("two").state != kiarabot::core::PluginState::unavailable &&
         std::chrono::steady_clock::now() < stopped_by) {
    std::this_thread::yield();
  }
  require(value(store->read()).plugins.at("two").state == kiarabot::core::PluginState::unavailable);
  Connection shutdown(*stub, "two", two_token);
  require(shutdown.accepted);
  server.stop();
  shutdown.close();
}

void ack_timeout() {
  InMemoryRuntimeStore store;
  auto now = RuntimeTimePoint{} + std::chrono::hours(1);
  kiarabot::core::RuntimeOptions options;
  options.ack_timeout = std::chrono::seconds(2);
  options.max_attempts = 1;
  auto hub = std::make_shared<PluginHub>(
      store, std::vector<PluginPolicy>{{"one", {"message.read"}}}, [&] { return now; }, options);
  const auto attached = hub->attach("one", registration("one"));
  require(attached.response.accepted());
  insert(store, hub, "event1", 1);
  require(value(hub->next(attached.session))->event_id() == "event1");
  // The peer received the event but never acknowledged it: the delivery must not
  // block the plugin queue forever.
  now += std::chrono::seconds(3);
  require(!value(hub->next(attached.session)));
  require(value(store.read()).deliveries.at("event1").at("one").state == DeliveryState::retrying);
  require(value(store.read()).deliveries.at("event1").at("one").attempts == 1);
  now += std::chrono::seconds(2);
  require(value(hub->next(attached.session))->event_id() == "event1");
  now += std::chrono::seconds(3);
  require(!value(hub->next(attached.session)));
  require(value(store.read()).deliveries.at("event1").at("one").state ==
          DeliveryState::dead_letter);
  insert(store, hub, "event2", 2);
  require(value(hub->next(attached.session))->event_id() == "event2");
}

void conflict_retry() {
  InMemoryRuntimeStore store;
  auto now = RuntimeTimePoint{} + std::chrono::hours(1);
  auto hub = std::make_shared<PluginHub>(
      store, std::vector<PluginPolicy>{{"one", {"message.read"}}}, [&] { return now; });
  const std::string token(32, 'c');
  BridgeServer server({{BridgeRole::Plugin, "one", token}}, {}, hub);
  require(server.start("127.0.0.1:0"));
  auto stub = kiarabot::v1::PluginBridge::NewStub(grpc::CreateChannel(
      "127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials()));
  Connection one(*stub, "one", token);
  require(one.accepted);
  insert(store, hub, "event1", 1);
  require(one.read().event().event_id() == "event1");
  // A single optimistic-locking conflict must be retried inside the bridge; the
  // plugin stream stays open and the acknowledgement is still persisted.
  store.set_fail_conflicts(1);
  one.confirm("event1");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline &&
         value(store.read()).deliveries.at("event1").at("one").state != DeliveryState::acked) {
    std::this_thread::yield();
  }
  require(value(store.read()).deliveries.at("event1").at("one").state == DeliveryState::acked);
  insert(store, hub, "event2", 2);
  require(one.read().event().event_id() == "event2");
  one.confirm("event2");
  one.close();
  server.stop();
}

} // namespace
int main() try {
  configuration_gate();
  application();
  transport();
  ack_timeout();
  conflict_retry();
  std::cout << "Plugin authorization, subscriptions, independent ACK, replay and fencing passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
