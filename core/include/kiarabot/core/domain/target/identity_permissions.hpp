#pragma once

#include <cstdint>

namespace kiarabot::core::target {

// Closed internal vocabulary mapped by NAME at the transport boundary, never by numeric cast.
// Sources: policy.proto at 0de1622 and G08 78b09a6; additions map by name, not numeric cast.
// P07 (policy.proto Permission 41-46) adds the per-kind Action grants; their string names are
// registered with the P07 implementation, not by a numeric cast here.
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
  MessageRead,
  MessageSend,
  MessageReact,
  MessageDelete,
  MemberMute,
  MemberKick,
  RequestApprove,
  // P09 shared-namespace read (wire 47); only the boundary conversion is added for now.
  NamespaceRead,
  // P12 sensitive-read permissions (wire 48/49). Control checks them through Identity
  // delegations; the internal enum only needs the boundary conversion.
  LogRead,
  AuditRead
};

} // namespace kiarabot::core::target
