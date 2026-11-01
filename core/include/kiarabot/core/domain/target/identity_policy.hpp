#pragma once

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "kiarabot/core/domain/target/identity_credential_rules.hpp"

namespace kiarabot::core::target {

inline constexpr std::array<std::pair<std::string_view, Permission>, 40> kPermissionNames{
    {{"account.manage", Permission::AccountManage},
     {"account.policy.manage", Permission::AccountPolicyManage},
     {"account.policy.read", Permission::AccountPolicyRead},
     {"action.execute", Permission::ActionExecute},
     {"action.inspect", Permission::ActionInspect},
     {"adapter.connect", Permission::AdapterConnect},
     {"delivery.manage", Permission::DeliveryManage},
     {"delivery.process", Permission::DeliveryProcess},
     {"host.execute", Permission::HostExecute},
     {"host.read", Permission::HostRead},
     {"host.register", Permission::HostRegister},
     {"identity.assignment.manage", Permission::IdentityAssignmentManage},
     {"identity.assignment.read", Permission::IdentityAssignmentRead},
     {"identity.authorization.check", Permission::IdentityAuthorizationCheck},
     {"identity.delegation.issue", Permission::IdentityDelegationIssue},
     {"identity.invalidation.read", Permission::IdentityInvalidationRead},
     {"identity.keys.read", Permission::IdentityKeysRead},
     {"identity.principal.manage", Permission::IdentityPrincipalManage},
     {"identity.principal.read", Permission::IdentityPrincipalRead},
     {"identity.session.create", Permission::IdentitySessionCreate},
     {"identity.session.read", Permission::IdentitySessionRead},
     {"identity.session.revoke", Permission::IdentitySessionRevoke},
     {"model.call.inspect", Permission::ModelCallInspect},
     {"model.chat", Permission::ModelChat},
     {"model.manage", Permission::ModelManage},
     {"model.read", Permission::ModelRead},
     {"namespace.manage", Permission::NamespaceManage},
     {"plugin.connect", Permission::PluginConnect},
     {"plugin.manage", Permission::PluginManage},
     {"resource.manage", Permission::ResourceManage},
     {"resource.read", Permission::ResourceRead},
     {"storage.namespace", Permission::StorageNamespace},
     {"task.read", Permission::TaskRead},
     {"task.schedule", Permission::TaskSchedule},
     {"timer.accept", Permission::TimerAccept},
     {"account.read", Permission::AccountRead},
     {"plugin.read", Permission::PluginRead},
     {"adapter.read", Permission::AdapterRead},
     {"plugin.config.read", Permission::PluginConfigRead},
     {"message.read", Permission::MessageRead}}};
inline bool known_permission(Permission permission) {
  for (const auto& entry : kPermissionNames) {
    if (entry.second == permission) {
      return true;
    }
  }
  return false;
}
inline Outcome<Permission> permission_from_name(std::string_view name) {
  for (const auto& entry : kPermissionNames) {
    if (entry.first == name) {
      return entry.second;
    }
  }
  return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
}
inline bool valid_policy_resource(const ResourceRef& resource) {
  return valid_resource_ref(resource) && resource.id.value != "*";
}
inline bool known_principal_kind(PrincipalKind kind) {
  return kind == PrincipalKind::Human || kind == PrincipalKind::PlatformService ||
         kind == PrincipalKind::Adapter || kind == PrincipalKind::Plugin;
}
inline bool known_role(FixedRole role) {
  return role == FixedRole::Admin || role == FixedRole::Operator || role == FixedRole::Viewer;
}
// Explicit allowlists: future permissions NEVER become default-role privileges automatically.
// Action/result inspection, raw storage, model calls and platform workload capabilities require
// explicit scoped grants. MessageRead is explicit-only, absent from every role allowlist.
// Unknown names/values always reject; future additions do not inherit role privileges.
inline bool default_role_allows(FixedRole role, Permission permission) {
  if (!known_role(role) || !known_permission(permission)) {
    return false;
  }
  switch (permission) {
  case Permission::ResourceRead:
  case Permission::ModelRead:
  case Permission::AccountRead:
  case Permission::PluginRead:
  case Permission::AdapterRead:
  case Permission::HostRead:
    return true;
  case Permission::PluginManage:
  case Permission::TaskRead:
  case Permission::TaskSchedule:
  case Permission::DeliveryManage:
  case Permission::AccountPolicyRead:
  case Permission::AccountPolicyManage:
  case Permission::PluginConfigRead:
  case Permission::IdentitySessionRead:
  case Permission::IdentitySessionRevoke:
    return role == FixedRole::Admin || role == FixedRole::Operator;
  case Permission::AccountManage:
  case Permission::IdentityAssignmentManage:
  case Permission::IdentityAssignmentRead:
  case Permission::IdentityPrincipalManage:
  case Permission::IdentityPrincipalRead:
  case Permission::ModelManage:
  case Permission::NamespaceManage:
  case Permission::ResourceManage:
    return role == FixedRole::Admin;
  default:
    return false;
  }
}
struct PolicyResourceLess {
  bool operator()(const ResourceRef& left, const ResourceRef& right) const {
    return left.kind != right.kind ? left.kind < right.kind : left.id.value < right.id.value;
  }
};
inline Status validate_exact_assignment(const ExactAssignment& assignment) {
  if (assignment.id.value.empty() || assignment.subject.value.empty() ||
      assignment.version.value == 0 ||
      assignment.roles.empty() == assignment.explicit_permissions.empty()) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  std::set<ResourceRef, PolicyResourceLess> role_scope;
  if (!assignment.roles.empty()) {
    const auto role = assignment.roles.front().role;
    for (const auto& binding : assignment.roles) {
      if (binding.role != role || !known_role(binding.role) ||
          !valid_policy_resource(binding.resource) || !role_scope.insert(binding.resource).second) {
        return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
    }
  } else {
    std::map<ResourceRef, std::set<Permission>, PolicyResourceLess> permissions;
    for (const auto& binding : assignment.explicit_permissions) {
      if (!known_permission(binding.permission) || !valid_policy_resource(binding.resource) ||
          !permissions[binding.resource].insert(binding.permission).second) {
        return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
    }
    const auto& expected = permissions.begin()->second;
    for (const auto& [resource, assigned] : permissions) {
      static_cast<void>(resource);
      if (assigned != expected) {
        return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
    }
  }
  return std::monostate{};
}
inline Status validate_assignment_collection(std::span<const ExactAssignment> assignments,
                                             const SubjectId& expected_subject) {
  std::set<std::string> ids;
  for (const auto& assignment : assignments) {
    if (assignment.subject != expected_subject || !ids.insert(assignment.id.value).second ||
        std::holds_alternative<Failure>(validate_exact_assignment(assignment))) {
      return rejected(ErrorCode::Internal, Recovery::Diagnose);
    }
  }
  return std::monostate{};
}
struct AssignmentProjection {
  AssignmentId id;
  SubjectId subject;
  std::optional<FixedRole> role;
  std::vector<Permission> permissions;
  std::vector<ResourceRef> scope;
  ResourceVersion version;
};
// Framework-free safe AssignmentRecord projection. Never flatten arbitrary pairs into a larger
// permissions x scope product: corrupted/non-representable stored data returns Internal.
inline Outcome<AssignmentProjection> project_assignment(const ExactAssignment& assignment) {
  if (std::holds_alternative<Failure>(validate_exact_assignment(assignment))) {
    return rejected(ErrorCode::Internal, Recovery::Diagnose);
  }
  AssignmentProjection result{assignment.id,     assignment.subject, std::nullopt, {}, {},
                              assignment.version};
  std::set<ResourceRef, PolicyResourceLess> resources;
  if (!assignment.roles.empty()) {
    result.role = assignment.roles.front().role;
    for (const auto& role : assignment.roles) {
      resources.insert(role.resource);
    }
  } else {
    std::set<Permission> permissions;
    for (const auto& binding : assignment.explicit_permissions) {
      resources.insert(binding.resource);
      permissions.insert(binding.permission);
    }
    result.permissions.assign(permissions.begin(), permissions.end());
  }
  result.scope.assign(resources.begin(), resources.end());
  return result;
}
// The inherited P03 grant is an effective-permission metadata projection only. Authorization
// still reads exact assignments; never reconstruct roles x all scopes from these rows.
inline Outcome<ResourceGrant> to_resource_grant_metadata(const ExactAssignment& assignment) {
  const auto projected = project_assignment(assignment);
  if (const auto* failure = std::get_if<Failure>(&projected)) {
    return *failure;
  }
  const auto& projection = std::get<AssignmentProjection>(projected);
  ResourceGrant result{assignment.id, assignment.subject, projection.role, {}, assignment.version};
  if (projection.role) {
    for (const auto& resource : projection.scope) {
      for (const auto& [name, permission] : kPermissionNames) {
        if (default_role_allows(*projection.role, permission)) {
          result.scopes.push_back({resource, std::string{name}});
        }
      }
    }
  } else {
    for (const auto& binding : assignment.explicit_permissions) {
      const auto found =
          std::find_if(kPermissionNames.begin(), kPermissionNames.end(),
                       [&](const auto& entry) { return entry.second == binding.permission; });
      if (found == kPermissionNames.end()) {
        return rejected(ErrorCode::Internal, Recovery::Diagnose);
      }
      result.scopes.push_back({binding.resource, std::string{found->first}});
    }
  }
  return result;
}
// A repository supplies current grants; caller authenticates CURRENT subject before this decision.
// Workload/plugin subjects never inherit human roles. Catalog equality has no ancestor semantics.
inline Status authorize_exact(const Principal& subject, Permission permission,
                              const ResourceRef& resource,
                              std::span<const ExactAssignment> assignments) {
  if (!known_permission(permission) || !valid_policy_resource(resource)) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (subject.id.value.empty() || subject.version.value == 0 ||
      subject.state != LifecycleState::Active) {
    return rejected(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  if (!known_principal_kind(subject.kind)) {
    return rejected(ErrorCode::Internal, Recovery::Diagnose);
  }
  for (const auto& assignment : assignments) {
    if (assignment.subject != subject.id) {
      continue;
    }
    if (std::holds_alternative<Failure>(validate_exact_assignment(assignment))) {
      return rejected(ErrorCode::Internal, Recovery::Diagnose);
    }
    for (const auto& binding : assignment.explicit_permissions) {
      if (binding.resource == resource && binding.permission == permission) {
        return std::monostate{};
      }
    }
    if (subject.kind == PrincipalKind::Human) {
      for (const auto& binding : assignment.roles) {
        if (binding.resource == resource && default_role_allows(binding.role, permission)) {
          return std::monostate{};
        }
      }
    }
  }
  return rejected(ErrorCode::PermissionDenied);
}
// Explicit self-session authority only, never a generic Principal->Session inheritance rule.
inline Status authorize_session_list(const Principal& actor, const SubjectId& target,
                                     std::span<const ExactAssignment> assignments) {
  if (target.value.empty()) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (actor.id.value.empty() || actor.version.value == 0 || actor.state != LifecycleState::Active) {
    return rejected(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  if (actor.kind == PrincipalKind::Human && actor.id == target) {
    return std::monostate{};
  }
  return authorize_exact(actor, Permission::IdentitySessionRead,
                         {ResourceKind::Principal, {target.value}}, assignments);
}
inline Status authorize_session_revoke(const Principal& actor, const SessionMetadata& target,
                                       std::span<const ExactAssignment> assignments) {
  if (target.id.value.empty() || target.subject.value.empty()) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (actor.id.value.empty() || actor.version.value == 0 || actor.state != LifecycleState::Active) {
    return rejected(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  if (actor.kind == PrincipalKind::Human && actor.id == target.subject) {
    return std::monostate{};
  }
  return authorize_exact(actor, Permission::IdentitySessionRevoke,
                         {ResourceKind::Session, {target.id.value}}, assignments);
}
inline bool unique_policy_resources(std::span<const ResourceRef> scope) {
  if (scope.empty()) {
    return false;
  }
  for (std::size_t i = 0; i < scope.size(); ++i) {
    if (!valid_policy_resource(scope[i])) {
      return false;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (scope[i] == scope[j]) {
        return false;
      }
    }
  }
  return true;
}
inline Status validate_assignment_spec(const ExactAssignmentSpec& spec, const Principal& target) {
  if (spec.subject != target.id || target.id.value.empty() || !known_principal_kind(target.kind)) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (const auto* role = std::get_if<RoleAssignmentSpec>(&spec.basis)) {
    if (target.kind != PrincipalKind::Human || !known_role(role->role) ||
        !unique_policy_resources(role->scope)) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
  } else {
    const auto& explicit_spec = std::get<ExplicitAssignmentSpec>(spec.basis);
    if (!unique_policy_resources(explicit_spec.scope) || explicit_spec.permissions.empty()) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    for (std::size_t i = 0; i < explicit_spec.permissions.size(); ++i) {
      if (!known_permission(explicit_spec.permissions[i])) {
        return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
      for (std::size_t j = 0; j < i; ++j) {
        if (explicit_spec.permissions[i] == explicit_spec.permissions[j]) {
          return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
        }
      }
    }
  }
  return std::monostate{};
}
inline std::vector<ResourceRef> assignment_scope(const ExactAssignment& assignment) {
  std::vector<ResourceRef> scope;
  const auto append = [&](const ResourceRef& resource) {
    if (std::find(scope.begin(), scope.end(), resource) == scope.end()) {
      scope.push_back(resource);
    }
  };
  for (const auto& role : assignment.roles) {
    append(role.resource);
  }
  for (const auto& permission : assignment.explicit_permissions) {
    append(permission.resource);
  }
  return scope;
}
// Caller passes already authenticated/current actor. Both old and new exact scope are required,
// so an update cannot use authority over the new scope to steal an old assignment.
inline Status authorize_assignment_change(const Principal& actor, const ExactAssignment* previous,
                                          const ExactAssignmentSpec& proposed,
                                          std::span<const ExactAssignment> actor_grants) {
  std::vector<ResourceRef> scope =
      previous ? assignment_scope(*previous) : std::vector<ResourceRef>{};
  const auto& next_scope =
      std::visit([](const auto& basis) -> const std::vector<ResourceRef>& { return basis.scope; },
                 proposed.basis);
  for (const auto& resource : next_scope) {
    if (std::find(scope.begin(), scope.end(), resource) == scope.end()) {
      scope.push_back(resource);
    }
  }
  if (scope.empty()) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  for (const auto& resource : scope) {
    const auto authorized =
        authorize_exact(actor, Permission::IdentityAssignmentManage, resource, actor_grants);
    if (std::holds_alternative<Failure>(authorized)) {
      return authorized;
    }
  }
  return std::monostate{};
}
inline Outcome<ExactAssignment> prepare_assignment(AssignmentId server_or_existing_id,
                                                   const ExactAssignmentSpec& spec,
                                                   const Principal& target,
                                                   const ExactAssignment* previous,
                                                   std::optional<ResourceVersion> expected) {
  const auto validation = validate_assignment_spec(spec, target);
  if (const auto* failure = std::get_if<Failure>(&validation)) {
    return *failure;
  }
  if (server_or_existing_id.value.empty()) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  ResourceVersion version{1};
  if (previous) {
    if (!expected || previous->id != server_or_existing_id || previous->subject != spec.subject) {
      return rejected(ErrorCode::Conflict);
    }
    const auto next = next_version(previous->version, *expected);
    if (const auto* failure = std::get_if<Failure>(&next)) {
      return *failure;
    }
    version = std::get<ResourceVersion>(next);
  } else if (expected) {
    return rejected(ErrorCode::NotFound);
  }
  ExactAssignment result{std::move(server_or_existing_id), spec.subject, {}, {}, version};
  if (const auto* role = std::get_if<RoleAssignmentSpec>(&spec.basis)) {
    for (const auto& resource : role->scope) {
      result.roles.push_back({role->role, resource});
    }
  } else {
    const auto& explicit_spec = std::get<ExplicitAssignmentSpec>(spec.basis);
    for (const auto& resource : explicit_spec.scope) {
      for (const auto permission : explicit_spec.permissions) {
        result.explicit_permissions.push_back({permission, resource});
      }
    }
  }
  return result;
}
inline Outcome<AuthorizationState> assignment_changed(const AuthorizationState& current,
                                                      ResourceVersion expected) {
  if (current.subject.value.empty()) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  const auto version = next_version(current.version, expected);
  if (const auto* failure = std::get_if<Failure>(&version)) {
    return *failure;
  }
  auto changed = current;
  changed.version = std::get<ResourceVersion>(version);
  changed.invalidation_dirty = true;
  return changed;
}

} // namespace kiarabot::core::target
