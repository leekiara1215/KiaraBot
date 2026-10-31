#include "kiarabot/core/application/runtime_maintenance.hpp"

#include <algorithm>
#include <utility>

namespace kiarabot::core {
namespace {
constexpr std::chrono::hours kMinimumRetention{24};

std::size_t watermark(std::size_t limit) {
  return std::max<std::size_t>(1, limit - limit / 5);
}

bool expired(const NamespaceEntry& entry, RuntimeTimePoint now) {
  return entry.expires_at != RuntimeTimePoint{} && entry.expires_at <= now;
}

bool expired(const HostReceipt& receipt, RuntimeTimePoint now) {
  return receipt.expires_at != RuntimeTimePoint{} && receipt.expires_at <= now;
}

bool terminal(DeliveryState state) {
  return state == DeliveryState::acked || state == DeliveryState::dead_letter;
}
} // namespace

StorageResult<MaintenanceStats> RuntimeMaintenance::run(RuntimeTimePoint now) {
  if (now == RuntimeTimePoint{}) {
    now = std::chrono::system_clock::now();
  }
  const RuntimeTimePoint cutoff = now - std::max(options_.retention, kMinimumRetention);
  auto begun = store_->begin();
  if (const auto* failure = std::get_if<StorageError>(&begun)) {
    return *failure;
  }
  auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(begun));
  if (!transaction) {
    return StorageError::Unavailable;
  }
  RuntimeSnapshot& snapshot = transaction->snapshot();
  MaintenanceStats stats;

  // Terminal events carry their per-plugin deliveries; removing the event
  // removes the delivery records in the same commit.
  for (auto entry = snapshot.events.begin(); entry != snapshot.events.end();) {
    if (!terminal(entry->second.state) || entry->second.updated_at > cutoff) {
      ++entry;
      continue;
    }
    const auto deliveries = snapshot.deliveries.find(entry->first);
    if (deliveries != snapshot.deliveries.end()) {
      stats.deliveries += deliveries->second.size();
      snapshot.deliveries.erase(deliveries);
    }
    entry = snapshot.events.erase(entry);
    ++stats.events;
  }
  for (auto entry = snapshot.actions.begin(); entry != snapshot.actions.end();) {
    if (entry->second.state == ActionState::accepted || entry->second.updated_at > cutoff) {
      ++entry;
      continue;
    }
    entry = snapshot.actions.erase(entry);
    ++stats.actions;
  }

  // Audit is append-ordered; drop retention-expired records, then trim the
  // oldest records down to a watermark below the hard limit.
  const auto audit_before = snapshot.audit.size();
  std::erase_if(snapshot.audit,
                [&](const RuntimeAuditEntry& entry) { return entry.occurred_at <= cutoff; });
  const std::size_t audit_target = watermark(options_.audit_limit);
  if (snapshot.audit.size() > audit_target) {
    snapshot.audit.erase(snapshot.audit.begin(),
                         snapshot.audit.begin() +
                             static_cast<std::ptrdiff_t>(snapshot.audit.size() - audit_target));
  }
  stats.audit = audit_before - snapshot.audit.size();

  // Command idempotency records carry no timestamp (the snapshot format stores
  // only the fingerprint), so expiry is a deterministic count bound instead of
  // time based. Business idempotency is still enforced by the event, action and
  // Host-receipt records; evicting a command record only re-executes an
  // idempotent effect that re-appends one audit entry.
  std::size_t commands = 0;
  for (const auto& [subject, records] : snapshot.commands) {
    static_cast<void>(subject);
    commands += records.size();
  }
  const std::size_t command_target = watermark(options_.command_limit);
  if (commands > command_target) {
    std::vector<std::pair<std::string, std::string>> keys;
    keys.reserve(commands);
    for (const auto& [subject, records] : snapshot.commands) {
      for (const auto& [key, record] : records) {
        static_cast<void>(record);
        keys.emplace_back(subject, key);
      }
    }
    std::sort(keys.begin(), keys.end());
    const std::size_t remove = commands - command_target;
    for (std::size_t index = 0; index < remove; ++index) {
      auto records = snapshot.commands.find(keys[index].first);
      if (records == snapshot.commands.end()) {
        continue;
      }
      if (records->second.erase(keys[index].second) != 0) {
        ++stats.commands;
      }
      if (records->second.empty()) {
        snapshot.commands.erase(records);
      }
    }
  }

  for (auto& [plugin, entries] : snapshot.namespaces) {
    static_cast<void>(plugin);
    const std::size_t before = entries.size();
    std::erase_if(entries, [&](const auto& item) { return expired(item.second, now); });
    stats.namespace_entries += before - entries.size();
  }
  for (auto& [plugin, receipts] : snapshot.host_receipts) {
    static_cast<void>(plugin);
    const std::size_t before = receipts.size();
    std::erase_if(receipts, [&](const auto& item) { return expired(item.second, now); });
    stats.host_receipts += before - receipts.size();
  }

  if (!stats.changed()) {
    return stats; // Destruction rolls the transaction back; nothing was written.
  }
  const auto committed = transaction->commit();
  if (const auto* failure = std::get_if<StorageError>(&committed)) {
    return *failure;
  }
  return stats;
}
} // namespace kiarabot::core
