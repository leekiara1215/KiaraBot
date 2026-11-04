#include <algorithm>
#include <set>

#include "common.hpp"
#include "components.hpp"
#include "kiarabot/core/domain/target/event_delivery.hpp"
#include "kiarabot/core/domain/target/host_command_records.hpp"
#include "kiarabot/core/ports/target/business_ports.hpp"
#include "mutation.hpp"

namespace kiarabot::core::target::runtime_resources_detail {
namespace {
// P13a lifecycle helpers -------------------------------------------------------------------
bool lifecycle_resource_kind(ResourceKind kind) {
  return kind == ResourceKind::Account || kind == ResourceKind::Adapter ||
         kind == ResourceKind::Installation || kind == ResourceKind::Instance ||
         kind == ResourceKind::Host;
}
Failure lifecycle_conflict() {
  return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
}

struct LifecycleResource {
  ResourceRef resource;
  ResourceVersion version;
  LifecycleState state = LifecycleState::Active;
  bool managed_by_host = false;
};

Outcome<LifecycleResource> load_lifecycle_resource(RuntimeUnitOfWork& work,
                                                   const ResourceRef& resource) {
  if (!canonical(resource.id.value) || !lifecycle_resource_kind(resource.kind)) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (resource.kind == ResourceKind::Account) {
    auto found = work.accounts().find(resource.id);
    if (const auto* failure = std::get_if<Failure>(&found)) {
      return *failure;
    }
    const auto value = std::move(std::get<std::optional<BotAccount>>(found));
    if (!value) {
      return not_found();
    }
    return LifecycleResource{resource, value->version, value->state, false};
  }
  if (resource.kind == ResourceKind::Adapter) {
    auto found = work.adapters().find(resource.id);
    if (const auto* failure = std::get_if<Failure>(&found)) {
      return *failure;
    }
    const auto value = std::move(std::get<std::optional<AdapterRecord>>(found));
    if (!value) {
      return not_found();
    }
    return LifecycleResource{resource, value->version, value->state, false};
  }
  if (resource.kind == ResourceKind::Installation) {
    auto found = work.installations().find(InstallationId{resource.id.value});
    if (const auto* failure = std::get_if<Failure>(&found)) {
      return *failure;
    }
    const auto value = std::move(std::get<std::optional<PluginInstallation>>(found));
    if (!value) {
      return not_found();
    }
    return LifecycleResource{resource, value->version, value->state, false};
  }
  if (resource.kind == ResourceKind::Host) {
    auto found = work.hosts().find(resource.id);
    if (const auto* failure = std::get_if<Failure>(&found)) {
      return *failure;
    }
    const auto value = std::move(std::get<std::optional<HostRegistration>>(found));
    if (!value) {
      return not_found();
    }
    return LifecycleResource{resource, value->version, value->state, false};
  }
  auto found = work.instances().find(InstanceId{resource.id.value});
  if (const auto* failure = std::get_if<Failure>(&found)) {
    return *failure;
  }
  const auto value = std::move(std::get<std::optional<PluginInstance>>(found));
  if (!value) {
    return not_found();
  }
  return LifecycleResource{resource, value->version, value->state, value->host.has_value()};
}

// Applies one lifecycle state transition with the caller-supplied expected version (or a current
// version re-read when expected is null). Deletion also revokes a Runtime-local bridge credential;
// it never revives credentials on the Active path.
Outcome<LifecycleResource> save_lifecycle_state(RuntimeUnitOfWork& work,
                                                const ResourceRef& resource, LifecycleState state,
                                                std::optional<ResourceVersion> expected,
                                                bool stop_desired, TimePoint now) {
  auto current = load_lifecycle_resource(work, resource);
  if (const auto* failure = std::get_if<Failure>(&current)) {
    return *failure;
  }
  auto value = std::move(std::get<LifecycleResource>(current));
  if (expected && value.version != *expected) {
    return lifecycle_conflict();
  }
  if (state == LifecycleState::Deleting) {
    if (resource.kind == ResourceKind::Adapter || resource.kind == ResourceKind::Instance) {
      auto revoked = work.bridge_credentials().revoke(resource.id.value, now);
      if (const auto* failure = std::get_if<Failure>(&revoked)) {
        return *failure;
      }
    }
  }
  if (resource.kind == ResourceKind::Account) {
    auto found = work.accounts().find(resource.id);
    if (const auto* failure = std::get_if<Failure>(&found)) {
      return *failure;
    }
    auto record = std::move(std::get<std::optional<BotAccount>>(found));
    if (!record) {
      return not_found();
    }
    record->state = state;
    record->version.value += 1;
    auto saved = work.accounts().save(*record, value.version);
    if (const auto* failure = std::get_if<Failure>(&saved)) {
      return *failure;
    }
    return LifecycleResource{resource, record->version, state, false};
  }
  if (resource.kind == ResourceKind::Adapter) {
    auto found = work.adapters().find(resource.id);
    if (const auto* failure = std::get_if<Failure>(&found)) {
      return *failure;
    }
    auto record = std::move(std::get<std::optional<AdapterRecord>>(found));
    if (!record) {
      return not_found();
    }
    record->state = state;
    record->version.value += 1;
    auto saved = work.adapters().save(*record, value.version);
    if (const auto* failure = std::get_if<Failure>(&saved)) {
      return *failure;
    }
    return LifecycleResource{resource, record->version, state, false};
  }
  if (resource.kind == ResourceKind::Installation) {
    const auto id = InstallationId{resource.id.value};
    auto found = work.installations().find(id);
    if (const auto* failure = std::get_if<Failure>(&found)) {
      return *failure;
    }
    auto record = std::move(std::get<std::optional<PluginInstallation>>(found));
    if (!record) {
      return not_found();
    }
    record->state = state;
    record->version.value += 1;
    auto saved = work.installations().save(*record, value.version);
    if (const auto* failure = std::get_if<Failure>(&saved)) {
      return *failure;
    }
    return LifecycleResource{resource, record->version, state, false};
  }
  if (resource.kind == ResourceKind::Host) {
    auto found = work.hosts().find(resource.id);
    if (const auto* failure = std::get_if<Failure>(&found)) {
      return *failure;
    }
    auto record = std::move(std::get<std::optional<HostRegistration>>(found));
    if (!record) {
      return not_found();
    }
    record->state = state;
    record->version.value += 1;
    auto saved = work.hosts().save(*record, value.version);
    if (const auto* failure = std::get_if<Failure>(&saved)) {
      return *failure;
    }
    return LifecycleResource{resource, record->version, state, false};
  }
  const auto id = InstanceId{resource.id.value};
  auto found = work.instances().find(id);
  if (const auto* failure = std::get_if<Failure>(&found)) {
    return *failure;
  }
  auto record = std::move(std::get<std::optional<PluginInstance>>(found));
  if (!record) {
    return not_found();
  }
  record->state = state;
  if (stop_desired) {
    record->desired = DesiredState::Stopped;
  }
  record->version.value += 1;
  auto saved = work.instances().save(*record, value.version);
  if (const auto* failure = std::get_if<Failure>(&saved)) {
    return *failure;
  }
  return LifecycleResource{resource, record->version, state, record->host.has_value()};
}

Operation lifecycle_operation(const CommandContext& context, const ResourceRef& resource,
                              TimePoint now, OperationState state, ExecutionOutcome outcome) {
  Operation operation;
  operation.ref = {Service::Runtime, OperationId{context.request_id}, resource};
  operation.version = ResourceVersion{1};
  operation.state = state;
  operation.execution_outcome = outcome;
  operation.created_at = now;
  operation.updated_at = now;
  operation.retain_until = now + kLifecycleOperationRetention;
  return operation;
}

std::vector<OperationParticipant> lifecycle_participants(const LifecycleResource& resource) {
  std::vector<OperationParticipant> participants;
  participants.push_back(OperationParticipant{"runtime", OperationState::Succeeded, {}, {}});
  const bool instance = resource.resource.kind == ResourceKind::Instance;
  const bool installation = resource.resource.kind == ResourceKind::Installation;
  if (instance && resource.managed_by_host) {
    participants.push_back(OperationParticipant{"host", OperationState::Accepted, {}, {}});
  }
  if (instance || installation) {
    participants.push_back(OperationParticipant{"scheduler", OperationState::Accepted, {}, {}});
  }
  return participants;
}

bool participant_done(const OperationParticipant& participant) {
  return participant.state == OperationState::Succeeded ||
         participant.state == OperationState::Failed ||
         participant.state == OperationState::Cancelled ||
         participant.state == OperationState::Expired;
}

struct LifecyclePending {
  LifecycleTombstone tombstone;
  Operation operation;
  std::vector<OperationParticipant> participants;
};

template <typename Value>
Outcome<Value>
observe_work(RuntimeUnitOfWorkFactory& transactions,
             const std::function<Outcome<Value>(RuntimeUnitOfWork&, TimePoint)>& body) {
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
      return *failure;
    }
    auto result = body(*work, std::get<TimePoint>(now));
    work->rollback();
    return std::move(result);
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return internal_failure();
  }
}
class RuntimeLifecycleApplicationImpl final : public RuntimeLifecycleApplication {
public:
  RuntimeLifecycleApplicationImpl(const RuntimeResourceDependencies& dependencies)
      : transactions_(dependencies.transactions), authorization_(dependencies.authorization),
        host_lifecycle_(dependencies.host_lifecycle),
        scheduler_lifecycle_(dependencies.scheduler_lifecycle) {}

  Outcome<ResourceState> get_resource_state(const ResourceStateQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    if (!canonical(request.resource.id.value)) {
      return invalid_runtime_input();
    }
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::ResourceRead,
                  request.context.method, {request.resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    return with_work<ResourceState>(
        transactions_, request.context,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<ResourceState> {
          if (request.resource.kind == ResourceKind::Account) {
            auto found = work.accounts().find(request.resource.id);
            if (const auto* failure = std::get_if<Failure>(&found)) {
              return *failure;
            }
            const auto value = std::move(std::get<std::optional<BotAccount>>(found));
            if (!value) {
              return not_found();
            }
            ResourceState state;
            state.resource = request.resource;
            state.version = value->version;
            state.lifecycle = value->state;
            state.desired = DesiredState::Running;
            state.source_service = Service::Runtime;
            state.observed_at = now;
            return state;
          }
          if (request.resource.kind == ResourceKind::Adapter) {
            auto found = work.adapters().find(request.resource.id);
            if (const auto* failure = std::get_if<Failure>(&found)) {
              return *failure;
            }
            const auto value = std::move(std::get<std::optional<AdapterRecord>>(found));
            if (!value) {
              return not_found();
            }
            ResourceState state;
            state.resource = request.resource;
            state.version = value->version;
            state.lifecycle = value->state;
            state.desired = DesiredState::Running;
            state.adapter_fence = value->current;
            state.registered = adapter_online(*value, now);
            state.observed_at = now;
            state.source_service = Service::Runtime;
            return state;
          }
          if (request.resource.kind == ResourceKind::Installation) {
            auto found = work.installations().find(InstallationId{request.resource.id.value});
            if (const auto* failure = std::get_if<Failure>(&found)) {
              return *failure;
            }
            const auto value = std::move(std::get<std::optional<PluginInstallation>>(found));
            if (!value) {
              return not_found();
            }
            ResourceState state;
            state.resource = request.resource;
            state.version = value->version;
            state.lifecycle = value->state;
            state.desired = DesiredState::Running;
            state.observed_at = now;
            return state;
          }
          if (request.resource.kind == ResourceKind::Instance) {
            auto found = work.instances().find(InstanceId{request.resource.id.value});
            if (const auto* failure = std::get_if<Failure>(&found)) {
              return *failure;
            }
            const auto value = std::move(std::get<std::optional<PluginInstance>>(found));
            if (!value) {
              return not_found();
            }
            ResourceState state;
            state.resource = request.resource;
            state.version = value->version;
            state.lifecycle = value->state;
            state.desired = value->desired;
            state.fence = value->current;
            state.registered = value->current.has_value();
            state.desired_config_version = value->desired_version;
            state.applied_config_version = value->applied_config_version;
            state.observed_at = now;
            state.source_service = Service::Runtime;
            return state;
          }
          return failure(ErrorCode::CapabilityNotSupported);
        });
  }

  Outcome<Operation> get_operation(const OperationQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    if (request.operation.owner != Service::Runtime || !canonical(request.operation.id.value) ||
        !canonical(request.operation.resource.id.value)) {
      return invalid_runtime_input();
    }
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::ResourceRead,
                  request.context.method, {request.operation.resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    return with_work<Operation>(transactions_, request.context,
                                [&](RuntimeUnitOfWork& work, TimePoint) -> Outcome<Operation> {
                                  auto found =
                                      require_found(work.operations().find(request.operation.id));
                                  if (const auto* failure = std::get_if<Failure>(&found)) {
                                    return *failure;
                                  }
                                  auto operation = std::move(std::get<Operation>(found));
                                  if (operation.ref != request.operation) {
                                    return not_found();
                                  }
                                  auto participants = work.operation_participants(operation.ref.id);
                                  if (const auto* failure = std::get_if<Failure>(&participants)) {
                                    // A partial P05 adapter may not implement the P13a participant
                                    // projection. That is not a reason to fail an otherwise valid
                                    // operation read; the wire simply shows no rows.
                                    if (failure->code != ErrorCode::CapabilityNotSupported) {
                                      return *failure;
                                    }
                                  } else {
                                    operation.participants = std::move(
                                        std::get<std::vector<OperationParticipant>>(participants));
                                  }
                                  return operation;
                                });
  }

  Outcome<Operation> begin_delete(const BeginDeleteRequest& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    if (!canonical(request.resource.id.value) || !lifecycle_resource_kind(request.resource.kind)) {
      return invalid_runtime_input();
    }
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::ResourceManage,
                  request.context.method, {request.resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    auto result = mutate<Operation>(
        transactions_, request.context, request.resource, authorization_version,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<Operation> {
          auto loaded = load_lifecycle_resource(work, request.resource);
          if (const auto* failure = std::get_if<Failure>(&loaded)) {
            return *failure;
          }
          auto current = std::move(std::get<LifecycleResource>(loaded));
          if (request.context.expected_version &&
              current.version != *request.context.expected_version) {
            return lifecycle_conflict();
          }
          if (current.state != LifecycleState::Active &&
              current.state != LifecycleState::Disabled) {
            return lifecycle_conflict();
          }
          auto existing = work.find_lifecycle_tombstone(request.resource);
          if (const auto* failure = std::get_if<Failure>(&existing)) {
            return *failure;
          }
          if (std::get<std::optional<LifecycleTombstone>>(existing)) {
            return lifecycle_conflict();
          }
          const auto participants = lifecycle_participants(current);
          const bool external =
              std::any_of(participants.begin(), participants.end(), [](const auto& participant) {
                return participant.state != OperationState::Succeeded;
              });
          const auto state = external ? LifecycleState::Deleting : LifecycleState::Deleted;
          auto saved =
              save_lifecycle_state(work, request.resource, state, current.version, true, now);
          if (const auto* failure = std::get_if<Failure>(&saved)) {
            return *failure;
          }
          auto operation = lifecycle_operation(
              request.context, request.resource, now,
              external ? OperationState::Accepted : OperationState::Succeeded,
              external ? ExecutionOutcome::NotStarted : ExecutionOutcome::KnownSucceeded);
          operation.participants = participants;
          auto written = work.operations().save(operation, std::nullopt);
          if (const auto* failure = std::get_if<Failure>(&written)) {
            return *failure;
          }
          auto participant_write = work.save_operation_participants(operation.ref.id, participants);
          if (const auto* failure = std::get_if<Failure>(&participant_write)) {
            return *failure;
          }
          LifecycleTombstone tombstone;
          tombstone.resource = request.resource;
          tombstone.operation_id = operation.ref.id.value;
          tombstone.state = state;
          tombstone.deleted_at = now;
          tombstone.restore_until = now + kLifecycleRestoreWindow;
          tombstone.purge_after = now + kLifecycleTombstoneRetention;
          auto tombstone_saved = work.save_lifecycle_tombstone(tombstone);
          if (const auto* failure = std::get_if<Failure>(&tombstone_saved)) {
            return *failure;
          }
          return operation;
        },
        [](const Operation& operation) { return operation.ref.resource; },
        [](const Operation&) { return ResourceVersion{1}; });
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    return std::move(std::get<Mutation<Operation>>(result).value);
  }

  Outcome<Operation> restore_resource(const RestoreResourceRequest& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    if (!canonical(request.resource.id.value) || !lifecycle_resource_kind(request.resource.kind)) {
      return invalid_runtime_input();
    }
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::ResourceManage,
                  request.context.method, {request.resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    auto result = mutate<Operation>(
        transactions_, request.context, request.resource, authorization_version,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<Operation> {
          auto found_tombstone = work.find_lifecycle_tombstone(request.resource);
          if (const auto* failure = std::get_if<Failure>(&found_tombstone)) {
            return *failure;
          }
          const auto tombstone =
              std::move(std::get<std::optional<LifecycleTombstone>>(found_tombstone));
          if (!tombstone || tombstone->state != LifecycleState::Deleted) {
            return lifecycle_conflict();
          }
          if (now >= tombstone->restore_until) {
            // The reversible window elapsed; restore must not resurrect content or credentials.
            return lifecycle_conflict();
          }
          auto loaded = load_lifecycle_resource(work, request.resource);
          if (const auto* failure = std::get_if<Failure>(&loaded)) {
            return *failure;
          }
          auto current = std::move(std::get<LifecycleResource>(loaded));
          if (request.context.expected_version &&
              current.version != *request.context.expected_version) {
            return lifecycle_conflict();
          }
          if (current.state != LifecycleState::Deleted) {
            return lifecycle_conflict();
          }
          auto saved = save_lifecycle_state(work, request.resource, LifecycleState::Active,
                                            current.version, false, now);
          if (const auto* failure = std::get_if<Failure>(&saved)) {
            return *failure;
          }
          auto tombstone_deleted = work.delete_lifecycle_tombstone(request.resource);
          if (const auto* failure = std::get_if<Failure>(&tombstone_deleted)) {
            return *failure;
          }
          auto operation =
              lifecycle_operation(request.context, request.resource, now, OperationState::Succeeded,
                                  ExecutionOutcome::KnownSucceeded);
          operation.participants = {
              OperationParticipant{"runtime", OperationState::Succeeded, {}, {}}};
          auto written = work.operations().save(operation, std::nullopt);
          if (const auto* failure = std::get_if<Failure>(&written)) {
            return *failure;
          }
          auto participant_write =
              work.save_operation_participants(operation.ref.id, operation.participants);
          if (const auto* failure = std::get_if<Failure>(&participant_write)) {
            return *failure;
          }
          return operation;
        },
        [](const Operation& operation) { return operation.ref.resource; },
        [](const Operation&) { return ResourceVersion{1}; });
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    return std::move(std::get<Mutation<Operation>>(result).value);
  }

  Outcome<LifecycleMaintenance> lifecycle_maintenance() override {
    // Phase 1: bounded owner-local observation, no network I/O inside the UoW.
    using LifecycleObservation = std::pair<TimePoint, std::vector<LifecyclePending>>;
    auto observed = observe_work<LifecycleObservation>(
        transactions_,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<LifecycleObservation> {
          std::vector<LifecyclePending> pending;
          auto tombstones = work.list_lifecycle_tombstones(100, "");
          if (const auto* failure = std::get_if<Failure>(&tombstones)) {
            return *failure;
          }
          for (const auto& tombstone : std::get<std::vector<LifecycleTombstone>>(tombstones)) {
            auto operation = work.operations().find(OperationId{tombstone.operation_id});
            if (const auto* failure = std::get_if<Failure>(&operation)) {
              return *failure;
            }
            const auto stored = std::move(std::get<std::optional<Operation>>(operation));
            if (!stored) {
              continue;
            }
            std::vector<OperationParticipant> participant_rows;
            auto participants = work.operation_participants(stored->ref.id);
            if (const auto* failure = std::get_if<Failure>(&participants)) {
              if (failure->code != ErrorCode::CapabilityNotSupported) {
                return *failure;
              }
            } else {
              participant_rows =
                  std::move(std::get<std::vector<OperationParticipant>>(participants));
            }
            pending.push_back(LifecyclePending{tombstone, *stored, std::move(participant_rows)});
          }
          return LifecycleObservation{now, std::move(pending)};
        });
    if (const auto* failure = std::get_if<Failure>(&observed)) {
      return *failure;
    }
    auto observation = std::move(std::get<LifecycleObservation>(observed));
    const auto sampled = observation.first;
    const auto pending = std::move(observation.second);
    struct Decision {
      LifecycleTombstone tombstone;
      Operation operation;
      std::vector<OperationParticipant> participants;
      bool purge = false;
      bool complete = false;
      bool blocked = false;
    };
    std::vector<Decision> decisions;
    for (const auto& item : pending) {
      Decision decision{item.tombstone, item.operation, item.participants, false, false, false};
      if (item.tombstone.state == LifecycleState::Deleted) {
        if (sampled >= item.tombstone.purge_after) {
          decision.purge = true;
        } else {
          continue;
        }
      } else if (item.tombstone.state != LifecycleState::Deleting) {
        continue;
      } else {
        if (item.operation.next_attempt && sampled < *item.operation.next_attempt) {
          continue;
        }
        for (auto& participant : decision.participants) {
          if (participant_done(participant)) {
            continue;
          }
          LifecycleParticipantResult result;
          if (participant.participant == "runtime") {
            result.disposition = LifecycleParticipantDisposition::Done;
          } else if (participant.participant == "host") {
            if (host_lifecycle_) {
              result = host_lifecycle_->stop_instance(item.tombstone.resource);
            } else {
              result.disposition = LifecycleParticipantDisposition::Blocked;
              result.reason = ErrorCode::UpstreamUnavailable;
              result.next_attempt = sampled + kLifecycleParticipantRetry;
            }
          } else if (participant.participant == "scheduler") {
            if (scheduler_lifecycle_) {
              result = scheduler_lifecycle_->cancel_tasks(item.tombstone.resource);
            } else {
              result.disposition = LifecycleParticipantDisposition::Blocked;
              result.reason = ErrorCode::UpstreamUnavailable;
              result.next_attempt = sampled + kLifecycleParticipantRetry;
            }
          } else {
            result.disposition = LifecycleParticipantDisposition::Unknown;
            result.next_attempt = sampled + kLifecycleParticipantRetry;
          }
          if (result.disposition == LifecycleParticipantDisposition::Done) {
            participant.state = OperationState::Succeeded;
            participant.reason.reset();
            participant.next_attempt.reset();
          } else if (result.disposition == LifecycleParticipantDisposition::Blocked) {
            participant.state = OperationState::Blocked;
            participant.reason = result.reason.value_or(ErrorCode::UpstreamUnavailable);
            participant.next_attempt =
                result.next_attempt.value_or(sampled + kLifecycleParticipantRetry);
            decision.blocked = true;
          } else {
            participant.state = OperationState::Running;
            participant.reason = result.reason;
            participant.next_attempt =
                result.next_attempt.value_or(sampled + kLifecycleParticipantRetry);
          }
        }
        decision.complete = std::all_of(decision.participants.begin(), decision.participants.end(),
                                        participant_done);
      }
      decisions.push_back(std::move(decision));
    }

    // Phase 2: apply decisions and the 60s retention batch in one local UoW.
    struct ApplyResult {
      LifecycleMaintenance maintenance;
      std::optional<Failure> failure;
    };
    const auto applied = [&]() -> Outcome<ApplyResult> {
      try {
        auto begun = transactions_.begin();
        if (const auto* failure = std::get_if<Failure>(&begun)) {
          return *failure;
        }
        auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
        if (!work) {
          return internal_failure();
        }
        auto now_result = work->current_time();
        if (const auto* failure = std::get_if<Failure>(&now_result)) {
          work->rollback();
          return *failure;
        }
        const auto now = std::get<TimePoint>(now_result);
        ApplyResult result;
        for (auto& decision : decisions) {
          if (decision.purge) {
            auto loaded = load_lifecycle_resource(*work, decision.tombstone.resource);
            if (const auto* failure = std::get_if<Failure>(&loaded)) {
              work->rollback();
              return *failure;
            }
            const auto current = std::move(std::get<LifecycleResource>(loaded));
            if (current.state == LifecycleState::Purged) {
              auto deleted = work->delete_lifecycle_tombstone(decision.tombstone.resource);
              if (const auto* failure = std::get_if<Failure>(&deleted)) {
                work->rollback();
                return *failure;
              }
              continue;
            }
            if (current.state != LifecycleState::Deleted) {
              continue;
            }
            auto purged = save_lifecycle_state(*work, decision.tombstone.resource,
                                               LifecycleState::Purged, current.version, false, now);
            if (const auto* failure = std::get_if<Failure>(&purged)) {
              work->rollback();
              return *failure;
            }
            auto deleted = work->delete_lifecycle_tombstone(decision.tombstone.resource);
            if (const auto* failure = std::get_if<Failure>(&deleted)) {
              work->rollback();
              return *failure;
            }
            ++result.maintenance.purged;
            continue;
          }
          auto operation = work->operations().find(OperationId{decision.operation.ref.id.value});
          if (const auto* failure = std::get_if<Failure>(&operation)) {
            work->rollback();
            return *failure;
          }
          auto stored = std::move(std::get<std::optional<Operation>>(operation));
          if (!stored) {
            continue;
          }
          auto participants = work->operation_participants(stored->ref.id);
          if (const auto* failure = std::get_if<Failure>(&participants)) {
            if (failure->code != ErrorCode::CapabilityNotSupported) {
              work->rollback();
              return *failure;
            }
          }
          auto updated = decision.operation;
          updated.version.value = stored->version.value + 1;
          updated.participants = decision.participants;
          updated.updated_at = now;
          updated.next_attempt.reset();
          if (decision.complete) {
            auto saved = save_lifecycle_state(*work, decision.tombstone.resource,
                                              LifecycleState::Deleted, std::nullopt, false, now);
            if (const auto* failure = std::get_if<Failure>(&saved)) {
              work->rollback();
              return *failure;
            }
            updated.state = OperationState::Succeeded;
            updated.execution_outcome = ExecutionOutcome::KnownSucceeded;
            decision.tombstone.state = LifecycleState::Deleted;
            ++result.maintenance.completed;
          } else if (decision.blocked) {
            updated.state = OperationState::Blocked;
            updated.blocked_reason = ErrorCode::UpstreamUnavailable;
            updated.next_attempt = now + kLifecycleParticipantRetry;
            ++result.maintenance.blocked;
          } else {
            updated.state = OperationState::Running;
            updated.next_attempt = now + kLifecycleParticipantRetry;
          }
          auto written = work->operations().save(updated, stored->version);
          if (const auto* failure = std::get_if<Failure>(&written)) {
            work->rollback();
            return *failure;
          }
          auto participant_write =
              work->save_operation_participants(updated.ref.id, decision.participants);
          if (const auto* failure = std::get_if<Failure>(&participant_write)) {
            work->rollback();
            return *failure;
          }
          auto tombstone_saved = work->save_lifecycle_tombstone(decision.tombstone);
          if (const auto* failure = std::get_if<Failure>(&tombstone_saved)) {
            work->rollback();
            return *failure;
          }
          ++result.maintenance.reconciled;
        }
        ++maintenance_round_;
        if (maintenance_round_ % 60U == 0U) {
          const auto prune = [&](auto&& operation, std::size_t* total) -> bool {
            auto pruned = operation();
            if (const auto* failure = std::get_if<Failure>(&pruned)) {
              return false;
            }
            *total += std::get<std::size_t>(pruned);
            return true;
          };
          std::size_t pruned = 0;
          bool cleanup_failed = false;
          cleanup_failed |=
              !prune([&] { return work->receipts().prune_expired(now, 100); }, &pruned);
          cleanup_failed |=
              !prune([&] { return work->journal().prune_received(now, 100); }, &pruned);
          cleanup_failed |= !prune([&] { return work->prune_outbox(now, 100); }, &pruned);
          cleanup_failed |= !prune(
              [&] { return work->prune_audit(now - kLifecycleTombstoneRetention, 100); }, &pruned);
          cleanup_failed |= !prune(
              [&] {
                return work->host_commands().expire_dispatched(now, kHostCommandDispatchTimeout);
              },
              &pruned);
          cleanup_failed |= !prune(
              [&] {
                return work->host_commands().prune(now, kHostCommandResolvedRetention,
                                                   kHostCommandUnresolvedRetention, 100);
              },
              &pruned);
          cleanup_failed |= !prune([&] { return work->prune_operations(now, 100); }, &pruned);
          cleanup_failed |=
              !prune([&] { return work->storage_receipts().prune_expired(now, 100); }, &pruned);
          result.maintenance.pruned = pruned;
          result.maintenance.cleanup_failed = cleanup_failed;
        }
        if (work->commit() != CommitResult::Committed) {
          return unavailable();
        }
        return result;
      } catch (const std::bad_alloc&) {
        return unavailable();
      } catch (...) {
        return internal_failure();
      }
    }();
    if (const auto* failure = std::get_if<Failure>(&applied)) {
      return *failure;
    }
    return std::get<ApplyResult>(applied).maintenance;
  }

private:
  RuntimeUnitOfWorkFactory& transactions_;
  RuntimeAuthorizationPort& authorization_;
  HostLifecycleParticipantPort* const host_lifecycle_;
  SchedulerLifecycleParticipantPort* const scheduler_lifecycle_;
  std::size_t maintenance_round_ = 0;
};
} // namespace

std::unique_ptr<RuntimeLifecycleApplication>
make_lifecycle(const RuntimeResourceDependencies& dependencies) {
  return std::make_unique<RuntimeLifecycleApplicationImpl>(dependencies);
}

} // namespace kiarabot::core::target::runtime_resources_detail
