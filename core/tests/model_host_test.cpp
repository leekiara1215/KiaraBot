#include <functional>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "kiarabot/core/host_dispatcher.hpp"

namespace {
namespace wire = kiarabot::v1;
using namespace std::chrono_literals;
void check(bool ok, const std::source_location& at = std::source_location::current()) {
  if (!ok) {
    throw std::runtime_error("model host assertion " + std::to_string(at.line()));
  }
}
struct Provider final : kiarabot::core::IModelProvider {
  int calls{0};
  std::function<void()> during;
  wire::ErrorCode code{wire::ERROR_CODE_UNSPECIFIED};
  kiarabot::core::PortResult<wire::ModelChatResponse>
  chat(const wire::ModelChatRequest&, std::chrono::milliseconds timeout) override {
    ++calls;
    check(timeout > 0ms && timeout <= 100s);
    if (during) {
      during();
    }
    if (code != wire::ERROR_CODE_UNSPECIFIED) {
      wire::Error error;
      error.set_code(code);
      error.set_message("private-provider-secret");
      (*error.mutable_details())["secret"] = "private-provider-secret";
      return error;
    }
    wire::ModelChatResponse response;
    response.set_text("answer");
    return response;
  }
};
} // namespace
int main() try {
  kiarabot::core::InMemoryRuntimeStore store;
  auto now = kiarabot::core::RuntimeTimePoint(1000s);
  auto provider = std::make_shared<Provider>();
  kiarabot::core::HostDispatcher host(store, [&] { return now; }, {}, provider);
  kiarabot::core::PluginSession plugin{
      "one", "instance", "fence", {}, {"model.chat", "storage.namespace"}};
  wire::HostRequest request;
  request.set_request_id("request");
  request.set_trace_id("trace");
  request.mutable_deadline()->set_seconds(1100);
  request.mutable_model_chat()->set_model("default");
  request.mutable_model_chat()->add_messages()->set_role("user");
  auto denied = plugin;
  denied.permissions.clear();
  check(host.dispatch(denied, request).error().code() == wire::PERMISSION_DENIED);
  check(provider->calls == 0);
  store.set_fail_commits(true);
  check(host.dispatch(plugin, request).error().code() == wire::UPSTREAM_UNAVAILABLE);
  check(provider->calls == 0);
  store.set_fail_commits(false);
  provider->during = [&] {
    auto storage = request;
    storage.mutable_storage_get()->set_key("missing");
    check(host.dispatch(plugin, storage).error().code() == wire::NOT_FOUND);
  }; // No Host mutex or database transaction is held across provider I/O.
  check(host.dispatch(plugin, request).model_chat().text() == "answer");
  for (const auto code : {wire::TIMEOUT, wire::RATE_LIMITED, wire::UPSTREAM_UNAVAILABLE}) {
    provider->code = code;
    const auto response = host.dispatch(plugin, request);
    check(response.error().code() == code && !response.has_model_chat());
    check(response.SerializeAsString().find("private-provider-secret") == std::string::npos);
  }
  provider->code = wire::ERROR_CODE_UNSPECIFIED;
  provider->during = [&] { store.set_fail_commits(true); };
  const auto failed_audit = host.dispatch(plugin, request);
  check(failed_audit.error().code() == wire::UPSTREAM_UNAVAILABLE &&
        !failed_audit.error().retryable());
  store.set_fail_commits(false);
  provider->during = [&] { now += 101s; };
  check(host.dispatch(plugin, request).error().code() == wire::TIMEOUT);
  const auto calls = provider->calls;
  check(host.dispatch(plugin, request).error().code() == wire::TIMEOUT);
  check(provider->calls == calls);
  std::cout << "model authorization, audit gating, unlocked I/O, deadline and redaction passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
