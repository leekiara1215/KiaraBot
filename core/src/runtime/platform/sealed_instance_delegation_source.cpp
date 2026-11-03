#include "kiarabot/core/runtime/platform/sealed_instance_delegation_source.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unistd.h>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <sys/stat.h>

#include "kiarabot/core/domain/target/identity_permissions.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/platform.pb.h"

namespace kiarabot::core::platform {
namespace {
namespace t = target;
using Json = nlohmann::json;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
t::Failure unauthenticated() {
  return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
}
constexpr std::size_t kMaximumFile = 64 * 1024;
constexpr std::int64_t kMaximumTtlSeconds = 56;
bool canonical_uuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
      continue;
    }
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}
bool regular_private_file(const std::string& path) {
  struct stat status {};
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    return false;
  }
  const bool valid = ::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
                     (status.st_mode & 07777) == 0600 && status.st_uid == ::geteuid();
  ::close(descriptor);
  return valid;
}
std::optional<std::string> base64url_decode(std::string_view input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::array<std::int16_t, 256> table{};
  table.fill(-1);
  for (std::size_t index = 0; index < 64; ++index) {
    table[static_cast<unsigned char>(kAlphabet[index])] = static_cast<std::int16_t>(index);
  }
  std::string output;
  output.reserve(input.size() * 3 / 4 + 3);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char character : input) {
    const auto value = table[static_cast<unsigned char>(character)];
    if (value < 0) {
      return std::nullopt;
    }
    buffer = (buffer << 6U) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output += static_cast<char>((buffer >> static_cast<unsigned>(bits)) & 0xffU);
    }
  }
  return output;
}
struct ProofClaims {
  std::int64_t issued_at = 0;
  std::int64_t expires_at = 0;
};
std::optional<ProofClaims> decode_proof(std::string_view token) {
  if (token.size() > 32768 || token.find('.') == std::string_view::npos) {
    return std::nullopt;
  }
  const auto first = token.find('.');
  const auto second = token.find('.', first + 1);
  if (first == std::string_view::npos || second == std::string_view::npos ||
      token.find('.', second + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  const auto payload = base64url_decode(token.substr(first + 1, second - first - 1));
  if (!payload) {
    return std::nullopt;
  }
  const auto document = Json::parse(*payload, nullptr, false);
  if (document.is_discarded() || !document.is_object() || !document.contains("aud") ||
      !document.contains("iat") || !document.contains("exp") || !document.contains("sub") ||
      !document["aud"].is_string() || document["aud"].get<std::string>() != "runtime" ||
      !document["iat"].is_number_integer() || !document["exp"].is_number_integer() ||
      !document["sub"].is_string() || document["sub"].get<std::string>().empty()) {
    return std::nullopt;
  }
  ProofClaims claims;
  claims.issued_at = document["iat"].get<std::int64_t>();
  claims.expires_at = document["exp"].get<std::int64_t>();
  return claims;
}
std::optional<t::Permission> permission_from_name(std::string_view name) {
  for (const auto& [candidate, value] : t::kPermissionNames) {
    if (candidate == name) {
      return value;
    }
  }
  return std::nullopt;
}
bool task_permission(t::Permission permission) {
  return permission == t::Permission::TaskSchedule || permission == t::Permission::TaskRead;
}
bool model_permission(t::Permission permission) {
  return permission == t::Permission::ModelChat || permission == t::Permission::ModelCallInspect;
}
std::optional<t::ResourceRef> resource_from_json(const Json& value) {
  if (!value.is_object() || value.size() != 2 || !value.contains("kind") || !value.contains("id") ||
      !value["kind"].is_string() || !value["id"].is_string()) {
    return std::nullopt;
  }
  const auto kind_name = value["kind"].get<std::string>();
  const auto id = value["id"].get<std::string>();
  if (!canonical_uuid(id)) {
    return std::nullopt;
  }
  v1::ResourceKind wire_kind = v1::RESOURCE_KIND_UNSPECIFIED;
  if (!v1::ResourceKind_Parse(kind_name, &wire_kind)) {
    return std::nullopt;
  }
  const auto decoded = target_codec::from_wire(wire_kind);
  if (const auto* kind = std::get_if<t::ResourceKind>(&decoded)) {
    return t::ResourceRef{*kind, t::ResourceId{id}};
  }
  return std::nullopt;
}
} // namespace

namespace {
std::string derive_model_path(const std::string& path) {
  static constexpr std::string_view kSuffix = ".json";
  if (path.size() > kSuffix.size() &&
      std::string_view{path}.substr(path.size() - kSuffix.size()) == kSuffix) {
    return path.substr(0, path.size() - kSuffix.size()) + "-model" + std::string{kSuffix};
  }
  return path + "-model";
}
std::optional<t::ResourceKind> kind_from_json(const Json& value) {
  if (!value.is_object() || value.size() != 2 || !value.contains("kind") || !value.contains("id") ||
      !value["kind"].is_string() || !value["id"].is_string()) {
    return std::nullopt;
  }
  v1::ResourceKind wire_kind = v1::RESOURCE_KIND_UNSPECIFIED;
  if (!v1::ResourceKind_Parse(value["kind"].get<std::string>(), &wire_kind)) {
    return std::nullopt;
  }
  const auto decoded = target_codec::from_wire(wire_kind);
  if (const auto* kind = std::get_if<t::ResourceKind>(&decoded)) {
    return *kind;
  }
  return std::nullopt;
}
} // namespace

SealedInstanceDelegationSource::SealedInstanceDelegationSource(
    SealedInstanceDelegationSourceOptions options)
    : path_(std::move(options.path)),
      model_path_(options.model_path.empty() ? derive_model_path(path_)
                                             : std::move(options.model_path)) {}

SealedInstanceDelegationSource::~SealedInstanceDelegationSource() = default;

target::Outcome<std::unique_ptr<SealedInstanceDelegationSource>>
SealedInstanceDelegationSource::create(SealedInstanceDelegationSourceOptions options) {
  if (options.path.empty() || options.path.size() > 4096 || options.model_path.size() > 4096) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  try {
    return std::unique_ptr<SealedInstanceDelegationSource>(
        new SealedInstanceDelegationSource(std::move(options)));
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
  } catch (...) {
    return failure(t::ErrorCode::Internal);
  }
}

target::Outcome<InstanceParentDelegation>
read_sealed_delegation(const std::string& path, bool model_kind,
                       const t::SchedulerTaskContext& context, std::string_view model_id,
                       t::Permission permission) {
  try {
    if (context.owner.value.empty() || context.installation.value.empty() ||
        (model_kind ? !model_permission(permission) : !task_permission(permission)) ||
        !regular_private_file(path)) {
      return unauthenticated();
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return unauthenticated();
    }
    std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (raw.empty() || raw.size() > kMaximumFile) {
      return unauthenticated();
    }
    const auto document = Json::parse(raw, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
      return unauthenticated();
    }
    const std::set<std::string> required{
        "schema_version", "instance_id", "installation_id", "audience",    "permissions",
        "scope",          "issued_at",   "expires_at",      "ttl_seconds", "delegation_proof"};
    std::set<std::string> actual;
    for (const auto& [key, value] : document.items()) {
      (void)value;
      actual.insert(key);
    }
    if (actual != required || document["schema_version"] != 1 ||
        !document["instance_id"].is_string() || !document["installation_id"].is_string() ||
        !document["audience"].is_string() || !document["permissions"].is_array() ||
        !document["scope"].is_array() || !document["issued_at"].is_number_integer() ||
        !document["expires_at"].is_number_integer() ||
        !document["ttl_seconds"].is_number_integer() || !document["delegation_proof"].is_string()) {
      return unauthenticated();
    }
    const auto instance = document["instance_id"].get<std::string>();
    const auto installation = document["installation_id"].get<std::string>();
    if (!canonical_uuid(instance) || !canonical_uuid(installation) ||
        instance != context.owner.value || installation != context.installation.value ||
        document["audience"].get<std::string>() != "runtime") {
      return unauthenticated();
    }
    std::vector<t::Permission> permissions;
    bool has_task = false;
    bool has_model = false;
    for (const auto& entry : document["permissions"]) {
      if (!entry.is_string()) {
        return unauthenticated();
      }
      const auto decoded = permission_from_name(entry.get<std::string>());
      if (!decoded || (!task_permission(*decoded) && !model_permission(*decoded))) {
        return unauthenticated();
      }
      if (std::find(permissions.begin(), permissions.end(), *decoded) != permissions.end()) {
        return unauthenticated();
      }
      permissions.push_back(*decoded);
      has_task = has_task || task_permission(*decoded);
      has_model = has_model || model_permission(*decoded);
    }
    if ((has_task && has_model) || (model_kind && !has_model) || (!model_kind && !has_task) ||
        std::find(permissions.begin(), permissions.end(), permission) == permissions.end()) {
      return unauthenticated();
    }
    bool has_instance = false;
    bool has_installation = false;
    bool has_model_scope = false;
    std::vector<t::ResourceRef> scope;
    for (const auto& entry : document["scope"]) {
      const auto decoded = resource_from_json(entry);
      if (!decoded) {
        return unauthenticated();
      }
      if (decoded->kind == t::ResourceKind::Instance && decoded->id.value != instance) {
        return unauthenticated();
      }
      if (decoded->kind == t::ResourceKind::Installation && decoded->id.value != installation) {
        return unauthenticated();
      }
      if (decoded->kind == t::ResourceKind::Model) {
        has_model_scope = true;
        if (!model_kind || decoded->id.value != model_id) {
          return unauthenticated();
        }
      } else if (decoded->kind != t::ResourceKind::Instance &&
                 decoded->kind != t::ResourceKind::Installation) {
        return unauthenticated();
      }
      if (std::find(scope.begin(), scope.end(), *decoded) != scope.end()) {
        return unauthenticated();
      }
      scope.push_back(*decoded);
      has_instance = has_instance || decoded->kind == t::ResourceKind::Instance;
      has_installation = has_installation || decoded->kind == t::ResourceKind::Installation;
    }
    if (model_kind) {
      if (!has_model_scope || scope.size() != 1) {
        return unauthenticated();
      }
    } else if (!has_instance || scope.size() > 2 || !has_installation && scope.size() > 1) {
      // Task envelope requires INSTANCE; INSTALLATION is optional but, when present, must match.
      if (!has_instance) {
        return unauthenticated();
      }
    }
    if (model_kind) {
      if (model_id.empty() || !canonical_uuid(model_id)) {
        return unauthenticated();
      }
    }
    const auto issued = document["issued_at"].get<std::int64_t>();
    const auto expires = document["expires_at"].get<std::int64_t>();
    const auto ttl = document["ttl_seconds"].get<std::int64_t>();
    if (ttl != expires - issued || ttl <= 0 || ttl > kMaximumTtlSeconds) {
      return unauthenticated();
    }
    const auto proof = document["delegation_proof"].get<std::string>();
    const auto claims = decode_proof(proof);
    if (!claims || claims->issued_at != issued || claims->expires_at != expires) {
      return unauthenticated();
    }
    const auto now = delegation_provider_now();
    const auto issued_at = t::TimePoint{std::chrono::seconds{issued}};
    const auto expires_at = t::TimePoint{std::chrono::seconds{expires}};
    if (expires_at <= now || issued_at > now + std::chrono::seconds{2}) {
      return unauthenticated();
    }
    for (const unsigned char character : proof) {
      if (character < 33 || character > 126) {
        return unauthenticated();
      }
    }
    return InstanceParentDelegation{proof, expires_at};
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
  } catch (...) {
    return unauthenticated();
  }
}

target::Outcome<InstanceParentDelegation>
SealedInstanceDelegationSource::acquire(const t::SchedulerTaskContext& context,
                                        t::Permission permission) {
  return read_sealed_delegation(path_, false, context, {}, permission);
}

target::Outcome<InstanceParentDelegation>
SealedInstanceDelegationSource::acquire_model(const t::SchedulerTaskContext& context,
                                              std::string_view model_id, t::Permission permission) {
  return read_sealed_delegation(model_path_, true, context, model_id, permission);
}

} // namespace kiarabot::core::platform
