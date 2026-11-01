#pragma once

#include <cstdint>

namespace kiarabot::core::target {

// Closed internal vocabulary mapped by NAME at the transport boundary, never by numeric cast.
// Source: policy.proto at 0de1622. Additional G08 permissions await their unique contract commit.
enum class Permission : std::uint16_t {
  Unspecified,
  AccountManage,
  AccountPolicyManage,
  AccountPolicyRead,
  ActionExecute,
  ActionInspect,
  AdapterConnect,
  DeliveryManage,
  DeliveryProcess,
  HostExecute,
  HostRead,
  HostRegister,
  IdentityAssignmentManage,
  IdentityAssignmentRead,
  IdentityAuthorizationCheck,
  IdentityDelegationIssue,
  IdentityInvalidationRead,
  IdentityKeysRead,
  IdentityPrincipalManage,
  IdentityPrincipalRead,
  IdentitySessionCreate,
  IdentitySessionRead,
  IdentitySessionRevoke,
  ModelCallInspect,
  ModelChat,
  ModelManage,
  ModelRead,
  NamespaceManage,
  PluginConnect,
  PluginManage,
  ResourceManage,
  ResourceRead,
  StorageNamespace,
  TaskRead,
  TaskSchedule,
  TimerAccept,
  AccountRead,
  PluginRead,
  AdapterRead,
  PluginConfigRead
};

} // namespace kiarabot::core::target
