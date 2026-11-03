// P05b-5/P05b-6 sealed-file reader matrix. P05b-6 owns writing/refresh; this fixes Runtime-side
// validation: 0600 regular non-symlink, 64KiB bounded exact JSON, instance/installation/audience/
// permission/scope/JWT-timestamp/TTL checks, and fail-closed on every mismatch or expiry.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
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

std::string base64url(std::string_view input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string output;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char character : input) {
    buffer = (buffer << 8U) | static_cast<unsigned char>(character);
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      output += kAlphabet[(buffer >> static_cast<unsigned>(bits)) & 0x3fU];
    }
  }
  if (bits > 0) {
    output += kAlphabet[(buffer << static_cast<unsigned>(6 - bits)) & 0x3fU];
  }
  return output;
}
std::string proof_token(std::int64_t issued, std::int64_t expires) {
  return base64url(R"({"alg":"EdDSA"})") + "." +
         base64url(R"({"aud":"runtime","iat":)" + std::to_string(issued) + R"(,"exp":)" +
                   std::to_string(expires) + R"(,"sub":"11111111-1111-4111-8111-111111111111"})") +
         ".signature";
}
std::string document(std::int64_t issued, std::int64_t expires, std::string instance = kInstance,
                     std::string installation = kInstallation, std::string audience = "runtime",
                     std::string permissions = R"(["task.schedule","task.read"])",
                     std::string proof = std::string{}) {
  if (proof.empty()) {
    proof = proof_token(issued, expires);
  }
  return std::string{"{\"schema_version\":1,\"instance_id\":\""} + instance +
         "\",\"installation_id\":\"" + installation + "\",\"audience\":\"" + audience +
         "\",\"permissions\":" + permissions +
         ",\"scope\":[{\"kind\":\"RESOURCE_INSTANCE\",\"id\":\"" + instance +
         "\"},{\"kind\":\"RESOURCE_INSTALLATION\",\"id\":\"" + installation +
         "\"}],\"issued_at\":" + std::to_string(issued) +
         ",\"expires_at\":" + std::to_string(expires) +
         ",\"ttl_seconds\":" + std::to_string(expires - issued) + ",\"delegation_proof\":\"" +
         proof + "\"}";
}
std::string model_document(std::int64_t issued, std::int64_t expires,
                           std::string model = "66666666-6666-4666-8666-666666666666",
                           std::string permissions = R"(["model.chat","model.call.inspect"])") {
  const auto proof = proof_token(issued, expires);
  return std::string{"{\"schema_version\":1,\"instance_id\":\""} + kInstance +
         "\",\"installation_id\":\"" + kInstallation +
         "\",\"audience\":\"runtime\",\"permissions\":" + permissions +
         ",\"scope\":[{\"kind\":\"RESOURCE_MODEL\",\"id\":\"" + model +
         "\"}],\"issued_at\":" + std::to_string(issued) +
         ",\"expires_at\":" + std::to_string(expires) +
         ",\"ttl_seconds\":" + std::to_string(expires - issued) + ",\"delegation_proof\":\"" +
         proof + "\"}";
}
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
  const auto path = directory / "instance-delegation.json";
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  auto source = platform::SealedInstanceDelegationSource::create({path.string()});
  check(!std::holds_alternative<t::Failure>(source), "source create");
  auto& reader = *std::get<std::unique_ptr<platform::SealedInstanceDelegationSource>>(source);

  write(path, document(now, now + 30));
  auto valid = reader.acquire(context(), t::Permission::TaskSchedule);
  check(!std::holds_alternative<t::Failure>(valid) &&
            std::get<platform::InstanceParentDelegation>(valid).token == proof_token(now, now + 30),
        "valid sealed proof returned");

  write(path, document(now, now + 30, kInstance, kInstallation, "runtime", R"(["task.read"])"));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "missing requested permission rejected");
  check(!std::holds_alternative<t::Failure>(reader.acquire(context(), t::Permission::TaskRead)),
        "declared permission accepted");

  write(path, document(now, now + 30, kOtherInstance));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "instance mismatch rejected");
  write(path, document(now, now + 30, kInstance, "44444444-4444-4444-8444-444444444444"));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "installation mismatch rejected");
  write(path, document(now, now + 30, kInstance, kInstallation, "scheduler"));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "parent audience must be runtime");
  write(path, document(now - 30, now - 1));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "expired proof rejected");
  write(path, document(now, now + 57));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "TTL over 56s rejected");
  write(path, document(now, now + 30, kInstance, kInstallation, "runtime", R"(["task.schedule"])",
                       "not-a-compact-jwt"));
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "non-JWT delegation proof rejected");
  write(path, "{not-json");
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "malformed JSON rejected");
  write(path, document(now, now + 30), 0644);
  check(failure_is(reader.acquire(context(), t::Permission::TaskSchedule),
                   t::ErrorCode::Unauthenticated),
        "non-0600 mode rejected");
  write(path, document(now, now + 30));
  const auto link = directory / "instance-delegation-link.json";
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

  const auto model_path = directory / "instance-delegation-model.json";
  auto model_source =
      platform::SealedInstanceDelegationSource::create({path.string(), model_path.string()});
  check(!std::holds_alternative<t::Failure>(model_source), "model source create");
  auto& model_reader =
      *std::get<std::unique_ptr<platform::SealedInstanceDelegationSource>>(model_source);
  constexpr auto kModel = "66666666-6666-4666-8666-666666666666";
  write(model_path, model_document(now, now + 30));
  auto valid_model = model_reader.acquire_model(context(), kModel, t::Permission::ModelChat);
  check(!std::holds_alternative<t::Failure>(valid_model), "valid model sealed proof returned");
  check(failure_is(model_reader.acquire_model(context(), "77777777-7777-4777-8777-777777777777",
                                              t::Permission::ModelChat),
                   t::ErrorCode::Unauthenticated),
        "cross-model scope rejected");
  write(model_path, model_document(now, now + 30, kModel, R"(["model.chat","task.read"])"));
  check(failure_is(model_reader.acquire_model(context(), kModel, t::Permission::ModelChat),
                   t::ErrorCode::Unauthenticated),
        "mixed task/model permissions rejected");
  write(model_path, document(now, now + 30));
  check(failure_is(model_reader.acquire_model(context(), kModel, t::Permission::ModelChat),
                   t::ErrorCode::Unauthenticated),
        "task envelope rejected by model reader");

  std::filesystem::remove_all(directory);
  std::cout << "P05b-8 sealed delegation source matrix passed\n";
  return 0;
}
