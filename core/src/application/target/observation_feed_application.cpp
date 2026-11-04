#include "kiarabot/core/application/target/observation_feed_application.hpp"

#include <algorithm>
#include <chrono>
#include <string_view>
#include <utility>

namespace kiarabot::core::target {
namespace {
Failure failure(ErrorCode code, Recovery recovery = Recovery::Diagnose) {
  return {code, Acceptance::NotAccepted, recovery, {}, {}};
}
bool uuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
    } else if (!((character >= '0' && character <= '9') ||
                 (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}
Permission permission(ObservationFamily family) {
  if (family == ObservationFamily::Tasks) {
    return Permission::TaskRead;
  }
  if (family == ObservationFamily::Messages) {
    return Permission::MessageRead;
  }
  return Permission::LogRead;
}
std::string_view permission_text(ObservationFamily family) {
  if (family == ObservationFamily::Tasks) {
    return "task.read";
  }
  if (family == ObservationFamily::Messages) {
    return "message.read";
  }
  return "log.read";
}
bool kind(Service owner, ObservationFamily family, ResourceKind resource) {
  if (family == ObservationFamily::Logs) {
    return resource == ResourceKind::Catalog;
  }
  if (family == ObservationFamily::Tasks) {
    return owner == Service::Scheduler &&
           (resource == ResourceKind::Task || resource == ResourceKind::Instance);
  }
  return owner == Service::Runtime && resource == ResourceKind::Account;
}
bool cursor_valid(const ObservationFeedCursor& value, Service owner, ObservationFamily family,
                  const std::vector<ResourceRef>& resources) {
  auto selected = value.scope;
  return value.owner == owner && value.family == family && !value.epoch.empty() &&
         value.epoch.size() <= 128 && value.epoch.find('\0') == std::string::npos &&
         observation_feed_scope(owner, family, selected) && selected == resources;
}
bool payload_matches(Service owner, ObservationFamily family, const ObservationPayload& payload) {
  if (const auto* log = std::get_if<LogObservation>(&payload)) {
    return family == ObservationFamily::Logs &&
           (log->owner == Service::Identity || log->owner == Service::Runtime) &&
           log->occurred_at > TimePoint{};
  }
  if (const auto* task = std::get_if<TaskObservation>(&payload)) {
    return family == ObservationFamily::Tasks && owner == Service::Scheduler && task->version > 0 &&
           task->updated_at > TimePoint{};
  }
  if (const auto* message = std::get_if<MessageObservation>(&payload)) {
    return family == ObservationFamily::Messages && owner == Service::Runtime &&
           message->received_at > TimePoint{};
  }
  return false;
}
bool page_valid(const ObservationFeedPage& page, const ObservationFeedQuery& query, Service owner) {
  if (!cursor_valid(page.next_cursor, owner, query.family, query.scope) ||
      page.checked_at <= TimePoint{} ||
      page.changes.size() > (query.page_size == 0 ? 32 : query.page_size) ||
      (page.has_more && page.changes.empty())) {
    return false;
  }
  auto sequence = query.cursor ? query.cursor->sequence : page.next_cursor.sequence;
  if (query.cursor &&
      (query.cursor->epoch != page.next_cursor.epoch || sequence > page.next_cursor.sequence)) {
    return false;
  }
  if (!query.cursor && !page.changes.empty()) {
    return false;
  }
  for (const auto& change : page.changes) {
    if (!cursor_valid(change.cursor, owner, query.family, query.scope) ||
        change.cursor.epoch != page.next_cursor.epoch || change.cursor.sequence <= sequence ||
        change.cursor.sequence > page.next_cursor.sequence ||
        !payload_matches(owner, query.family, change.payload)) {
      return false;
    }
    sequence = change.cursor.sequence;
  }
  return !page.has_more || page.next_cursor.sequence == sequence;
}

class Application final : public ObservationFeedApplication {
public:
  explicit Application(ObservationFeedDependencies dependencies)
      : dependencies_(std::move(dependencies)) {}
  Outcome<ObservationFeedPage> query(const ObservationFeedCommand& command) override {
    try {
      if (!observation_feed_owner_family(dependencies_.owner, dependencies_.family) ||
          observation_feed_method(dependencies_.owner, dependencies_.family).empty()) {
        return failure(ErrorCode::CapabilityNotSupported);
      }
      const auto started = dependencies_.now();
      const auto deadline =
          std::min(command.context.deadline, started + std::chrono::milliseconds{500});
      auto input = command.query;
      if (!uuid(command.context.request_id) || !uuid(command.context.trace_id) ||
          command.context.method !=
              observation_feed_method(dependencies_.owner, dependencies_.family) ||
          input.family != dependencies_.family || input.page_size > 100 ||
          !observation_feed_scope(dependencies_.owner, dependencies_.family, input.scope) ||
          (input.cursor &&
           !cursor_valid(*input.cursor, dependencies_.owner, dependencies_.family, input.scope))) {
        return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
      const auto& authentication = command.context.authentication;
      const auto* peer = std::get_if<PlatformPeer>(&authentication.direct_peer);
      if (!peer || peer->service != Service::Control || !uuid(peer->subject.value)) {
        return failure(ErrorCode::PermissionDenied, Recovery::Authenticate);
      }
      if (authentication.audience != dependencies_.owner || !uuid(authentication.subject.value) ||
          authentication.authorization_version.value == 0 ||
          authentication.verified_at <= TimePoint{} || authentication.proof.empty() ||
          authentication.expires_at <= started) {
        return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
      }
      if (deadline <= started || std::chrono::steady_clock::now() >= input.deadline) {
        return failure(ErrorCode::Timeout, Recovery::Backoff);
      }
      const auto required = permission_text(dependencies_.family);
      for (const auto& resource : input.scope) {
        if (std::none_of(authentication.scopes.begin(), authentication.scopes.end(),
                         [&](const auto& binding) {
                           return binding.resource == resource && binding.permission == required;
                         })) {
          return failure(ErrorCode::PermissionDenied, Recovery::ReadCurrent);
        }
      }
      const auto selected_permission = permission(dependencies_.family);
      auto authorized = dependencies_.authorization.check(
          {authentication, selected_permission, command.context.method, input.scope, deadline});
      if (const auto* error = std::get_if<Failure>(&authorized)) {
        return *error;
      }
      const auto& decision = std::get<RuntimeAuthorizationDecision>(authorized);
      const auto checked = dependencies_.now();
      if (!decision.allowed) {
        return decision.denial ? *decision.denial
                               : failure(ErrorCode::PermissionDenied, Recovery::ReadCurrent);
      }
      if (decision.denial || decision.subject != authentication.subject ||
          decision.permission != selected_permission || decision.scope != input.scope ||
          decision.authorization_version.value == 0 || decision.checked_at <= TimePoint{} ||
          decision.checked_at < started - std::chrono::seconds{2} ||
          decision.checked_at > checked + std::chrono::seconds{2}) {
        return failure(ErrorCode::Internal);
      }
      if (checked >= deadline || checked >= authentication.expires_at) {
        return failure(ErrorCode::Timeout, Recovery::Backoff);
      }
      input.deadline =
          std::min(input.deadline, std::chrono::steady_clock::now() + (deadline - checked));
      auto begun = dependencies_.begin();
      if (const auto* error = std::get_if<Failure>(&begun)) {
        return *error;
      }
      auto work = std::move(std::get<std::unique_ptr<UnitOfWork>>(begun));
      if (!work || work->owner() != dependencies_.owner) {
        return failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
      }
      auto sampled = work->current_time();
      if (const auto* error = std::get_if<Failure>(&sampled)) {
        return *error;
      }
      const auto database_now = std::get<TimePoint>(sampled);
      if (database_now >= deadline || database_now >= authentication.expires_at ||
          std::chrono::steady_clock::now() >= input.deadline) {
        return failure(ErrorCode::Timeout, Recovery::Backoff);
      }
      auto queried = dependencies_.query(*work, input);
      work->rollback();
      if (const auto* error = std::get_if<Failure>(&queried)) {
        return *error;
      }
      auto page = std::move(std::get<ObservationFeedPage>(queried));
      const auto finished = dependencies_.now();
      if (finished >= deadline || finished >= authentication.expires_at ||
          std::chrono::steady_clock::now() >= input.deadline) {
        return failure(ErrorCode::Timeout, Recovery::Backoff);
      }
      if (!page_valid(page, input, dependencies_.owner) ||
          page.checked_at > finished + std::chrono::seconds{2}) {
        return failure(ErrorCode::Internal);
      }
      return page;
    } catch (...) {
      return failure(ErrorCode::Internal);
    }
  }

private:
  ObservationFeedDependencies dependencies_;
};
} // namespace

bool observation_feed_owner_family(Service owner, ObservationFamily family) {
  switch (family) {
  case ObservationFamily::Logs:
    return owner == Service::Identity || owner == Service::Runtime;
  case ObservationFamily::Messages:
    return owner == Service::Runtime;
  case ObservationFamily::Tasks:
    return owner == Service::Scheduler;
  }
  return false;
}
std::string_view observation_feed_method(Service owner, ObservationFamily family) {
  if (!observation_feed_owner_family(owner, family)) {
    return {};
  }
  switch (family) {
  case ObservationFamily::Logs:
    return owner == Service::Identity ? "kiarabot.v1.IdentityService/ListLogChanges"
                                      : "kiarabot.v1.RuntimeService/ListLogChanges";
  case ObservationFamily::Messages:
    return "kiarabot.v1.RuntimeService/ListMessageMetadataChanges";
  case ObservationFamily::Tasks:
    return "kiarabot.v1.SchedulerService/ListTaskChanges";
  }
  return {};
}
std::string_view observation_feed_permission_text(Service owner, ObservationFamily family) {
  if (!observation_feed_owner_family(owner, family)) {
    return {};
  }
  return permission_text(family);
}
bool observation_feed_scope(Service owner, ObservationFamily family,
                            std::vector<ResourceRef>& scope) {
  if (!observation_feed_owner_family(owner, family) || scope.empty() || scope.size() > 127) {
    return false;
  }
  for (const auto& resource : scope) {
    if (!kind(owner, family, resource.kind) || resource.id.value.empty() ||
        resource.id.value.size() > 256) {
      return false;
    }
  }
  std::sort(scope.begin(), scope.end(), [](const auto& left, const auto& right) {
    return std::pair{left.kind, left.id.value} < std::pair{right.kind, right.id.value};
  });
  return std::adjacent_find(scope.begin(), scope.end()) == scope.end();
}

Outcome<std::unique_ptr<ObservationFeedApplication>>
make_observation_feed_application(ObservationFeedDependencies dependencies) {
  if (!observation_feed_owner_family(dependencies.owner, dependencies.family) ||
      !dependencies.begin || !dependencies.query) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (!dependencies.now) {
    dependencies.now = [] {
      return TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())};
    };
  }
  try {
    return std::unique_ptr<ObservationFeedApplication>(
        std::make_unique<Application>(std::move(dependencies)));
  } catch (...) {
    return failure(ErrorCode::Internal);
  }
}
} // namespace kiarabot::core::target
