#pragma once

#include <cstdint>

namespace kiarabot::core::target {

// Closed internal vocabulary mapped by NAME at the transport boundary, never by numeric cast.
// Sources: policy.proto at 0de1622 and G08 78b09a6; additions map by name, not numeric cast.
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
  PluginConfigRead,
  MessageRead
};

} // namespace kiarabot::core::target
