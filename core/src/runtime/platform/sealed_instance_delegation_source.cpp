#include "kiarabot/core/runtime/platform/sealed_instance_delegation_source.hpp"

#include <cerrno>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
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
constexpr std::size_t kMaximumFile = 96 * 1024;
std::string trim_line(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}
std::optional<t::TimePoint> parse_time(const Json& value) {
  std::int64_t seconds = 0;
  const auto now_seconds = [](const char* text) -> std::optional<std::int64_t> {
    if (text == nullptr) {
      return std::nullopt;
    }
    std::tm tm{};
    if (strptime(text, "%Y-%m-%dT%H:%M:%SZ", &tm) == nullptr &&
        strptime(text, "%Y-%m-%dT%H:%M:%S", &tm) == nullptr) {
      return std::nullopt;
    }
    const auto result = timegm(&tm);
    if (result < 0) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(result);
  };
  if (value.is_number_integer()) {
    if (value.get<std::int64_t>() < 0) {
      return std::nullopt;
    }
    seconds = value.get<std::int64_t>();
  } else if (value.is_string()) {
    const auto parsed = now_seconds(value.get<std::string>().c_str());
    if (!parsed) {
      return std::nullopt;
    }
    seconds = *parsed;
  } else {
    return std::nullopt;
  }
  return t::TimePoint{std::chrono::seconds{seconds}};
}
std::optional<t::Permission> permission_from_name(std::string_view name) {
  for (const auto& [candidate, value] : t::kPermissionNames) {
    if (candidate == name) {
      return value;
    }
  }
  return std::nullopt;
}
std::optional<t::ResourceRef> resource_from_json(const Json& value) {
  if (!value.is_object() || !value.contains("kind") || !value.contains("id") ||
      !value["kind"].is_string() || !value["id"].is_string()) {
    return std::nullopt;
  }
  const auto kind_name = value["kind"].get<std::string>();
  const auto id = value["id"].get<std::string>();
  if (id.empty() || id.size() > 256) {
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
bool regular_private_file(const std::string& path) {
  struct stat status {};
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    return false;
  }
  const bool valid = ::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
                     (status.st_mode & 077) == 0 && status.st_uid == ::geteuid();
  ::close(descriptor);
  return valid;
}
} // namespace

SealedInstanceDelegationSource::SealedInstanceDelegationSource(
    SealedInstanceDelegationSourceOptions options)
    : path_(std::move(options.path)) {}

SealedInstanceDelegationSource::~SealedInstanceDelegationSource() = default;

target::Outcome<std::unique_ptr<SealedInstanceDelegationSource>>
SealedInstanceDelegationSource::create(SealedInstanceDelegationSourceOptions options) {
  if (options.path.empty() || options.path.size() > 4096) {
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
SealedInstanceDelegationSource::acquire(const t::SchedulerTaskContext& context,
                                        t::Permission permission) {
  try {
    if (context.owner.value.empty() || context.installation.value.empty() ||
        (permission != t::Permission::TaskSchedule && permission != t::Permission::TaskRead) ||
        !regular_private_file(path_)) {
      return unauthenticated();
    }
    std::ifstream input(path_, std::ios::binary);
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
    const auto text = [&](const char* name) -> std::optional<std::string> {
      if (!document.contains(name) || !document[name].is_string()) {
        return std::nullopt;
      }
      auto value = document[name].get<std::string>();
      if (value.empty() || value.size() > 4096) {
        return std::nullopt;
      }
      return value;
    };
    const auto instance = text("instance_id");
    const auto installation = text("installation_id");
    const auto audience = text("audience");
    const auto token = text("token");
    if (!instance || !installation || !audience || !token || *instance != context.owner.value ||
        *installation != context.installation.value || *audience != "runtime") {
      return unauthenticated();
    }
    if (!document.contains("permissions") || !document["permissions"].is_array() ||
        document["permissions"].empty() || document["permissions"].size() > 8 ||
        !document.contains("scope") || !document["scope"].is_array() || document["scope"].empty() ||
        document["scope"].size() > 8 || !document.contains("issued_at") ||
        !document.contains("expires_at")) {
      return unauthenticated();
    }
    bool permission_allowed = false;
    for (const auto& entry : document["permissions"]) {
      if (!entry.is_string()) {
        return unauthenticated();
      }
      const auto decoded = permission_from_name(entry.get<std::string>());
      if (!decoded) {
        return unauthenticated();
      }
      if (*decoded == permission) {
        permission_allowed = true;
      }
    }
    if (!permission_allowed) {
      return unauthenticated();
    }
    bool instance_scope = false;
    bool installation_scope = false;
    for (const auto& entry : document["scope"]) {
      const auto decoded = resource_from_json(entry);
      if (!decoded) {
        return unauthenticated();
      }
      if (decoded->kind == t::ResourceKind::Instance && decoded->id.value != context.owner.value) {
        return unauthenticated();
      }
      if (decoded->kind == t::ResourceKind::Installation &&
          decoded->id.value != context.installation.value) {
        return unauthenticated();
      }
      if (decoded->kind == t::ResourceKind::Instance) {
        instance_scope = true;
      }
      if (decoded->kind == t::ResourceKind::Installation) {
        installation_scope = true;
      }
    }
    if (!instance_scope || !installation_scope) {
      return unauthenticated();
    }
    const auto issued = parse_time(document["issued_at"]);
    const auto expires = parse_time(document["expires_at"]);
    if (!issued || !expires) {
      return unauthenticated();
    }
    const auto now = delegation_provider_now();
    const auto lifetime = *expires - *issued;
    if (lifetime <= std::chrono::seconds::zero() || lifetime > std::chrono::seconds{56} ||
        *issued > now + std::chrono::seconds{2} || *expires <= now) {
      return unauthenticated();
    }
    if (token->size() > 32768) {
      return unauthenticated();
    }
    for (const unsigned char character : *token) {
      if (character < 33 || character > 126) {
        return unauthenticated();
      }
    }
    return InstanceParentDelegation{*token, *expires};
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
  } catch (...) {
    return unauthenticated();
  }
}

} // namespace kiarabot::core::platform
