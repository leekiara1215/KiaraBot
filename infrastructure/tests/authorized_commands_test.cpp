#include <iostream>
#include <source_location>
#include <sqlite3.h>
#include <stdexcept>

#include "kiarabot/core/authorized_commands.hpp"
#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"

namespace {
void check(bool result, std::source_location location = std::source_location::current()) {
  if (!result) {
    throw std::runtime_error("Authorization assertion failed at line " +
                             std::to_string(location.line()));
  }
}
template <typename T> T value(kiarabot::core::StorageResult<T> result) {
  check(std::holds_alternative<T>(result));
  return std::get<T>(std::move(result));
}
void exercise(kiarabot::core::RuntimeStore& store) {
  using kiarabot::core::AuthorizedCommands;
  using kiarabot::core::CommandContext;
  using kiarabot::core::PortStatus;
  using kiarabot::core::RuntimeSnapshot;
  using kiarabot::core::RuntimeTimePoint;
  using kiarabot::v1::Error;
  CommandContext request{"plugin", "message.send",      "group", "request", "trace",
                         "key",    std::string(64, 'a')};
  auto clock = [] { return RuntimeTimePoint{}; };
  AuthorizedCommands denied(store, {}, clock);
  bool invoked = false;
  auto effect = [&](RuntimeSnapshot& state) -> PortStatus {
    invoked = true;
    state.delivery_order = 42;
    return std::monostate{};
  };
  check(std::get<Error>(denied.execute(request, effect)).code() == kiarabot::v1::PERMISSION_DENIED);
  check(!invoked && value(store.read()).delivery_order == 0);
  AuthorizedCommands allowed(store, {{"plugin", "message.send", "group"}}, clock);
  check(std::holds_alternative<std::monostate>(allowed.execute(request, effect)));
  check(invoked && value(store.read()).delivery_order == 42);
  auto count = value(store.read()).audit.size();
  invoked = false;
  check(std::holds_alternative<std::monostate>(allowed.execute(request, effect)));
  check(!invoked && value(store.read()).audit.size() == count);
  request.fingerprint = std::string(64, 'b');
  check(std::get<Error>(allowed.execute(request, effect)).code() == kiarabot::v1::CONFLICT);
  request.idempotency_key = "failure";
  auto failed = [](RuntimeSnapshot& state) -> PortStatus {
    state.delivery_order = 999;
    Error error;
    error.set_code(kiarabot::v1::TIMEOUT);
    error.set_message("secret");
    return error;
  };
  const auto error = std::get<Error>(allowed.execute(request, failed));
  check(error.code() == kiarabot::v1::TIMEOUT &&
        error.message().find("secret") == std::string::npos);
  check(value(store.read()).delivery_order == 42 &&
        value(store.read()).audit.back().outcome == "TIMEOUT");
  AuthorizedCommands full(store, {{"plugin", "message.send", "group"}}, clock, 0);
  check(std::get<Error>(full.execute(request, effect)).code() ==
        kiarabot::v1::UPSTREAM_UNAVAILABLE);
  check(!invoked);
  request.subject = "other";
  check(std::get<Error>(allowed.execute(request, effect)).code() ==
        kiarabot::v1::PERMISSION_DENIED);
  kiarabot::core::Runtime runtime(store);
  value(runtime.ingest({"event", "trace", "", 0, false}));
  check(value(store.read()).audit.size() >= 4 && !value(store.read()).commands.empty());
}
} // namespace
int main() {
  try {
    kiarabot::core::InMemoryRuntimeStore memory;
    exercise(memory);
    auto sqlite = value(kiarabot::infrastructure::SqliteRuntimeStore::open(":memory:"));
    exercise(*sqlite);
    kiarabot::core::InMemoryRuntimeStore broken;
    broken.set_fail_commits(true);
    kiarabot::core::AuthorizedCommands commands(broken, {{"p", "write", "r"}}, {});
    kiarabot::core::CommandContext request{
        "p", "write", "r", "request", "trace", "key", std::string(64, 'a')};
    auto outcome = commands.execute(request, [](auto& state) -> kiarabot::core::PortStatus {
      state.delivery_order = 1;
      return std::monostate{};
    });
    check(std::get<kiarabot::v1::Error>(outcome).code() == kiarabot::v1::UPSTREAM_UNAVAILABLE);
    auto original = value(broken.read());
    check(original.delivery_order == 0 && original.audit.empty() && original.commands.empty());
    std::cout << "Default denial, idempotency, atomic audit and failure redaction passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
