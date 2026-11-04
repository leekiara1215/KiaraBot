#pragma once

#include <algorithm>
#include <charconv>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

#include "common.hpp"

namespace kiarabot::core::target::runtime_resources_detail {

class SnapshotPages {
public:
  SnapshotPages(RuntimeIdPort& ids, RuntimeAuthorizationPort& authorization)
      : ids_(ids), authorization_(authorization) {}

private:
  // A stored row set is immutable. Tokens name a snapshot plus an immutable candidate offset;
  // authorization is rechecked on every page and reusable proof bytes are never retained.
  struct ResourceSnapshot {
    std::string id;
    SubjectId subject;
    std::string query;
    std::string first_request_id;
    std::string first_page_key;
    std::vector<std::string> scope;
    ResourceVersion authorization_version;
    std::uint32_t page_size;
    TimePoint observed_at;
    TimePoint expires_at;
    std::vector<ReplayResult> records;
    std::size_t bytes = 0;
  };
  static constexpr std::size_t kMaximumSnapshots = 1024;
  static constexpr std::size_t kMaximumSubjectSnapshots = 64;
  static constexpr std::size_t kMaximumSnapshotRecords = 4096;
  static constexpr std::size_t kMaximumTotalSnapshotRecords = 16384;
  static constexpr std::size_t kMaximumSnapshotBytes = 64 * 1024 * 1024;

  static std::vector<std::string> snapshot_scope(const AuthenticationContext& authentication) {
    std::vector<std::string> scope;
    for (const auto& item : authentication.scopes) {
      scope.push_back(std::to_string(static_cast<int>(item.resource.kind)) + "/" +
                      item.resource.id.value + "/" + item.permission);
    }
    std::sort(scope.begin(), scope.end());
    scope.erase(std::unique(scope.begin(), scope.end()), scope.end());
    return scope;
  }
  // Conservative upper bounds include protobuf tags/length prefixes and all dynamic payload.
  static std::size_t snapshot_bytes(const BotAccount& value) {
    return 512 + 2 * (value.id.value.size() + value.platform.size() +
                      value.platform_account_id.size() + value.display_name.size());
  }
  static std::size_t snapshot_bytes(const PluginInstallation& value) {
    return 512 +
           2 * (value.id.value.size() + value.plugin_type.value.size() + value.manifest.size());
  }
  static std::size_t snapshot_bytes(const PluginInstance& value) {
    std::size_t size = 2048 + 2 * (value.id.value.size() + value.installation.value.size() +
                                   value.plugin_type.value.size());
    if (value.host) {
      size += 2 * value.host->value.size();
    }
    if (value.current) {
      size +=
          2 * (value.current->instance.value.size() + value.current->runtime_epoch.value.size() +
               value.current->connection.value.size() + value.current->execution.value.size());
    }
    return size;
  }
  static std::size_t snapshot_bytes(const AdapterRecord& value) {
    std::size_t size =
        2048 +
        2 * (value.adapter.value.size() + value.registration.implementation.size() +
             value.registration.implementation_version.size() +
             value.registration.protocol_version.size() + value.current.instance.value.size() +
             value.current.runtime_epoch.value.size() + value.current.connection.value.size());
    for (const auto& capability : value.registration.declared_capabilities) {
      size += 32 + 2 * (capability.name.size() + capability.version.size());
    }
    for (const auto& capability : value.registration.effective_capabilities) {
      size += 32 + 2 * (capability.name.size() + capability.version.size());
    }
    return size;
  }

public:
  template <typename Value>
  Outcome<ListResult<Value>>
  list_page(RuntimeUnitOfWork& work, const CommandContext& context, const PageRequest& page,
            std::string_view query, const ResourceVersion& authorization_version, TimePoint now,
            Permission permission,
            const std::function<Outcome<std::vector<Value>>(RuntimeUnitOfWork&, std::size_t,
                                                            const std::string&)>& loader,
            const std::function<ResourceRef(const Value&)>& resource_of,
            const std::function<const std::string&(const Value&)>& id) {
    std::shared_ptr<const ResourceSnapshot> snapshot;
    std::size_t offset = 0;
    const auto scope = snapshot_scope(context.authentication);
    const auto first_page_key =
        context.authentication.subject.value + "|" + context.method + "|" + context.request_id;
    const auto matches = [&](const ResourceSnapshot& value) {
      return value.expires_at > now && value.subject == context.authentication.subject &&
             value.query == query && value.authorization_version == authorization_version &&
             value.page_size == page.page_size && value.scope == scope;
    };
    if (page.page_token.empty()) {
      std::lock_guard lock(cursors_mutex_);
      const auto replay = first_pages_.find(first_page_key);
      if (replay != first_pages_.end()) {
        const auto stored = snapshots_.find(replay->second);
        if (stored == snapshots_.end() || !matches(*stored->second)) {
          return stale_version();
        }
        snapshot = stored->second;
      }
    }
    if (!page.page_token.empty()) {
      const auto separator = page.page_token.find('.');
      if (separator == std::string::npos) {
        return stale_version();
      }
      const auto position = std::string_view(page.page_token).substr(separator + 1);
      auto parsed = std::from_chars(position.data(), position.data() + position.size(), offset);
      if (parsed.ec != std::errc{} || parsed.ptr != position.data() + position.size() ||
          position != std::to_string(offset)) {
        return stale_version();
      }
      std::lock_guard lock(cursors_mutex_);
      const auto found = snapshots_.find(page.page_token.substr(0, separator));
      if (found == snapshots_.end() || found->second->expires_at <= now ||
          found->second->subject != context.authentication.subject ||
          found->second->query != query ||
          found->second->authorization_version != authorization_version ||
          found->second->page_size != page.page_size || found->second->scope != scope ||
          offset >= found->second->records.size() || offset == 0 ||
          !issued_offsets_[found->first].contains(offset)) {
        return stale_version();
      }
      snapshot = found->second;
    } else if (!snapshot) {
      auto allocated = ids_.snapshot_id();
      if (const auto* rejected = std::get_if<Failure>(&allocated)) {
        return *rejected;
      }
      auto building =
          std::make_shared<ResourceSnapshot>(ResourceSnapshot{std::get<std::string>(allocated),
                                                              context.authentication.subject,
                                                              std::string(query),
                                                              context.request_id,
                                                              first_page_key,
                                                              scope,
                                                              authorization_version,
                                                              page.page_size,
                                                              now,
                                                              now + kSnapshotLifetime,
                                                              {},
                                                              0});
      building->bytes = 1024 + 2 * (building->id.size() + building->subject.value.size() +
                                    building->query.size() + building->first_page_key.size() +
                                    building->first_request_id.size());
      for (const auto& item : building->scope) {
        building->bytes += 32 + 2 * item.size();
      }
      std::string after;
      for (;;) {
        auto loaded = loader(work, 1024, after);
        if (const auto* rejected = std::get_if<Failure>(&loaded)) {
          return *rejected;
        }
        auto values = std::move(std::get<std::vector<Value>>(loaded));
        for (auto& value : values) {
          if (id(value) <= after) {
            return internal_failure();
          }
          after = id(value);
          building->bytes += snapshot_bytes(value);
          building->records.emplace_back(std::move(value));
          if (building->records.size() > kMaximumSnapshotRecords ||
              building->bytes > kMaximumSnapshotBytes) {
            return failure(ErrorCode::RateLimited, Recovery::Backoff);
          }
        }
        if (values.size() < 1024) {
          break;
        }
      }
      std::lock_guard lock(cursors_mutex_);
      // A simultaneous first-page retry may already have published the same immutable view.
      const auto replay = first_pages_.find(first_page_key);
      if (replay != first_pages_.end()) {
        const auto stored = snapshots_.find(replay->second);
        if (stored == snapshots_.end() || !matches(*stored->second)) {
          return stale_version();
        }
        snapshot = stored->second;
      } else {
        std::size_t count = 0;
        std::size_t records = building->records.size();
        std::size_t bytes = building->bytes;
        for (auto it = snapshots_.begin(); it != snapshots_.end();) {
          // An in-flight reader still retains these encoded bytes; reclaim only after it releases.
          if (it->second->expires_at <= now && it->second.use_count() == 1) {
            first_pages_.erase(it->second->first_page_key);
            issued_offsets_.erase(it->first);
            it = snapshots_.erase(it);
          } else {
            count += it->second->subject == building->subject;
            records += it->second->records.size();
            bytes += it->second->bytes;
            ++it;
          }
        }
        if (count >= kMaximumSubjectSnapshots || snapshots_.size() >= kMaximumSnapshots ||
            records > kMaximumTotalSnapshotRecords || bytes > kMaximumSnapshotBytes) {
          return failure(ErrorCode::RateLimited, Recovery::Backoff);
        }
        if (!snapshots_.emplace(building->id, building).second) {
          return internal_failure();
        }
        first_pages_.emplace(first_page_key, building->id);
        snapshot = building;
      }
    }
    // Release the consistent database view before online per-child Identity checks.
    work.rollback();
    ListResult<Value> result;
    result.page.page_size = page.page_size;
    result.snapshot.snapshot_id = snapshot->id;
    result.snapshot.authorization_version = authorization_version;
    result.snapshot.observed_at = snapshot->observed_at;
    result.snapshot.expires_at = snapshot->expires_at;
    for (std::size_t index = offset; index < snapshot->records.size(); ++index) {
      const auto* value = std::get_if<Value>(&snapshot->records[index]);
      if (!value) {
        return internal_failure();
      }
      auto allowed = authorize(authorization_, context.authentication, permission, context.method,
                               {resource_of(*value)}, context.deadline);
      if (const auto* rejected = std::get_if<Failure>(&allowed)) {
        if (rejected->code == ErrorCode::PermissionDenied ||
            rejected->code == ErrorCode::NotFound) {
          continue;
        }
        return *rejected;
      }
      if (std::get<RuntimeAuthorizationDecision>(allowed).authorization_version !=
          authorization_version) {
        return stale_version();
      }
      if (result.items.size() == page.page_size) {
        {
          std::lock_guard lock(cursors_mutex_);
          if (!snapshots_.contains(snapshot->id)) {
            return stale_version();
          }
          issued_offsets_[snapshot->id].insert(index);
        }
        result.page.next_page_token = snapshot->id + "." + std::to_string(index);
        break;
      }
      result.items.push_back(*value);
    }
    return result;
  }

private:
  RuntimeIdPort& ids_;
  RuntimeAuthorizationPort& authorization_;
  std::mutex cursors_mutex_;
  std::unordered_map<std::string, std::shared_ptr<const ResourceSnapshot>> snapshots_;
  std::unordered_map<std::string, std::set<std::size_t>> issued_offsets_;
  std::unordered_map<std::string, std::string> first_pages_;
};

} // namespace kiarabot::core::target::runtime_resources_detail
