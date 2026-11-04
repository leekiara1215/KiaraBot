#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "kiarabot/infrastructure/postgres/database.hpp"

namespace t = kiarabot::core::target;
namespace pg = kiarabot::infrastructure::postgres;
namespace {
template <typename Value> Value take(t::Outcome<Value> result) {
  if (const auto* failure = std::get_if<t::Failure>(&result)) {
    std::cerr << "P03 assertion: observation seed stable code " << static_cast<int>(failure->code)
              << '\n';
    throw std::runtime_error("seed failed");
  }
  return std::move(std::get<Value>(result));
}
std::string secret(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string result;
  std::getline(input, result);
  if (result.size() != 64) {
    throw std::runtime_error("secret unavailable");
  }
  return result;
}
} // namespace

// Invoked only after the Python driver verifies its actual owned PG backend.
// This helper uses the production UoW so fixture journal rows cannot bypass its
// transaction/final-snapshot hooks. No credentials or arbitrary exceptions leave it.
int main(int argc, char** argv) {
  try {
    if (argc != 6) {
      return 2;
    }
    const std::filesystem::path root = std::filesystem::canonical(argv[1]);
    const std::string owner = argv[3];
    if (!root.filename().string().starts_with("kiarabot-p03-") ||
        !std::filesystem::is_regular_file(root / "p03-owned-marker") ||
        (owner != "runtime" && owner != "scheduler")) {
      return 2;
    }
    const auto port = std::stoul(argv[2]);
    if (port == 0 || port > 65535) {
      return 2;
    }
    const auto service = owner == "runtime" ? t::Service::Runtime : t::Service::Scheduler;
    const auto prefix = "p03_" + owner;
    const auto role = prefix + "_runtime";
    auto database =
        take(pg::Database::open({service, "localhost", static_cast<std::uint16_t>(port), prefix,
                                 role, secret(root / ("password-" + role)),
                                 (root / "server.pem").string(), prefix + "_migration", role}));
    auto work = take(database->begin());
    const auto clock = take(work->current_time());
    t::Operation operation;
    operation.ref = {
        service,
        {argv[5]},
        {owner == "runtime" ? t::ResourceKind::Account : t::ResourceKind::Task, {argv[4]}}};
    operation.version = {1};
    operation.state = t::OperationState::Blocked;
    operation.blocked_reason = t::ErrorCode::UpstreamUnavailable;
    operation.next_attempt = clock + std::chrono::seconds{10};
    operation.created_at = clock;
    operation.updated_at = clock;
    take(work->operations().save(operation, {}));
    take(work->save_operation_participants(
        operation.ref.id, {{"owner", t::OperationState::Blocked, t::ErrorCode::UpstreamUnavailable,
                            operation.next_attempt}}));
    if (work->commit() != t::CommitResult::Committed) {
      return 1;
    }
    return 0;
  } catch (...) {
    std::cerr << "P03 assertion: observation seed failed; diagnostics redacted\n";
    return 1;
  }
}
