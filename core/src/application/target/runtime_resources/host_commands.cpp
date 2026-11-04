#include <algorithm>
#include <set>

#include "common.hpp"
#include "components.hpp"
#include "kiarabot/core/domain/target/event_delivery.hpp"
#include "kiarabot/core/domain/target/host_command_records.hpp"

namespace kiarabot::core::target::runtime_resources_detail {
namespace {
// P08b-3a trusted transport persistence: no user authorization, one short write UoW per call.
// The caller (internal mTLS HostBridge server) has already verified the peer certificate/fence.
template <typename Value, typename Function>
Outcome<Value> with_host_work(RuntimeUnitOfWorkFactory& transactions, Function function) {
  try {
    auto begun = transactions.begin();
    if (const auto* failure = std::get_if<Failure>(&begun)) {
      return *failure;
    }
    auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
    if (!work) {
      return internal_failure();
    }
    auto now = work->current_time();
    if (const auto* failure = std::get_if<Failure>(&now)) {
      work->rollback();
      return *failure;
    }
    auto result = function(*work, std::get<TimePoint>(now));
    if (const auto* failure = std::get_if<Failure>(&result)) {
      work->rollback();
      return *failure;
    }
    if (work->commit() != CommitResult::Committed) {
      return unavailable();
    }
    return std::move(std::get<Value>(result));
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return internal_failure();
  }
}

class RuntimeHostCommandApplicationImpl final : public RuntimeHostCommandApplication {
public:
  RuntimeHostCommandApplicationImpl(const RuntimeResourceDependencies& dependencies)
      : transactions_(dependencies.transactions) {}

  Outcome<std::vector<HostCommandOutboxRecord>>
  host_pending_commands(const std::string& host_id, std::uint64_t connection_generation,
                        std::size_t limit) override {
    if (host_id.empty() || host_id.size() > 128 || connection_generation == 0 || limit == 0 ||
        limit > 100 || host_id.find('\0') != std::string::npos) {
      return invalid_runtime_input();
    }
    return with_host_work<std::vector<HostCommandOutboxRecord>>(
        transactions_, [&](RuntimeUnitOfWork& work, TimePoint) {
          return work.host_commands().pending(host_id, connection_generation, limit);
        });
  }

  Status host_command_dispatched(const std::string& command_id, std::uint64_t connection_generation,
                                 std::string_view runtime_epoch) override {
    if (command_id.empty() || connection_generation == 0 || runtime_epoch.empty()) {
      return invalid_runtime_input();
    }
    return with_host_work<std::monostate>(
        transactions_, [&](RuntimeUnitOfWork& work, TimePoint now) -> Status {
          return work.host_commands().mark_dispatched(command_id, connection_generation,
                                                      runtime_epoch, now);
        });
  }

  Status host_command_receipt(const std::string& command_id, std::uint64_t connection_generation,
                              bool persisted, std::optional<ExecutionOutcome> outcome,
                              std::optional<std::string> process_marker) override {
    if (command_id.empty() || connection_generation == 0 ||
        (process_marker && process_marker->size() > 256)) {
      return invalid_runtime_input();
    }
    return with_host_work<std::monostate>(
        transactions_, [&](RuntimeUnitOfWork& work, TimePoint now) -> Status {
          auto found = work.host_commands().find(command_id);
          if (const auto* failure = std::get_if<Failure>(&found)) {
            return *failure;
          }
          const auto& record = std::get<std::optional<HostCommandOutboxRecord>>(found);
          if (!record) {
            return failure(ErrorCode::NotFound, Recovery::ReadCurrent);
          }
          // A receipt from an old/replaced connection generation must never finalize a command
          // that a newer Host connection is responsible for.
          if (record->connection_generation != connection_generation) {
            return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
          }
          if (record->state == HostCommandDeliveryState::Receipted) {
            return record->receipt_persisted == persisted && record->receipt_outcome == outcome &&
                           record->receipt_process_marker == process_marker
                       ? Status{std::monostate{}}
                       : Status{failure(ErrorCode::Conflict, Recovery::ReadCurrent)};
          }
          return work.host_commands().record_receipt(command_id, persisted, outcome, process_marker,
                                                     now);
        });
  }

  Outcome<bool> host_observation(const HostObservationRecord& observation) override {
    if (observation.host_id.empty() || observation.connection_generation == 0 ||
        observation.sequence == 0 || observation.instance_id.empty() ||
        observation.execution_generation == 0) {
      return invalid_runtime_input();
    }
    // The HostBridge transport has already verified the frame fence against the authoritative
    // registration/connection state; this trusted adapter must not require a second PG row lookup
    // (composition may keep registration in a different store). The observation key itself carries
    // host+connection_generation+sequence, so dedupe/old-generation separation stays intact.
    return with_host_work<bool>(transactions_, [&](RuntimeUnitOfWork& work, TimePoint) {
      return work.host_commands().record_observation(observation);
    });
  }

  Outcome<HostCommandMaintenance> host_command_maintenance() override {
    return with_host_work<HostCommandMaintenance>(
        transactions_,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<HostCommandMaintenance> {
          HostCommandMaintenance result;
          auto expired = work.host_commands().expire_dispatched(now, kHostCommandDispatchTimeout);
          if (const auto* failure = std::get_if<Failure>(&expired)) {
            return *failure;
          }
          result.expired = std::get<std::size_t>(expired);
          auto pruned = work.host_commands().prune(now, kHostCommandResolvedRetention,
                                                   kHostCommandUnresolvedRetention, 100);
          if (const auto* failure = std::get_if<Failure>(&pruned)) {
            return *failure;
          }
          result.pruned = std::get<std::size_t>(pruned);
          return result;
        });
  }

private:
  RuntimeUnitOfWorkFactory& transactions_;
};
} // namespace

std::unique_ptr<RuntimeHostCommandApplication>
make_host_commands(const RuntimeResourceDependencies& dependencies) {
  return std::make_unique<RuntimeHostCommandApplicationImpl>(dependencies);
}

} // namespace kiarabot::core::target::runtime_resources_detail
