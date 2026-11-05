// P12f fake Runtime/Scheduler owner services for the real Control acceptance matrix.
// Both services listen on one mutually-authenticated TLS endpoint; responses are typed, canonical
// and only cover the paths Control forwards. Marker ids containing "conflict"/"unavailable" force
// stable owner failures so the Control error mapping can be asserted without mutating fixtures.
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <google/protobuf/util/json_util.h>
#include <grpcpp/grpcpp.h>

#include "kiarabot/v1/host.grpc.pb.h"
#include "kiarabot/v1/runtime.grpc.pb.h"
#include "kiarabot/v1/scheduler.grpc.pb.h"

namespace {
namespace wire = kiarabot::v1;
constexpr std::int64_t kNow = 1700000000;
std::string read(const char* path) {
  std::ifstream input(path);
  std::ostringstream result;
  result << input.rdbuf();
  return result.str();
}
grpc::Status failure(wire::ErrorCode code, grpc::StatusCode transport,
                     const wire::RequestContext& context) {
  wire::RpcFailure rpc;
  rpc.mutable_context()->set_acceptance(wire::NOT_ACCEPTED);
  rpc.mutable_context()->set_recovery(wire::DO_NOT_RETRY);
  rpc.mutable_error()->set_code(code);
  rpc.mutable_error()->set_request_id(context.request_id());
  rpc.mutable_error()->set_trace_id(context.trace_id());
  rpc.mutable_error()->set_message("fixture-owner-error");
  return {transport, "fixture-owner-error", rpc.SerializeAsString()};
}
bool marked(const std::string& value, std::string_view marker) {
  return value.find(marker) != std::string::npos;
}
grpc::Status marker_status(const std::string& value, const wire::RequestContext& context,
                           bool* handled) {
  *handled = false;
  if (marked(value, "conflict")) {
    *handled = true;
    return failure(wire::CONFLICT, grpc::StatusCode::ABORTED, context);
  }
  if (marked(value, "unavailable")) {
    *handled = true;
    return failure(wire::UPSTREAM_UNAVAILABLE, grpc::StatusCode::UNAVAILABLE, context);
  }
  return grpc::Status::OK;
}
void operation(wire::OperationRef* output, std::string_view id, std::string_view owner,
               const wire::ResourceRef& resource) {
  output->set_owner_service(std::string{owner});
  output->set_operation_id(std::string{id});
  *output->mutable_resource() = resource;
}
// WS-04 target stream fixtures. Scope resource ids select a deterministic journal script:
// ...0001 live (one change per poll, then idle), ...0002 flood (32 wide changes per poll, bounded
// at 5000) and ...0003 gap (typed CONFLICT+RESYNC after sequence 2). Other ids behave as an
// empty, confirmed head so unrelated fixtures keep working.
constexpr char kStreamLiveAccount[] = "aaaaaaaa-aaaa-4aaa-8aaa-000000000001";
constexpr char kStreamFloodAccount[] = "aaaaaaaa-aaaa-4aaa-8aaa-000000000002";
constexpr char kStreamGapAccount[] = "aaaaaaaa-aaaa-4aaa-8aaa-000000000003";
constexpr char kStreamEpoch[] = "fixture-stream-epoch-1";
constexpr std::uint64_t kStreamMaximum = 1000000;
void stamp_now(google::protobuf::Timestamp* output) {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
  output->set_seconds(seconds.count());
  output->set_nanos(0);
}
bool same_scope(const wire::ResourceScope& left, const wire::ResourceScope& right) {
  if (left.resources_size() != right.resources_size()) {
    return false;
  }
  for (int index = 0; index < left.resources_size(); ++index) {
    if (left.resources(index).kind() != right.resources(index).kind() ||
        left.resources(index).id() != right.resources(index).id()) {
      return false;
    }
  }
  return true;
}
grpc::Status stream_resync(const wire::RequestContext& context) {
  wire::RpcFailure rpc;
  rpc.mutable_context()->set_acceptance(wire::NOT_ACCEPTED);
  rpc.mutable_context()->set_recovery(wire::RESYNC_SNAPSHOT);
  rpc.mutable_error()->set_code(wire::CONFLICT);
  rpc.mutable_error()->set_request_id(context.request_id());
  rpc.mutable_error()->set_trace_id(context.trace_id());
  rpc.mutable_error()->set_message("fixture-stream-cursor-gap");
  return {grpc::StatusCode::ABORTED, "fixture-stream-cursor-gap", rpc.SerializeAsString()};
}
void stream_cursor(wire::StreamCursor* output, const wire::ResourceScope& scope,
                   std::uint64_t sequence) {
  output->set_source_service("runtime");
  output->set_epoch(kStreamEpoch);
  *output->mutable_scope() = scope;
  output->set_sequence(sequence);
}
void stream_change(wire::OperationChange* output, const wire::ResourceScope& scope,
                   std::uint64_t sequence, bool wide) {
  auto* operation = output->mutable_operation();
  operation->mutable_ref()->set_owner_service("runtime");
  operation->mutable_ref()->set_operation_id("operation-" + std::to_string(sequence));
  *operation->mutable_ref()->mutable_resource() = scope.resources(0);
  operation->set_version(sequence + 1);
  operation->set_state(wire::OPERATION_RUNNING);
  operation->set_execution_outcome(wire::EXECUTION_UNKNOWN);
  stamp_now(operation->mutable_observed_at());
  if (wide) {
    for (int index = 0; index < 16; ++index) {
      auto* participant = operation->add_participants();
      participant->set_participant("participant-" + std::to_string(index) + "-" +
                                   std::string(80, 'p'));
      participant->set_state(wire::OPERATION_RUNNING);
    }
  }
  stream_cursor(output->mutable_cursor(), scope, sequence);
}
void stream_page(const wire::OperationChangesRequest& request,
                 wire::OperationChangesResponse* output) {
  const auto& scope = request.scope();
  const std::string mode = scope.resources(0).id();
  const std::uint64_t sequence =
      request.has_cursor() ? request.cursor().sequence() : std::uint64_t{0};
  std::uint64_t next = request.has_cursor() ? sequence : 0;
  if (request.has_cursor() && sequence < kStreamMaximum) {
    if (mode == kStreamFloodAccount) {
      const auto count = std::min<std::uint64_t>(32, kStreamMaximum - sequence);
      for (std::uint64_t offset = 1; offset <= count; ++offset) {
        stream_change(output->add_changes(), scope, sequence + offset, true);
      }
      next = sequence + count;
      output->set_has_more(true);
    } else if ((mode == kStreamLiveAccount || mode == kStreamGapAccount) && sequence < 5) {
      stream_change(output->add_changes(), scope, sequence + 1, false);
      next = sequence + 1;
    }
  }
  stream_cursor(output->mutable_next_cursor(), scope, next);
  stamp_now(output->mutable_checked_at());
}
// Deterministic full-directory scan fixtures: two immutable-snapshot pages per catalog plus error
// modes for the fail-closed assertions (authorization denial on page 2 / snapshot change mid-scan).
constexpr char kAdapterCatalog[] = "c1111111-1111-4111-8111-111111111111";
constexpr char kAdapterFailCatalog[] = "d1111111-1111-4111-8111-111111111111";
constexpr char kAdapterMismatchCatalog[] = "e1111111-1111-4111-8111-111111111111";
constexpr char kInstanceCatalog[] = "f1111111-1111-4111-8111-111111111111";
constexpr char kInstanceFailCatalog[] = "a2111111-1111-4111-8111-111111111111";
constexpr char kInstanceMismatchCatalog[] = "b2111111-1111-4111-8111-111111111111";
// Transient owner unavailability: the first kTransientOwnerCalls page calls fail with
// DEADLINE_EXCEEDED, then the same immutable-page fixture succeeds.
constexpr char kAdapterTransientCatalog[] = "c8111111-1111-4111-8111-111111111111";
constexpr char kInstanceTransientCatalog[] = "c7111111-1111-4111-8111-111111111111";
constexpr int kTransientOwnerCalls = 3;

class FakeRuntime final : public wire::RuntimeService::Service {
public:
  std::atomic<int> logs_feed_state_{0};
  std::atomic<int> messages_feed_state_{0};
  std::atomic<int> changing_adapter_calls_{0};
  std::atomic<int> error_adapter_calls_{0};
  std::atomic<int> changing_instance_calls_{0};
  std::atomic<int> error_instance_calls_{0};
  std::atomic<int> transient_adapter_calls_{0};
  std::atomic<int> transient_instance_calls_{0};
  grpc::Status ListOperationChanges(grpc::ServerContext*,
                                    const wire::OperationChangesRequest* input,
                                    wire::OperationChangesResponse* output) override {
    if (!input->has_context() || !input->context().has_deadline() || !input->has_scope() ||
        input->scope().resources_size() != 1 || input->page_size() > 100 ||
        input->context().request_id().empty() || input->context().trace_id().empty()) {
      return failure(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    const std::string mode = input->scope().resources(0).id();
    if (mode == kStreamGapAccount && input->has_cursor() && input->cursor().sequence() >= 2) {
      return stream_resync(input->context());
    }
    if (input->has_cursor() &&
        (input->cursor().source_service() != "runtime" || input->cursor().epoch() != kStreamEpoch ||
         !same_scope(input->cursor().scope(), input->scope()))) {
      return failure(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    stream_page(*input, output);
    return grpc::Status::OK;
  }
  // v1.2 Runtime feeds: deterministic non-empty first page, cursor continuation, typed RESYNC for
  // a wrong-dataset cursor. No log payload/message body is ever fabricated.
  grpc::Status ListLogChanges(grpc::ServerContext*, const wire::ObservationChangesRequest* input,
                              wire::LogChangesResponse* output) override {
    if (!input->has_context() || !input->has_scope() || input->scope().resources_size() != 1 ||
        input->scope().resources(0).kind() != wire::RESOURCE_CATALOG ||
        input->scope().resources(0).id() != "control" || input->page_size() > 100) {
      return failure(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    const auto dataset = [&](wire::StreamCursor* cursor, std::uint64_t sequence) {
      cursor->set_source_service("runtime");
      cursor->set_epoch("2d000000-0000-4000-8000-000000000001");
      *cursor->mutable_scope() = input->scope();
      cursor->set_sequence(sequence);
    };
    if (input->has_cursor() && (input->cursor().source_service() != "runtime" ||
                                input->cursor().epoch() != "2d000000-0000-4000-8000-000000000001" ||
                                !same_scope(input->cursor().scope(), input->scope()))) {
      return stream_resync(input->context());
    }
    std::uint64_t next = 1;
    if (!input->has_cursor()) {
      logs_feed_state_.store(1);
      next = 0;
    } else if (input->cursor().sequence() == 0 && logs_feed_state_.load() == 1) {
      auto* change = output->add_changes();
      dataset(change->mutable_cursor(), 1);
      auto* log = change->mutable_log();
      log->set_owner_service("runtime");
      log->set_entry_id("runtime-log-1");
      log->set_severity("INFO");
      log->set_component("runtime");
      log->set_method("kiarabot.v1.RuntimeService/ListAccounts");
      log->set_event("request.recorded");
      log->mutable_resource()->set_kind(wire::RESOURCE_CATALOG);
      log->mutable_resource()->set_id("control");
      log->set_request_id("00000000-0000-4000-8000-000000000101");
      log->set_trace_id("10000000-0000-4000-8000-000000000101");
      log->mutable_occurred_at()->set_seconds(kNow);
      logs_feed_state_.store(2);
      next = 1;
    } else {
      next = input->cursor().sequence();
    }
    dataset(output->mutable_next_cursor(), next);
    output->set_has_more(false);
    stamp_now(output->mutable_checked_at());
    return grpc::Status::OK;
  }

  grpc::Status ListMessageMetadataChanges(grpc::ServerContext*,
                                          const wire::ObservationChangesRequest* input,
                                          wire::MessageMetadataChangesResponse* output) override {
    if (!input->has_context() || !input->has_scope() || input->scope().resources_size() != 1 ||
        input->scope().resources(0).kind() != wire::RESOURCE_ACCOUNT || input->page_size() > 100) {
      return failure(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    const auto dataset = [&](wire::StreamCursor* cursor, std::uint64_t sequence) {
      cursor->set_source_service("runtime");
      cursor->set_epoch("2d000000-0000-4000-8000-000000000002");
      *cursor->mutable_scope() = input->scope();
      cursor->set_sequence(sequence);
    };
    if (input->has_cursor() && (input->cursor().source_service() != "runtime" ||
                                input->cursor().epoch() != "2d000000-0000-4000-8000-000000000002" ||
                                !same_scope(input->cursor().scope(), input->scope()))) {
      return stream_resync(input->context());
    }
    std::uint64_t next = 1;
    if (!input->has_cursor()) {
      messages_feed_state_.store(1);
      next = 0;
    } else if (input->cursor().sequence() == 0 && messages_feed_state_.load() == 1) {
      auto* change = output->add_changes();
      dataset(change->mutable_cursor(), 1);
      auto* message = change->mutable_message();
      message->set_owner_service("runtime");
      message->set_message_id("33333333-3333-4333-8333-000000000001");
      message->set_conversation_id("44444444-4444-4444-8444-000000000001");
      message->set_account_id(input->scope().resources(0).id());
      message->mutable_received_at()->set_seconds(kNow);
      messages_feed_state_.store(2);
      next = 1;
    } else {
      next = input->cursor().sequence();
    }
    dataset(output->mutable_next_cursor(), next);
    output->set_has_more(false);
    stamp_now(output->mutable_checked_at());
    return grpc::Status::OK;
  }

  // Snapshot channels: exact-resource owner queries with deterministic values. Constant ids keep
  // the same value (no repeat event); changing ids advance version; error ids fail once before a
  // real value so the producer must stay unknown/stale rather than fabricate.
  grpc::Status GetAdapter(grpc::ServerContext*, const wire::RuntimeGetAdapterRequest* input,
                          wire::RuntimeGetAdapterResponse* output) override {
    if (!input->has_resource() || input->resource().kind() != wire::RESOURCE_ADAPTER ||
        input->resource().id().empty()) {
      return failure(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    const std::string id = input->resource().id();
    if (id.ends_with("0d01")) {
      // Deterministic slow owner page: SESSION_READY must not wait for this first owner poll.
      std::this_thread::sleep_for(std::chrono::milliseconds{800});
    }
    if (id.ends_with("0e01") && error_adapter_calls_.fetch_add(1) == 0) {
      return failure(wire::UPSTREAM_UNAVAILABLE, grpc::StatusCode::UNAVAILABLE, input->context());
    }
    std::uint64_t version = 1;
    if (id.ends_with("0a02")) {
      version = static_cast<std::uint64_t>(1 + changing_adapter_calls_.fetch_add(1));
    }
    auto* record = output->mutable_adapter();
    record->mutable_resource()->set_kind(wire::RESOURCE_ADAPTER);
    record->mutable_resource()->set_id(id);
    record->set_version(version);
    record->set_online(true);
    record->set_lifecycle(wire::LIFECYCLE_ACTIVE);
    record->mutable_observed_at()->set_seconds(kNow + static_cast<std::int64_t>(version));
    record->mutable_last_registered_at()->set_seconds(kNow);
    auto* fence = record->mutable_current_fence();
    fence->set_instance_id("66666666-6666-4666-8666-000000000001");
    fence->set_connection_generation(1);
    fence->set_runtime_epoch("fixture-runtime-epoch-1");
    return grpc::Status::OK;
  }
  grpc::Status GetInstance(grpc::ServerContext*, const wire::RuntimeGetInstanceRequest* input,
                           wire::RuntimeGetInstanceResponse* output) override {
    if (!input->has_resource() || input->resource().kind() != wire::RESOURCE_INSTANCE ||
        input->resource().id().empty()) {
      return failure(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    const std::string id = input->resource().id();
    if (id.ends_with("0f01") && error_instance_calls_.fetch_add(1) == 0) {
      return failure(wire::UPSTREAM_UNAVAILABLE, grpc::StatusCode::UNAVAILABLE, input->context());
    }
    std::uint64_t version = 1;
    if (id.ends_with("0b02")) {
      version = static_cast<std::uint64_t>(1 + changing_instance_calls_.fetch_add(1));
    }
    auto* record = output->mutable_instance();
    record->set_instance_id(id);
    record->set_installation_id("99999999-9999-4999-8999-000000000001");
    record->set_host_id("88888888-8888-4888-8888-000000000001");
    record->set_externally_managed(false);
    auto* state = record->mutable_state();
    state->mutable_resource()->set_kind(wire::RESOURCE_INSTANCE);
    state->mutable_resource()->set_id(id);
    state->set_version(version);
    state->set_lifecycle(wire::LIFECYCLE_ACTIVE);
    state->set_desired(wire::DESIRED_RUNNING);
    state->set_registered(true);
    state->set_desired_config_version(1);
    state->set_applied_config_version(0);
    state->set_process_outcome(wire::EXECUTION_UNKNOWN);
    state->set_source_service("runtime");
    state->mutable_observed_at()->set_seconds(kNow + static_cast<std::int64_t>(version));
    auto* instance_fence = state->mutable_fence();
    instance_fence->set_instance_id(id);
    instance_fence->set_execution_generation(1);
    instance_fence->set_connection_generation(1);
    instance_fence->set_runtime_epoch("fixture-runtime-epoch-1");
    return grpc::Status::OK;
  }
  grpc::Status CreateAccount(grpc::ServerContext*, const wire::RuntimeCreateAccountRequest* input,
                             wire::RuntimeCreateAccountResponse* output) override {
    if (input->catalog().kind() != wire::RESOURCE_CATALOG ||
        input->catalog().id() != "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa" ||
        input->context().idempotency_key() != "matrix-account-create-0001") {
      return failure(wire::PERMISSION_DENIED, grpc::StatusCode::PERMISSION_DENIED,
                     input->context());
    }
    output->mutable_account()->set_account_id("55555555-5555-4555-8555-555555555555");
    output->mutable_account()->set_platform(input->platform());
    return grpc::Status::OK;
  }
  template <typename Input, typename Response, typename Append>
  grpc::Status catalog_page(const Input* input, Response* output, Append append) {
    if (input->page().page_size() == 0) {
      return failure(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    const std::string prefix = "fixture-catalog:";
    wire::ResourceScope scope;
    if (!input->context().delegation().starts_with(prefix) ||
        !google::protobuf::util::JsonStringToMessage(
             input->context().delegation().substr(prefix.size()), &scope)
             .ok()) {
      return failure(wire::PERMISSION_DENIED, grpc::StatusCode::PERMISSION_DENIED,
                     input->context());
    }
    if (input->page().page_token() == "bad") {
      return failure(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    if (input->page().page_token() == "down") {
      return failure(wire::UPSTREAM_UNAVAILABLE, grpc::StatusCode::UNAVAILABLE, input->context());
    }
    for (int index = 1; index < scope.resources_size(); ++index) {
      append(scope.resources(index).id());
    }
    auto* snapshot = output->mutable_snapshot();
    snapshot->set_snapshot_id("snapshot-fixture");
    snapshot->set_authorization_version(1);
    snapshot->set_owner_service("runtime");
    snapshot->mutable_observed_at()->set_seconds(kNow);
    snapshot->mutable_expires_at()->set_seconds(kNow + 600);
    snapshot->mutable_page()->set_page_size(
        input->page().page_size() == 0 ? 20 : input->page().page_size());
    return grpc::Status::OK;
  }
  grpc::Status ListAccounts(grpc::ServerContext*, const wire::RuntimeListAccountsRequest* input,
                            wire::RuntimeListAccountsResponse* output) override {
    return catalog_page(input, output,
                        [&](const std::string& id) { output->add_accounts()->set_account_id(id); });
  }
  grpc::Status ListInstallations(grpc::ServerContext*,
                                 const wire::RuntimeListInstallationsRequest* input,
                                 wire::RuntimeListInstallationsResponse* output) override {
    return catalog_page(input, output, [&](const std::string& id) {
      output->add_installations()->set_installation_id(id);
    });
  }
  // Full-directory immutable-page fixture for the WS catalog scan. Non-WS catalogs keep the
  // historical fixture-catalog behavior used by the REST catalog acceptance.
  grpc::Status ListInstances(grpc::ServerContext*, const wire::RuntimeListInstancesRequest* input,
                             wire::RuntimeListInstancesResponse* output) override {
    if (input->has_catalog() && input->catalog().kind() == wire::RESOURCE_CATALOG &&
        input->catalog().id() == kInstanceTransientCatalog &&
        transient_instance_calls_.fetch_add(1) < kTransientOwnerCalls) {
      return failure(wire::TIMEOUT, grpc::StatusCode::DEADLINE_EXCEEDED, input->context());
    }
    if (input->has_catalog() && input->catalog().kind() == wire::RESOURCE_CATALOG &&
        (input->catalog().id() == kInstanceCatalog ||
         input->catalog().id() == kInstanceFailCatalog ||
         input->catalog().id() == kInstanceMismatchCatalog ||
         input->catalog().id() == kInstanceTransientCatalog)) {
      return instance_catalog_page(input, output);
    }
    return catalog_page(input, output, [&](const std::string& id) {
      output->add_instances()->set_instance_id(id);
    });
  }
  grpc::Status ListAdapters(grpc::ServerContext*, const wire::RuntimeListAdaptersRequest* input,
                            wire::RuntimeListAdaptersResponse* output) override {
    if (input->has_catalog() && input->catalog().kind() == wire::RESOURCE_CATALOG &&
        input->catalog().id() == kAdapterTransientCatalog &&
        transient_adapter_calls_.fetch_add(1) < kTransientOwnerCalls) {
      return failure(wire::TIMEOUT, grpc::StatusCode::DEADLINE_EXCEEDED, input->context());
    }
    if (input->has_catalog() && input->catalog().kind() == wire::RESOURCE_CATALOG &&
        (input->catalog().id() == kAdapterCatalog || input->catalog().id() == kAdapterFailCatalog ||
         input->catalog().id() == kAdapterMismatchCatalog ||
         input->catalog().id() == kAdapterTransientCatalog)) {
      return adapter_catalog_page(input, output);
    }
    return catalog_page(input, output, [&](const std::string& id) {
      output->add_adapters()->mutable_resource()->set_id(id);
    });
  }
  void adapter_catalog_record(wire::AdapterRecord* record, const std::string& id,
                              std::uint64_t version) {
    record->mutable_resource()->set_kind(wire::RESOURCE_ADAPTER);
    record->mutable_resource()->set_id(id);
    record->set_version(version);
    record->set_online(true);
    record->set_lifecycle(wire::LIFECYCLE_ACTIVE);
    record->mutable_observed_at()->set_seconds(kNow + static_cast<std::int64_t>(version));
    auto* fence = record->mutable_current_fence();
    fence->set_instance_id("66666666-6666-4666-8666-000000000001");
    fence->set_connection_generation(1);
    fence->set_runtime_epoch("fixture-runtime-epoch-1");
  }
  void instance_catalog_record(wire::InstanceRecord* record, const std::string& id,
                               std::uint64_t version) {
    record->set_instance_id(id);
    record->set_installation_id("99999999-9999-4999-8999-000000000001");
    record->set_host_id("88888888-8888-4888-8888-000000000001");
    record->set_externally_managed(false);
    auto* state = record->mutable_state();
    state->mutable_resource()->set_kind(wire::RESOURCE_INSTANCE);
    state->mutable_resource()->set_id(id);
    state->set_version(version);
    state->set_lifecycle(wire::LIFECYCLE_ACTIVE);
    state->set_desired(wire::DESIRED_RUNNING);
    state->set_registered(true);
    state->set_desired_config_version(1);
    state->set_applied_config_version(0);
    state->set_process_outcome(wire::EXECUTION_UNKNOWN);
    state->set_source_service("runtime");
    state->mutable_observed_at()->set_seconds(kNow + static_cast<std::int64_t>(version));
    auto* instance_fence = state->mutable_fence();
    instance_fence->set_instance_id(id);
    instance_fence->set_execution_generation(1);
    instance_fence->set_connection_generation(1);
    instance_fence->set_runtime_epoch("fixture-runtime-epoch-1");
  }
  grpc::Status adapter_catalog_page(const wire::RuntimeListAdaptersRequest* input,
                                    wire::RuntimeListAdaptersResponse* output) {
    const std::string page_token = input->page().page_token();
    auto* snapshot = output->mutable_snapshot();
    snapshot->set_owner_service("runtime");
    snapshot->set_authorization_version(1);
    stamp_now(snapshot->mutable_observed_at());
    stamp_now(snapshot->mutable_expires_at());
    snapshot->mutable_expires_at()->set_seconds(snapshot->expires_at().seconds() + 600);
    snapshot->mutable_page()->set_page_size(input->page().page_size());
    if (page_token.empty()) {
      snapshot->set_snapshot_id(input->catalog().id() == kAdapterMismatchCatalog
                                    ? "adapter-catalog-snapshot-a"
                                    : "adapter-catalog-snapshot-1");
      snapshot->mutable_page()->set_next_page_token("page-2");
      adapter_catalog_record(output->add_adapters(), "77777777-7777-4777-8777-000000000a01", 1);
      adapter_catalog_record(output->add_adapters(), "77777777-7777-4777-8777-000000000a02",
                             static_cast<std::uint64_t>(1 + changing_adapter_calls_.fetch_add(1)));
      return grpc::Status::OK;
    }
    if (page_token != "page-2") {
      return failure(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    if (input->catalog().id() == kAdapterFailCatalog) {
      return failure(wire::PERMISSION_DENIED, grpc::StatusCode::PERMISSION_DENIED,
                     input->context());
    }
    snapshot->set_snapshot_id(input->catalog().id() == kAdapterMismatchCatalog
                                  ? "adapter-catalog-snapshot-b"
                                  : "adapter-catalog-snapshot-1");
    adapter_catalog_record(output->add_adapters(), "77777777-7777-4777-8777-000000000a03", 1);
    return grpc::Status::OK;
  }
  grpc::Status instance_catalog_page(const wire::RuntimeListInstancesRequest* input,
                                     wire::RuntimeListInstancesResponse* output) {
    const std::string page_token = input->page().page_token();
    auto* snapshot = output->mutable_snapshot();
    snapshot->set_owner_service("runtime");
    snapshot->set_authorization_version(1);
    stamp_now(snapshot->mutable_observed_at());
    stamp_now(snapshot->mutable_expires_at());
    snapshot->mutable_expires_at()->set_seconds(snapshot->expires_at().seconds() + 600);
    snapshot->mutable_page()->set_page_size(input->page().page_size());
    if (page_token.empty()) {
      snapshot->set_snapshot_id(input->catalog().id() == kInstanceMismatchCatalog
                                    ? "instance-catalog-snapshot-a"
                                    : "instance-catalog-snapshot-1");
      snapshot->mutable_page()->set_next_page_token("page-2");
      instance_catalog_record(output->add_instances(), "66666666-6666-4666-8666-000000000b01", 1);
      instance_catalog_record(
          output->add_instances(), "66666666-6666-4666-8666-000000000b02",
          static_cast<std::uint64_t>(1 + changing_instance_calls_.fetch_add(1)));
      return grpc::Status::OK;
    }
    if (page_token != "page-2") {
      return failure(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    if (input->catalog().id() == kInstanceFailCatalog) {
      return failure(wire::PERMISSION_DENIED, grpc::StatusCode::PERMISSION_DENIED,
                     input->context());
    }
    snapshot->set_snapshot_id(input->catalog().id() == kInstanceMismatchCatalog
                                  ? "instance-catalog-snapshot-b"
                                  : "instance-catalog-snapshot-1");
    instance_catalog_record(output->add_instances(), "66666666-6666-4666-8666-000000000b03", 1);
    return grpc::Status::OK;
  }
  grpc::Status ListHosts(grpc::ServerContext*, const wire::RuntimeListHostsRequest* input,
                         wire::RuntimeListHostsResponse*) override {
    return failure(wire::CAPABILITY_NOT_SUPPORTED, grpc::StatusCode::UNIMPLEMENTED,
                   input->context());
  }
  grpc::Status QueryActionResult(grpc::ServerContext*,
                                 const wire::RuntimeQueryActionResultRequest* input,
                                 wire::RuntimeQueryActionResultResponse* output) override {
    bool handled = false;
    auto status =
        marker_status(input->action_id() + input->owner_instance_id(), input->context(), &handled);
    if (handled)
      return status;
    auto* result = output->mutable_result();
    result->set_action_id(input->action_id().empty() ? "action-fixture" : input->action_id());
    result->set_status(wire::SUCCEEDED);
    result->set_execution_outcome(wire::EXECUTION_KNOWN_SUCCEEDED);
    result->set_platform_message_id("platform-message-fixture");
    result->mutable_completed_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
  grpc::Status BindAdapter(grpc::ServerContext*, const wire::RuntimeBindAdapterRequest* input,
                           wire::RuntimeBindAdapterResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->binding().account_id(), input->context(), &handled);
    if (handled)
      return status;
    *output->mutable_binding() = input->binding();
    operation(output->mutable_operation(), "bind-operation", "runtime", {});
    return grpc::Status::OK;
  }
  grpc::Status UnbindAdapter(grpc::ServerContext*, const wire::RuntimeUnbindAdapterRequest* input,
                             wire::RuntimeUnbindAdapterResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->binding().account_id(), input->context(), &handled);
    if (handled)
      return status;
    operation(output->mutable_operation(), "unbind-operation", "runtime", {});
    return grpc::Status::OK;
  }
  grpc::Status CreateInstallation(grpc::ServerContext*,
                                  const wire::RuntimeCreateInstallationRequest* input,
                                  wire::RuntimeCreateInstallationResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->plugin_type_id(), input->context(), &handled);
    if (handled)
      return status;
    auto* installation = output->mutable_installation();
    installation->set_installation_id("installation-fixture");
    installation->set_plugin_type_id(input->plugin_type_id());
    return grpc::Status::OK;
  }
  grpc::Status CreateInstance(grpc::ServerContext*, const wire::RuntimeCreateInstanceRequest* input,
                              wire::RuntimeCreateInstanceResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->installation_id(), input->context(), &handled);
    if (handled)
      return status;
    auto* instance = output->mutable_instance();
    instance->set_instance_id("instance-fixture");
    instance->set_installation_id(input->installation_id());
    instance->set_host_id(input->host_id());
    instance->set_externally_managed(input->externally_managed());
    instance->mutable_state()->set_version(1);
    instance->mutable_state()->set_lifecycle(wire::LIFECYCLE_ACTIVE);
    instance->mutable_state()->set_desired(wire::DESIRED_RUNNING);
    instance->mutable_state()->set_registered(false);
    instance->mutable_state()->set_process_outcome(wire::EXECUTION_NOT_STARTED);
    instance->mutable_state()->mutable_observed_at()->set_seconds(kNow);
    instance->mutable_state()->set_source_service("runtime");
    return grpc::Status::OK;
  }
  grpc::Status SetDesiredState(grpc::ServerContext*,
                               const wire::RuntimeSetDesiredStateRequest* input,
                               wire::RuntimeSetDesiredStateResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->instance_id(), input->context(), &handled);
    if (handled)
      return status;
    auto* instance = output->mutable_instance();
    instance->set_instance_id(input->instance_id());
    instance->set_installation_id("installation-fixture");
    instance->set_host_id("host-fixture");
    instance->set_externally_managed(false);
    auto* state = instance->mutable_state();
    state->mutable_resource()->set_kind(wire::RESOURCE_INSTANCE);
    state->mutable_resource()->set_id(input->instance_id());
    state->set_version(2);
    state->set_lifecycle(wire::LIFECYCLE_ACTIVE);
    state->set_desired(input->desired_state());
    state->set_registered(true);
    state->set_desired_config_version(2);
    state->set_applied_config_version(1);
    state->set_process_outcome(wire::EXECUTION_KNOWN_SUCCEEDED);
    state->mutable_observed_at()->set_seconds(kNow);
    state->set_source_service("host");
    operation(output->mutable_operation(), "desired-state-operation", "runtime", state->resource());
    return grpc::Status::OK;
  }
  grpc::Status UpdateConfig(grpc::ServerContext*, const wire::RuntimeUpdateConfigRequest* input,
                            wire::RuntimeUpdateConfigResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->resource().id(), input->context(), &handled);
    if (handled)
      return status;
    auto* state = output->mutable_state();
    *state->mutable_resource() = input->resource();
    state->set_version(3);
    state->mutable_observed_at()->set_seconds(kNow);
    operation(output->mutable_operation(), "config-operation", "runtime", input->resource());
    return grpc::Status::OK;
  }
  grpc::Status GetInstanceConfig(grpc::ServerContext*,
                                 const wire::RuntimeGetInstanceConfigRequest* input,
                                 wire::RuntimeGetInstanceConfigResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->resource().id(), input->context(), &handled);
    if (handled)
      return status;
    auto* config = output->mutable_config();
    *config->mutable_instance() = input->resource();
    config->set_desired_version(2);
    config->set_applied_version(1);
    config->set_apply_state(wire::CONFIG_APPLY_PENDING);
    config->set_delivery_id("delivery-fixture");
    config->mutable_observed_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
  grpc::Status GetActionPolicy(grpc::ServerContext*,
                               const wire::RuntimeGetActionPolicyRequest* input,
                               wire::RuntimeGetActionPolicyResponse* output) override {
    auto* policy = output->mutable_policy();
    policy->set_account_id(input->account_id());
    policy->set_mute_enabled(true);
    policy->add_allowed_group_ids("group-fixture");
    policy->set_version(4);
    return grpc::Status::OK;
  }
  grpc::Status UpdateActionPolicy(grpc::ServerContext*,
                                  const wire::RuntimeUpdateActionPolicyRequest* input,
                                  wire::RuntimeUpdateActionPolicyResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->policy().account_id(), input->context(), &handled);
    if (handled)
      return status;
    *output->mutable_policy() = input->policy();
    output->mutable_policy()->set_version(5);
    return grpc::Status::OK;
  }
  grpc::Status BeginDelete(grpc::ServerContext*, const wire::RuntimeBeginDeleteRequest* input,
                           wire::RuntimeBeginDeleteResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->resource().id(), input->context(), &handled);
    if (handled)
      return status;
    operation(output->mutable_operation(), "delete-operation", "runtime", input->resource());
    return grpc::Status::OK;
  }
  grpc::Status RestoreResource(grpc::ServerContext*,
                               const wire::RuntimeRestoreResourceRequest* input,
                               wire::RuntimeRestoreResourceResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->resource().id(), input->context(), &handled);
    if (handled)
      return status;
    operation(output->mutable_operation(), "restore-operation", "runtime", input->resource());
    return grpc::Status::OK;
  }
  grpc::Status GetOperation(grpc::ServerContext*, const wire::RuntimeGetOperationRequest* input,
                            wire::RuntimeGetOperationResponse* output) override {
    auto* operation_out = output->mutable_operation();
    *operation_out->mutable_ref() = input->operation();
    operation_out->set_version(7);
    operation_out->set_state(wire::OPERATION_BLOCKED);
    operation_out->set_execution_outcome(wire::EXECUTION_UNKNOWN);
    operation_out->mutable_observed_at()->set_seconds(kNow);
    auto* participant = operation_out->add_participants();
    participant->set_participant("runtime");
    participant->set_state(wire::OPERATION_BLOCKED);
    participant->mutable_next_attempt_at()->set_seconds(kNow + 30);
    operation_out->set_stalled(false);
    return grpc::Status::OK;
  }
  grpc::Status ListAudit(grpc::ServerContext*, const wire::RuntimeListAuditRequest* input,
                         wire::RuntimeListAuditResponse* output) override {
    auto* row = output->add_entries();
    row->set_entry_id("aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa");
    row->set_actor("runtime-fixture");
    row->mutable_resource()->set_kind(wire::RESOURCE_ACCOUNT);
    row->mutable_resource()->set_id("11111111-1111-4111-8111-111111111111");
    row->set_request_id(input->context().request_id());
    row->set_trace_id(input->context().trace_id());
    row->set_method("fixture.runtime.audit");
    row->set_authorization_version(18446744073709551615ULL);
    row->mutable_occurred_at()->set_seconds(kNow);
    output->mutable_page()->set_page_size(input->filter().page().page_size());
    output->mutable_checked_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
  grpc::Status ListLogs(grpc::ServerContext*, const wire::RuntimeListLogsRequest* input,
                        wire::RuntimeListLogsResponse* output) override {
    auto* row = output->add_entries();
    row->set_entry_id("bbbbbbbb-1111-4111-8111-bbbbbbbbbbbb");
    row->set_severity("info");
    row->set_component("runtime-fixture");
    row->set_method("fixture.runtime.log");
    row->set_actor("runtime-fixture");
    row->mutable_resource()->set_kind(wire::RESOURCE_ACCOUNT);
    row->mutable_resource()->set_id("11111111-1111-4111-8111-111111111111");
    row->set_request_id(input->context().request_id());
    row->set_trace_id(input->context().trace_id());
    row->set_message("fixture runtime log");
    row->mutable_occurred_at()->set_seconds(kNow);
    output->mutable_page()->set_page_size(input->filter().page().page_size());
    output->mutable_checked_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
};
void task(wire::TaskRecord* output, const std::string& id) {
  output->set_task_id(id);
  output->set_owner_instance_id("22222222-2222-4222-8222-222222222222");
  output->set_installation_id("installation-fixture");
  output->mutable_target()->set_id("11111111-1111-4111-8111-111111111111");
  output->set_state(wire::SCHEDULED_TASK_RUNNING);
  output->set_version(2);
  output->mutable_created_at()->set_seconds(kNow);
  output->mutable_updated_at()->set_seconds(kNow);
}
class FakeScheduler final : public wire::SchedulerService::Service {
public:
  std::atomic<int> tasks_feed_state_{0};
  // v1.2 Scheduler task metadata feed: non-empty first page + cursor continuation.
  grpc::Status ListTaskChanges(grpc::ServerContext*, const wire::ObservationChangesRequest* input,
                               wire::TaskChangesResponse* output) override {
    if (!input->has_context() || !input->has_scope() || input->scope().resources_size() != 1 ||
        (input->scope().resources(0).kind() != wire::RESOURCE_TASK &&
         input->scope().resources(0).kind() != wire::RESOURCE_INSTANCE) ||
        input->page_size() > 100) {
      return failure(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    const bool gap = input->scope().resources(0).kind() == wire::RESOURCE_TASK &&
                     input->scope().resources(0).id() == "00000000-0000-4000-8000-0000000000ff";
    const auto dataset = [&](wire::StreamCursor* cursor, std::uint64_t sequence) {
      cursor->set_source_service("scheduler");
      cursor->set_epoch("3d000000-0000-4000-8000-000000000001");
      *cursor->mutable_scope() = input->scope();
      cursor->set_sequence(sequence);
    };
    if (input->has_cursor()) {
      if (input->cursor().source_service() != "scheduler" ||
          input->cursor().epoch() != "3d000000-0000-4000-8000-000000000001" ||
          !same_scope(input->cursor().scope(), input->scope())) {
        return stream_resync(input->context());
      }
      if (gap) {
        // Simulate an evicted journal boundary: the captured head is no longer resumable.
        return stream_resync(input->context());
      }
    }
    std::uint64_t next = gap ? 5 : 1;
    if (!input->has_cursor()) {
      tasks_feed_state_.store(gap ? 0 : 1);
      next = gap ? 5 : 0;
    } else if (!gap && input->cursor().sequence() == 0 && tasks_feed_state_.load() == 1) {
      auto* change = output->add_changes();
      dataset(change->mutable_cursor(), 1);
      auto* task = change->mutable_task();
      task->set_owner_service("scheduler");
      task->set_task_id("55555555-5555-4555-8555-000000000001");
      task->set_owner_instance_id("66666666-6666-4666-8666-000000000001");
      task->set_state(wire::SCHEDULED_TASK_PENDING);
      task->set_version(1);
      task->set_cancel_requested(false);
      task->mutable_updated_at()->set_seconds(kNow);
      tasks_feed_state_.store(2);
      next = 1;
    } else {
      next = input->cursor().sequence();
    }
    dataset(output->mutable_next_cursor(), next);
    output->set_has_more(false);
    stamp_now(output->mutable_checked_at());
    return grpc::Status::OK;
  }

  grpc::Status ListTasks(grpc::ServerContext*, const wire::SchedulerListTasksRequest* input,
                         wire::SchedulerListTasksResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->context().request_id(), input->context(), &handled);
    if (handled)
      return status;
    task(output->add_tasks(), "task-fixture");
    output->mutable_page()->set_page_size(input->page().page_size());
    return grpc::Status::OK;
  }
  grpc::Status GetTask(grpc::ServerContext*, const wire::SchedulerGetTaskRequest* input,
                       wire::SchedulerGetTaskResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->task_id(), input->context(), &handled);
    if (handled)
      return status;
    task(output->mutable_task(), input->task_id());
    return grpc::Status::OK;
  }
  grpc::Status CancelTask(grpc::ServerContext*, const wire::SchedulerCancelTaskRequest* input,
                          wire::SchedulerCancelTaskResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->task_id(), input->context(), &handled);
    if (handled)
      return status;
    task(output->mutable_task(), input->task_id());
    output->mutable_task()->set_state(wire::SCHEDULED_TASK_CANCELLED);
    return grpc::Status::OK;
  }
  grpc::Status GetOccurrence(grpc::ServerContext*, const wire::SchedulerGetOccurrenceRequest* input,
                             wire::SchedulerGetOccurrenceResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->occurrence_id(), input->context(), &handled);
    if (handled)
      return status;
    auto* occurrence = output->mutable_occurrence();
    occurrence->set_occurrence_id(input->occurrence_id());
    occurrence->set_task_id("task-fixture");
    occurrence->set_state(wire::OPERATION_SUCCEEDED);
    occurrence->set_accepted_event_id("event-fixture");
    occurrence->set_attempts(1);
    occurrence->mutable_created_at()->set_seconds(kNow);
    occurrence->mutable_updated_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
  grpc::Status GetOperation(grpc::ServerContext*, const wire::SchedulerGetOperationRequest* input,
                            wire::SchedulerGetOperationResponse* output) override {
    auto* operation_out = output->mutable_operation();
    *operation_out->mutable_ref() = input->operation();
    operation_out->set_version(1);
    operation_out->set_state(wire::OPERATION_SUCCEEDED);
    operation_out->set_execution_outcome(wire::EXECUTION_KNOWN_SUCCEEDED);
    operation_out->mutable_observed_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
};
class FakeHost final : public wire::HostManagementService::Service {
public:
  grpc::Status GetMaintenance(grpc::ServerContext*, const wire::HostGetMaintenanceRequest* input,
                              wire::HostGetMaintenanceResponse* output) override {
    maintenance(output->mutable_maintenance(), input->host_id(), 4, wire::HOST_ADMISSION_OPEN);
    return grpc::Status::OK;
  }
  grpc::Status BeginMaintenance(grpc::ServerContext*,
                                const wire::HostBeginMaintenanceRequest* input,
                                wire::HostMaintenanceResponse* output) override {
    if (input->has_expected_version() && input->expected_version() == 9999) {
      return failure(wire::CONFLICT, grpc::StatusCode::ABORTED, input->context());
    }
    if (input->has_expected_version() && input->expected_version() == 8888) {
      return failure(wire::UPSTREAM_UNAVAILABLE, grpc::StatusCode::UNAVAILABLE, input->context());
    }
    maintenance(output->mutable_maintenance(), input->host_id(), 5, wire::HOST_ADMISSION_CLOSED);
    auto* operation_out = output->mutable_operation();
    operation_host(operation_out->mutable_ref(), "55555555-5555-4555-8555-555555555555",
                   input->host_id());
    operation_out->set_version(1);
    operation_out->set_state(wire::OPERATION_ACCEPTED);
    operation_out->set_execution_outcome(wire::EXECUTION_UNKNOWN);
    operation_out->mutable_observed_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
  grpc::Status ResumeAdmission(grpc::ServerContext*, const wire::HostResumeAdmissionRequest* input,
                               wire::HostMaintenanceResponse* output) override {
    maintenance(output->mutable_maintenance(), input->host_id(), 6, wire::HOST_ADMISSION_OPEN);
    auto* operation_out = output->mutable_operation();
    operation_host(operation_out->mutable_ref(), "66666666-6666-4666-8666-666666666666",
                   input->host_id());
    operation_out->set_version(1);
    operation_out->set_state(wire::OPERATION_SUCCEEDED);
    operation_out->set_execution_outcome(wire::EXECUTION_KNOWN_SUCCEEDED);
    operation_out->mutable_observed_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
  grpc::Status GetOperation(grpc::ServerContext*, const wire::HostGetOperationRequest* input,
                            wire::HostGetOperationResponse* output) override {
    auto* operation_out = output->mutable_operation();
    *operation_out->mutable_ref() = input->operation();
    operation_out->set_version(7);
    operation_out->set_state(wire::OPERATION_BLOCKED);
    operation_out->set_execution_outcome(wire::EXECUTION_UNKNOWN);
    operation_out->mutable_observed_at()->set_seconds(kNow);
    auto* participant = operation_out->add_participants();
    participant->set_participant("managed_executions");
    participant->set_state(wire::OPERATION_BLOCKED);
    return grpc::Status::OK;
  }
  grpc::Status CancelOperation(grpc::ServerContext*, const wire::HostCancelOperationRequest* input,
                               wire::HostGetOperationResponse* output) override {
    auto* operation_out = output->mutable_operation();
    *operation_out->mutable_ref() = input->operation();
    operation_out->set_version(2);
    operation_out->set_state(wire::OPERATION_CANCELLED);
    operation_out->set_execution_outcome(wire::EXECUTION_UNKNOWN);
    operation_out->mutable_observed_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
  grpc::Status ListOperationChanges(grpc::ServerContext*, const wire::OperationChangesRequest*,
                                    wire::OperationChangesResponse* output) override {
    output->mutable_checked_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }

private:
  static void maintenance(wire::HostMaintenanceRecord* output, const std::string& host_id,
                          std::uint64_t version, wire::HostAdmissionState admission) {
    output->set_host_id(host_id);
    output->set_version(version);
    output->set_admission(admission);
    output->mutable_observed_at()->set_seconds(kNow);
  }
  static void operation_host(wire::OperationRef* output, const std::string& id,
                             const std::string& host_id) {
    output->set_owner_service("host");
    output->set_operation_id(id);
    output->mutable_resource()->set_kind(wire::RESOURCE_HOST);
    output->mutable_resource()->set_id(host_id);
  }
};

} // namespace

int main(int argc, char* argv[]) {
  if (argc != 4) {
    return 2;
  }
  grpc::SslServerCredentialsOptions options;
  options.pem_root_certs = read(argv[1]);
  options.pem_key_cert_pairs.push_back({read(argv[3]), read(argv[2])});
  options.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::SslServerCredentials(options), &port);
  FakeRuntime runtime;
  FakeScheduler scheduler;
  FakeHost host;
  builder.RegisterService(&runtime);
  builder.RegisterService(&scheduler);
  builder.RegisterService(&host);
  auto server = builder.BuildAndStart();
  if (!server) {
    return 1;
  }
  std::cout << port << '\n' << std::flush;
  server->Wait();
}
