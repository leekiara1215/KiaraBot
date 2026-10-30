#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "kiarabot/core/application/host_dispatcher.hpp"
#include "kiarabot/core/application/runtime.h"
#include "kiarabot/core/runtime/in_memory_runtime_store.hpp"
#include "kiarabot/core/runtime/request_digest.hpp"
#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"

namespace {
namespace wire = kiarabot::v1;
using namespace std::chrono_literals;
using kiarabot::core::HostDispatcher;
using kiarabot::core::PluginSession;
using kiarabot::core::RuntimeTimePoint;
void check(bool value, const std::source_location& location = std::source_location::current()) {
  if (!value) {
    throw std::runtime_error("Host assertion " + std::to_string(location.line()));
  }
}
template <typename T, typename E> T value(std::variant<T, E> result) {
  check(std::holds_alternative<T>(result));
  return std::get<T>(std::move(result));
}
wire::HostRequest set(std::string key, std::string bytes, std::string idem) {
  wire::HostRequest request;
  request.set_request_id("request");
  request.set_trace_id("trace");
  request.set_idempotency_key(idem);
  request.mutable_deadline()->set_seconds(1100);
  request.mutable_storage_set()->set_key(key);
  request.mutable_storage_set()->set_value(bytes);
  return request;
}
wire::HostRequest get(std::string key) {
  auto request = set("", "", "");
  request.mutable_storage_get()->set_key(key);
  return request;
}
void scenarios(kiarabot::core::RuntimeStore& store,
               kiarabot::core::InMemoryRuntimeStore* faults = nullptr) {
  auto now = RuntimeTimePoint(1000s);
  HostDispatcher host(store, [&] { return now; });
  PluginSession one{"one", "instance", "fence", {}, {"storage.namespace"}};
  PluginSession two{"two", "instance", "fence", {}, {"storage.namespace"}};
  PluginSession denied{"denied", "instance", "fence", {}, {}};
  auto request = set("key", std::string("a\0b", 3), "1234567890123456");
  const auto refusal = host.dispatch(denied, request);
  check(refusal.error().code() == wire::PERMISSION_DENIED &&
        refusal.request_id() == request.request_id());
  check(value(store.read()).namespaces.empty());
  check(host.dispatch(one, request).has_storage_set());
  const auto audit_count = value(store.read()).audit.size();
  request.set_request_id("reconnected");
  request.set_trace_id("new-trace");
  request.mutable_deadline()->set_seconds(1150);
  const auto duplicate = host.dispatch(one, request);
  check(duplicate.has_storage_set() && duplicate.request_id() == "reconnected");
  check(value(store.read()).audit.size() == audit_count);
  request.mutable_storage_set()->set_value("different");
  check(host.dispatch(one, request).error().code() == wire::CONFLICT);
  check(host.dispatch(one, get("key")).storage_get().value() == std::string("a\0b", 3));
  check(host.dispatch(two, get("key")).error().code() == wire::NOT_FOUND);
  check(host.dispatch(two, set("key", "two", "1234567890123456")).has_storage_set());
  check(host.dispatch(two, get("key")).storage_get().value() == "two");
  auto ttl = set("ttl", "temporary", "ttl1234567890123");
  ttl.mutable_storage_set()->mutable_expires_at()->set_seconds(1001);
  check(host.dispatch(one, ttl).has_storage_set());
  now += 2s;
  check(host.dispatch(one, get("ttl")).error().code() == wire::NOT_FOUND);
  check(host.dispatch(one, ttl).has_storage_set());
  check(host.dispatch(one, get("ttl")).error().code() == wire::NOT_FOUND);
  auto invalid = set("invalid", "value", "invalid123456789");
  invalid.mutable_storage_set()->mutable_expires_at()->set_seconds(1);
  check(host.dispatch(one, invalid).error().code() == wire::INVALID_ARGUMENT);
  invalid.mutable_storage_set()->mutable_expires_at()->set_seconds(10000000000LL);
  check(host.dispatch(one, invalid).error().code() == wire::INVALID_ARGUMENT);
  auto missing = get("missing");
  missing.set_idempotency_key("delete1234567890");
  missing.mutable_storage_delete()->set_key("missing");
  check(host.dispatch(one, missing).has_storage_delete());
  check(host.dispatch(one, set("empty", "", "empty12345678901")).has_storage_set());
  const auto empty = host.dispatch(one, get("empty"));
  check(empty.has_storage_get() && empty.storage_get().value().empty());
  auto overdue = get("key");
  overdue.mutable_deadline()->set_seconds(1);
  check(host.dispatch(one, overdue).error().code() == wire::TIMEOUT);
  if (faults) {
    const auto before = value(store.read());
    faults->set_fail_commits(true);
    check(host.dispatch(one, set("failed", "data", "failure123456789")).error().code() ==
          wire::UPSTREAM_UNAVAILABLE);
    faults->set_fail_commits(false);
    const auto after = value(store.read());
    check(after.audit.size() == before.audit.size());
    check(!after.namespaces.at("one").contains("failed"));
    check(!after.host_receipts.at("one").contains("failure123456789"));
    faults->set_fail_reads(true);
    check(host.dispatch(one, get("key")).error().code() == wire::UPSTREAM_UNAVAILABLE);
    faults->set_fail_reads(false);
  }
  // Ordinary Runtime algorithms must not drop Host data while committing state.
  kiarabot::core::Runtime runtime(store);
  value(runtime.ingest({"event", "trace", "", 0, false}));
  check(host.dispatch(one, get("key")).storage_get().value() == std::string("a\0b", 3));
}
void limits() {
  kiarabot::core::InMemoryRuntimeStore store;
  kiarabot::core::NamespacePolicy policy{4, 6, 2, 5, 100};
  HostDispatcher host(
      store, [] { return RuntimeTimePoint(1000s); }, policy);
  PluginSession plugin{"one", "instance", "fence", {}, {"storage.namespace"}};
  check(host.dispatch(plugin, set("one", "1234", "first12345678901")).has_storage_set());
  check(host.dispatch(plugin, set("two", "567", "second1234567890")).error().code() ==
        wire::RATE_LIMITED);
  check(host.dispatch(plugin, set("two", "56", "second1234567890")).has_storage_set());
  check(host.dispatch(plugin, set("three", "", "third12345678901")).error().code() ==
        wire::RATE_LIMITED);
  check(host.dispatch(plugin, set("one", "12345", "oversize12345678")).error().code() ==
        wire::RATE_LIMITED);
  kiarabot::core::InMemoryRuntimeStore receipt_store;
  policy.receipts = 1;
  HostDispatcher bounded(
      receipt_store, [] { return RuntimeTimePoint(1000s); }, policy);
  check(bounded.dispatch(plugin, set("one", "1", "first12345678901")).has_storage_set());
  check(bounded.dispatch(plugin, set("two", "2", "second1234567890")).error().code() ==
        wire::RATE_LIMITED);
  check(!value(receipt_store.read()).namespaces.at("one").contains("two"));
  policy.audit_records = 0;
  HostDispatcher audit_failure(
      receipt_store, [] { return RuntimeTimePoint(1000s); }, policy);
  check(audit_failure.dispatch(plugin, set("two", "2", "third12345678901")).error().code() ==
        wire::UPSTREAM_UNAVAILABLE);
  kiarabot::core::InMemoryRuntimeStore timed_store;
  int clock_calls = 0;
  HostDispatcher expires_during_request(
      timed_store, [&] { return RuntimeTimePoint(++clock_calls == 1 ? 1000s : 1200s); });
  check(expires_during_request.dispatch(plugin, set("key", "value", "timeout123456789"))
            .error()
            .code() == wire::TIMEOUT);
  check(value(timed_store.read()).namespaces.empty());
  check(value(timed_store.read()).host_receipts.empty());
  check(value(timed_store.read()).audit.empty());
}
} // namespace
int main() try {
  kiarabot::core::InMemoryRuntimeStore memory;
  scenarios(memory, &memory);
  limits();
  const auto path = (std::filesystem::temp_directory_path() /
                     ("kiarabot-host-" + kiarabot::core::random_instance_id() + ".db"))
                        .string();
  {
    auto store = value(kiarabot::infrastructure::SqliteRuntimeStore::open(path));
    scenarios(*store);
  }
  {
    auto store = value(kiarabot::infrastructure::SqliteRuntimeStore::open(path));
    HostDispatcher host(*store, [] { return RuntimeTimePoint(1003s); });
    PluginSession plugin{"one", "instance", "fence", {}, {"storage.namespace"}};
    check(host.dispatch(plugin, get("key")).storage_get().value() == std::string("a\0b", 3));
    check(value(store->read()).host_receipts.at("one").contains("1234567890123456"));
  }
  std::filesystem::remove(path);
  std::cout
      << "Host namespace isolation, receipts, TTL, limits, atomic rollback and restart passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
