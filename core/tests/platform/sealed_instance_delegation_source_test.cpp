// P05b-5/B1 sealed-file reader matrix. P05b-6 owns the Control/deployment writer and exact file
// production pipeline; this test fixes the Runtime-side fail-closed contract: 0600 regular
// non-symlink file, bounded JSON envelope, instance/installation/audience/permission/scope match,
// 56s max TTL, no token returned after expiry or on any mismatch.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include <sys/stat.h>

#include "kiarabot/core/runtime/platform/sealed_instance_delegation_source.hpp"

namespace {
namespace t = kiarabot::core::target;
namespace platform = kiarabot::core::platform;

void check(bool passed, const char* scenario) {
  if (!passed) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}
constexpr auto kInstance = "11111111-1111-4111-8111-111111111111";
constexpr auto kOtherInstance = "33333333-3333-4333-8333-333333333333";
constexpr auto kInstallation = "22222222-2222-4222-8222-222222222222";
constexpr auto kToken = "header.payload.signature";

t::SchedulerTaskContext context(std::string instance = kInstance) {
  t::SchedulerTaskContext value;
  value.owner = t::InstanceId{std::move(instance)};
  value.installation = t::InstallationId{kInstallation};
  value.request_id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
  value.trace_id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
  value.deadline = platform::delegation_provider_now() + std::chrono::seconds{2};
  value.idempotency_key = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
  return value;
}
std::string valid_json(std::int64_t issued, std::int64_t expires, std::string instance = kInstance,
                       std::string installation = kInstallation, std::string audience = "runtime",
                       std::string permissions = R"(["task.schedule","task.read"])",
                       std::string token = kToken) {
  return std::string{"{\"instance_id\":\""} + instance + "\",\"installation_id\":\"" +
         installation + "\",\"audience\":\"" + audience + "\",\"permissions\":" + permissions +
         ",\"scope\":[{\"kind\":\"RESOURCE_INSTANCE\",\"id\":\"" + instance +
         "\"},{\"kind\":\"RESOURCE_INSTALLATION\",\"id\":\"" + installation +
         "\"}],\"issued_at\":" + std::to_string(issued) +
         ",\"expires_at\":" + std::to_string(expires) + ",\"token\":\"" + token + "\"}";
}
void write(const std::filesystem::path& path, std::string_view value, mode_t mode = 0600) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << value;
  output.close();
  check(::chmod(path.c_str(), mode) == 0, "chmod");
}
bool failure_is(const t::Outcome<platform::InstanceParentDelegation>& value, t::ErrorCode code) {
  const auto* failure = std::get_if<t::Failure>(&value);
  return failure != nullptr && failure->code == code;
}
} // namespace

int main() {
  const auto directory = std::filesystem::temp_directory_path() / "p05b5-sealed-test";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = directory / "parent.json";
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  auto source = platform::SealedInstanceDelegationSource::create({path.string()});
  check(!std::holds_alternative<t::Failure>(source), "source create");
  auto& reader = *std::get<std::unique_ptr<platform::SealedInstanceDelegationSource>>(source);

  write(path, valid_json(now, now + 30));
  auto valid = reader.acquire(context(), t::Permission::TaskSchedule);
  check(!std::holds_alternative<t::Failure>(valid) &&
            std::get<platform::InstanceParentDelegation>(valid).token == kToken,
        "valid sealed file token returned");

  write(path, valid_json(now, now + 30, kInstance, kInstallation, "runtime", R"(["task.read"])"));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "missing requested permission rejected");
  check(!std::holds_alternative<t::Failure>(reader.acquire(context(), t::Permission::TaskRead)),
        "declared permission accepted");

  write(path, valid_json(now, now + 30, kOtherInstance));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "instance mismatch rejected");
  write(path, valid_json(now, now + 30, kInstance, "44444444-4444-4444-8444-444444444444"));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "installation mismatch rejected");
  write(path, valid_json(now, now + 30, kInstance, kInstallation, "scheduler"));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "parent audience must be runtime");
  write(path, valid_json(now + 30, now + 30));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "expired token rejected");
  write(path, valid_json(now, now + 57));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "TTL over 56s rejected");
  write(path, valid_json(now, now + 30, kInstance, kInstallation, "runtime", R"(["task.schedule"])",
                         "token with space"));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "non-printable/whitespace token rejected");
  write(path, "{not-json");
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "malformed JSON rejected");
  write(path, valid_json(now, now + 30), 0644);
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "non-0600 mode rejected");
  write(path, valid_json(now, now + 30));
  const auto link = directory / "parent-link.json";
  std::filesystem::remove(link);
  std::filesystem::create_symlink(path, link);
  auto link_source = platform::SealedInstanceDelegationSource::create({link.string()});
  check(!std::holds_alternative<t::Failure>(link_source), "symlink source construct");
  check(failure_is(std::get<std::unique_ptr<platform::SealedInstanceDelegationSource>>(link_source)
                       ->acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "symlink rejected");
  const auto missing = directory / "missing.json";
  auto missing_source = platform::SealedInstanceDelegationSource::create({missing.string()});
  check(!std::holds_alternative<t::Failure>(missing_source), "missing source construct");
  check(
      failure_is(std::get<std::unique_ptr<platform::SealedInstanceDelegationSource>>(missing_source)
                     ->acquire(context(), t::Permission::TaskSchedule),
                 t::ErrorCode::Unauthenticated),
      "missing file rejected");

  std::filesystem::remove_all(directory);
  std::cout << "P05b-5 sealed delegation source matrix passed\n";
  return 0;
}
