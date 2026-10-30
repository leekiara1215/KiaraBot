#include <iostream>
#include <sstream>
#include <stdexcept>

#include "kiarabot/infrastructure/runtime_log_sink.hpp"

namespace {
using namespace std::chrono_literals;
namespace core = kiarabot::core;
void check(bool value) {
  if (!value) {
    throw std::runtime_error("runtime log assertion failed");
  }
}
} // namespace
int main() try {
  auto now = core::RuntimeTimePoint(1700000000s);
  std::ostringstream output;
  auto sink = std::make_shared<kiarabot::infrastructure::BufferedRuntimeLogSink>(
      &output, [&] { return now; });
  auto memory = std::make_unique<core::InMemoryRuntimeStore>();
  auto* fault = memory.get();
  core::LoggingRuntimeStore store(std::move(memory), sink, [&] { return now; });
  auto begun = store.begin();
  auto transaction = std::get<std::unique_ptr<core::RuntimeTransaction>>(std::move(begun));
  auto& state = transaction->snapshot();
  state.audit.push_back({"actor", "plugin.manage", "plugin", "request", "trace", now, true, "OK"});
  state.audit.push_back(
      {"actor", "plugin.manage", "plugin", "request", "trace", now, false, "PERMISSION_DENIED"});
  state.events["event"].record = {"event", "trace", "session", 0, false, "private-body-and-token"};
  state.deliveries["event"]["plugin"].state = core::DeliveryState::retrying;
  check(std::holds_alternative<std::monostate>(transaction->commit()));
  check(sink->read().size() == 4);
  check(output.str().find("private-body-and-token") == std::string::npos);
  check(output.str().find("RETRYING") != std::string::npos);
  begun = store.begin();
  transaction = std::get<std::unique_ptr<core::RuntimeTransaction>>(std::move(begun));
  transaction->snapshot().deliveries["event"]["plugin"].state = core::DeliveryState::dead_letter;
  check(std::holds_alternative<std::monostate>(transaction->commit()));
  check(output.str().find("DEAD_LETTER") != std::string::npos);
  fault->set_fail_reads(true);
  check(std::holds_alternative<core::StorageError>(store.read()));
  fault->set_fail_reads(false);
  begun = store.begin();
  transaction = std::get<std::unique_ptr<core::RuntimeTransaction>>(std::move(begun));
  output.setstate(std::ios::badbit);
  transaction->snapshot().audit.push_back({"actor", "write", "x", "req", "trace", now, true, "OK"});
  check(std::holds_alternative<std::monostate>(transaction->commit()));
  check(store.sink_failures() == 1);
  check(std::get<core::RuntimeSnapshot>(store.read()).audit.size() == 3);
  now += 601s;
  check(sink->read().empty());
  std::cout << "runtime log success/deny/retry/dead-letter/fault passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
