#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "kiarabot/core/domain/target/persistence.hpp"

namespace kiarabot::core::target {

// P09 storage/namespace limits are trusted application policy, never derived from requests.
inline constexpr std::size_t kMaximumNamespaceKeys = 1024;
inline constexpr std::size_t kMaximumNamespaceBytes = 1024 * 1024;
inline constexpr std::size_t kMaximumStorageValueBytes = 64 * 1024;
inline constexpr std::size_t kMaximumInstallationKeys = 8192;
inline constexpr std::size_t kMaximumInstallationBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kMaximumStorageKeyBytes = 256;
inline constexpr std::size_t kMaximumSharedMembers = 1024;
inline constexpr auto kMaximumStorageTtl = std::chrono::hours{24 * 30};
inline constexpr std::size_t kStorageReceiptInstanceCapacity = 10000;
inline constexpr std::size_t kStorageReceiptInstallationCapacity = 80000;

// A plugin never supplies an owner. The default scope is the authenticated instance's private
// namespace; installation_shared is resolved from that instance's installation by the server.
struct PrivateNamespace {
  InstanceId instance;
  bool operator==(const PrivateNamespace&) const = default;
};
struct SharedNamespace {
  InstallationId installation;
  ResourceId shared_namespace;
  bool operator==(const SharedNamespace&) const = default;
};
using NamespaceScope = std::variant<PrivateNamespace, SharedNamespace>;

struct NamespaceValue {
  std::vector<std::byte> bytes;
  ResourceVersion version;
  std::optional<TimePoint> expires_at;
};

struct ExpectedAbsent {};
using CompareCondition = std::variant<ExpectedAbsent, ResourceVersion>;

struct SharedNamespaceRecord {
  ResourceId namespace_id;
  InstallationId installation;
  ResourceVersion version;
  bool disabled = false;
  TimePoint created_at;
  TimePoint updated_at;
};

struct SharedMember {
  InstanceId instance;
  bool can_read = false;
  bool can_write = false;
  bool operator==(const SharedMember&) const = default;
};

struct SharedMemberRecord {
  SharedMember member;
  ResourceVersion version;
};

struct NamespaceQuota {
  std::size_t namespace_keys_used = 0;
  std::size_t namespace_bytes_used = 0;
  std::size_t installation_keys_used = 0;
  std::size_t installation_bytes_used = 0;
};

// Typed immutable receipt result: storage bytes never enter a replay receipt (only versions and
// expiry), management receipts carry the public namespace projection.
struct StorageMutationReceipt {
  NamespaceScope scope;
  std::string key;
  bool deleted = false;
  std::optional<ResourceVersion> version;
  std::optional<TimePoint> expires_at;
  // CAS replay returns the original entries; private/shared values are never linkable to audit.
  std::vector<std::byte> value;
};
struct SharedNamespaceReceipt {
  SharedNamespaceRecord namespace_record;
};
struct SharedMembersReceipt {
  ResourceVersion version;
};
using NamespaceReceiptResult =
    std::variant<StorageMutationReceipt, SharedNamespaceReceipt, SharedMembersReceipt>;
struct NamespaceReceipt {
  ReceiptKey key;
  Sha256Digest fingerprint;
  NamespaceReceiptResult result;
  InstanceId instance;
  InstallationId installation;
  TimePoint created_at;
  TimePoint retain_until;
};
struct NamespaceReceiptCapacity {
  std::size_t instance_max = kStorageReceiptInstanceCapacity;
  std::size_t installation_max = kStorageReceiptInstallationCapacity;
};
struct NamespaceReceiptWrite {
  NamespaceReceipt receipt;
  bool duplicate = false;
};

inline bool valid_storage_key(std::string_view key) {
  return !key.empty() && key.size() <= kMaximumStorageKeyBytes &&
         key.find('\0') == std::string_view::npos;
}
inline bool valid_storage_value(std::size_t bytes) {
  return bytes <= kMaximumStorageValueBytes;
}
// Absolute UTC TTL: strictly in the future and at most 30 days ahead. Optional is valid.
inline bool valid_storage_expiry(std::optional<TimePoint> expires_at, TimePoint now) {
  if (!expires_at) {
    return true;
  }
  return *expires_at > now && *expires_at <= now + kMaximumStorageTtl;
}
inline std::size_t namespace_bytes(const std::vector<NamespaceValue>& values) {
  std::size_t total = 0;
  for (const auto& value : values) {
    total += value.bytes.size();
  }
  return total;
}
// True when adding/overwriting `replacement_bytes` in a namespace that currently holds
// `current_keys`/`current_bytes` stays within both the namespace and installation aggregates.
inline bool within_storage_quota(std::size_t current_keys, std::size_t current_bytes,
                                 std::size_t replacement_bytes, bool replacing) {
  const auto keys = current_keys + (replacing ? 0U : 1U);
  const auto bytes = current_bytes + replacement_bytes;
  return keys <= kMaximumNamespaceKeys && bytes <= kMaximumNamespaceBytes;
}
inline bool within_installation_quota(std::size_t installation_keys, std::size_t installation_bytes,
                                      std::size_t replacement_keys, std::size_t replacement_bytes,
                                      bool replacing) {
  const auto keys = installation_keys + (replacing ? 0U : replacement_keys);
  const auto bytes = installation_bytes + replacement_bytes;
  return keys <= kMaximumInstallationKeys && bytes <= kMaximumInstallationBytes;
}
// Private Set must omit the condition (last-writer-wins). Shared writes always require one:
// absent=create, version=update/delete. Delete of an absent key succeeds for the private LWW
// path; a shared delete needs the current version.
inline bool valid_private_set_condition(bool has_condition) {
  return !has_condition;
}
inline bool valid_shared_set_condition(bool has_condition) {
  return has_condition;
}
inline bool valid_shared_delete_condition(bool has_condition) {
  return has_condition;
}
inline bool valid_cas_condition(bool has_condition) {
  return has_condition;
}

// Converts the public wire expectation into the repository compare condition for the observed
// current row. Absence plus an expected version is a conflict (nothing to update).
inline std::optional<CompareCondition>
storage_compare_condition(bool has_condition, const CompareCondition& condition,
                          const std::optional<NamespaceValue>& current) {
  if (!current) {
    if (has_condition && std::holds_alternative<ResourceVersion>(condition)) {
      return std::nullopt;
    }
    return ExpectedAbsent{};
  }
  if (has_condition) {
    const auto* expected = std::get_if<ResourceVersion>(&condition);
    if (!expected || *expected != current->version) {
      return std::nullopt;
    }
    return current->version;
  }
  // Private LWW delete/set: compare against the observed current version so a concurrent writer
  // cannot be silently overwritten by a stale read.
  return current->version;
}
inline bool expired(const NamespaceValue& value, TimePoint now) {
  return value.expires_at.has_value() && now >= *value.expires_at;
}
inline bool valid_shared_member(const SharedMember& member) {
  return !member.instance.value.empty() && (member.can_read || member.can_write);
}
inline bool valid_shared_members(std::span<const SharedMember> members) {
  if (members.size() > kMaximumSharedMembers) {
    return false;
  }
  std::vector<InstanceId> seen;
  for (const auto& member : members) {
    if (!valid_shared_member(member)) {
      return false;
    }
    for (const auto& known : seen) {
      if (known == member.instance) {
        return false;
      }
    }
    seen.push_back(member.instance);
  }
  return true;
}
inline bool member_can_read(std::span<const SharedMemberRecord> members,
                            const InstanceId& instance) {
  for (const auto& member : members) {
    if (member.member.instance == instance) {
      return member.member.can_read;
    }
  }
  return false;
}
inline bool member_can_write(std::span<const SharedMemberRecord> members,
                             const InstanceId& instance) {
  for (const auto& member : members) {
    if (member.member.instance == instance) {
      return member.member.can_write;
    }
  }
  return false;
}

} // namespace kiarabot::core::target
