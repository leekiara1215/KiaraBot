# 统一协议层设计

T03a 新增 CoreToPlugin.error（oneof 字段 6），承载注册之后的终止错误，不复用旧编号。注册拒绝仍使用 RegisterPluginResponse.error；注册前身份错误仍为 gRPC UNAUTHENTICATED/PERMISSION_DENIED。注册后的非法 ACK、权限/状态拒绝、依赖故障使用终止 Error 帧，随后结束流；断线/取消可能使该帧不可达，此时客户端保持未确认结果未知并重新注册。完整注册、订阅和 ACK 约束见 doc/03-plugin-sdk.md 的 T03a 子集。

## 责任

统一协议层负责定义所有语言和进程都能理解的事件、动作、错误、能力和版本信息。

它是 Python 适配器、C++ Core、C++ 插件和 TypeScript 面板之间唯一的共享语言。

## 契约实现状态（v1）

`proto/tests/contract_status.json` 的 schema v2 明确区分执行范围：`categories` 保留开发版本的历史/reference 状态与现有声明，**其中的 implemented 不代表六服务 target 生产能力**；`execution_scopes.target` 是当前目标生产可用性，默认 not_implemented，只有经独立证据登记的 override 才可标 implemented，deprecated 不会被复活。所有历史 categories 状态保持，不能通过迁移或改名删除旧断言。

<!-- execution-scopes:start -->
当前 target 可用项为P02进程探针、P04-A Identity主体/凭据管理gRPC面、P09a-2共享namespace管理面与P10 Scheduler任务/交接面：

P10 Scheduler 的 domain/RPC/PG16.4/三进程 mTLS 交接已本地实测通过，P10g 已完成 target override/target_spec/证据/执行范围行登记；真实 7 天 EXPIRED、多 installation 容量、真实平台触发与生产委托来源等环境项仍待验收（见 [architecture-p10-scheduler.md](../reports/modules/architecture-p10-scheduler.md)）。

| target 契约项 | 状态 | 范围 |
| --- | --- | --- |
| `operations.getProcessLiveness` | implemented | 进程HTTP live，200/HEALTHY，不代表业务可用 |
| `operations.getProcessReadiness` | implemented | 进程HTTP ready，P02固定503/UNHEALTHY，不伪造ready |
| `HostRegisterHostResponse`、`HostRegisterHostRequest.host`、`HostRegisterHostResponse.host`、`HostRegisterHostResponse.fence`、`HostRecord.host_id`、`HostRecord.version`、`HostRecord.capabilities`、`HostRecord.observed_at` | implemented | HostBridge mTLS 注册回包。运行中的 Runtime 核对登记的证书，返回当前 Host 元数据和本进程连接 Fence。 |
| `HostToRuntime.registration`、`HostToRuntime.fence`、`HostToRuntime.heartbeat`、`RuntimeToHost.registered`、`RuntimeToHost.fence`、`RuntimeToHost.heartbeat_ack`、`HostHeartbeat.sent_at`、`HostHeartbeatAck.fence`、`HostHeartbeatAck.received_at`、`HostHeartbeatAck.heartbeat_interval_seconds` | implemented | HostBridge 流的注册、心跳和确认字段。真实 Runtime 通过当前连接代次检查，拒绝旧流回写。 |
| `IdentityRegisterPrincipalRequest`、`IdentityRegisterPrincipalResponse`、`IdentityRotateCredentialRequest`、`IdentityRotateCredentialResponse`、`IdentityRevokeCredentialRequest`、`IdentityRevokeCredentialResponse`、`IdentityGetPrincipalStatusRequest`、`IdentityGetPrincipalStatusResponse`、`IdentityGetCredentialStatusRequest`、`IdentityGetCredentialStatusResponse`、`CredentialStatus`、`CredentialDelivery` | implemented | P04-A：Identity daemon gRPC 五个主体/凭据管理方法；应用/仓储/RPC已接线并经真实PG/TLS测试，仍不代表生产部署或HTTP操作已完成 |
| `IdentityRegisterPrincipalRequest.context`、`IdentityRegisterPrincipalRequest.principal_kind`、`IdentityRegisterPrincipalRequest.binding`、`IdentityRegisterPrincipalRequest.allowed_services`、`IdentityRegisterPrincipalResponse.principal`、`IdentityRegisterPrincipalResponse.initial_credential`、`IdentityRotateCredentialRequest.context`、`IdentityRotateCredentialRequest.credential_id`、`IdentityRotateCredentialRequest.expected_version`、`IdentityRotateCredentialResponse.delivery`、`IdentityRevokeCredentialRequest.context`、`IdentityRevokeCredentialRequest.credential_id`、`IdentityRevokeCredentialRequest.expected_version`、`IdentityRevokeCredentialResponse.version`、`IdentityGetPrincipalStatusRequest.context`、`IdentityGetPrincipalStatusRequest.principal_id`、`IdentityGetPrincipalStatusResponse.principal`、`IdentityGetCredentialStatusRequest.context`、`IdentityGetCredentialStatusRequest.credential_id`、`IdentityGetCredentialStatusResponse.credential`、`CredentialDelivery.credential_id`、`CredentialDelivery.version`、`CredentialDelivery.new_secret`、`CredentialDelivery.delivery_receipt_id`、`CredentialDelivery.expires_at`、`CredentialStatus.credential_id`、`CredentialStatus.principal_id`、`CredentialStatus.binding`、`CredentialStatus.version`、`CredentialStatus.state`、`CredentialStatus.created_at`、`CredentialStatus.expires_at`、`CredentialStatus.checked_at` | implemented | P04-A target-scope字段：gRPC边界转换、持久化与投影均由Identity实现；HTTP操作面仍标not_implemented |
| `IdentityIssueDelegationRequest`、`IdentityIssueDelegationRequest.context`、`IdentityIssueDelegationRequest.subject_proof`、`IdentityIssueDelegationRequest.audience`、`IdentityIssueDelegationRequest.scope`、`IdentityIssueDelegationRequest.permissions`、`IdentityIssueDelegationRequest.client_certificate_sha256`、`IdentityIssueDelegationRequest.instance_id`、`IdentityIssueDelegationResponse`、`IdentityIssueDelegationResponse.delegation`、`DelegationProof`、`DelegationProof.proof`、`DelegationProof.audience`、`DelegationProof.expires_at`、`DelegationProof.business_hops` | implemented | Identity.IssueDelegation；真实 Identity、mTLS 和 PG 验证。 |
| `IdentityExchangeDelegationRequest`、`IdentityExchangeDelegationRequest.context`、`IdentityExchangeDelegationRequest.parent_proof`、`IdentityExchangeDelegationRequest.audience`、`IdentityExchangeDelegationRequest.scope`、`IdentityExchangeDelegationRequest.permissions`、`IdentityExchangeDelegationRequest.client_certificate_sha256`、`IdentityExchangeDelegationResponse`、`IdentityExchangeDelegationResponse.delegation` | implemented | Identity.ExchangeDelegation；真实 Identity、mTLS 和 PG 验证。 |
| `IdentityCheckAuthorizationRequest`、`IdentityCheckAuthorizationRequest.context`、`IdentityCheckAuthorizationRequest.subject_proof`、`IdentityCheckAuthorizationRequest.full_method`、`IdentityCheckAuthorizationRequest.scope`、`IdentityCheckAuthorizationRequest.known_authorization_version`、`IdentityCheckAuthorizationResponse`、`IdentityCheckAuthorizationResponse.decision`、`AuthorizationDecision`、`AuthorizationDecision.allowed`、`AuthorizationDecision.authorization_version`、`AuthorizationDecision.checked_at`、`AuthorizationDecision.denial` | implemented | Identity.CheckAuthorization；真实 Identity、mTLS 和 PG 验证。 |
| `IdentityGetVerificationKeysRequest`、`IdentityGetVerificationKeysRequest.context`、`IdentityGetVerificationKeysRequest.known_key_version`、`IdentityGetVerificationKeysResponse`、`IdentityGetVerificationKeysResponse.keys`、`IdentityGetVerificationKeysResponse.version`、`VerificationKey`、`VerificationKey.key_id`、`VerificationKey.ed25519_public_key`、`VerificationKey.version`、`VerificationKey.valid_until` | implemented | Identity.GetVerificationKeys；真实 Identity、mTLS 和 PG 验证。 |
| `PrincipalRecord`、`PrincipalRecord.principal_id`、`PrincipalRecord.binding`、`PrincipalRecord.principal_kind`、`PrincipalRecord.version`、`PrincipalRecord.state`、`PrincipalRecord.roles`、`PrincipalRecord.display_permissions` | implemented | P04 ValidateSession/GetPrincipal 的当前主体投影。各资源的授权范围随 AssignmentRecord 返回。 |
| `IdentityListAssignmentsRequest`、`IdentityListAssignmentsRequest.context`、`IdentityListAssignmentsRequest.principal_id`、`IdentityListAssignmentsRequest.scope`、`IdentityListAssignmentsRequest.page`、`IdentityListAssignmentsResponse`、`IdentityListAssignmentsResponse.assignments`、`IdentityListAssignmentsResponse.page`、`AssignmentRecord`、`AssignmentRecord.assignment_id`、`AssignmentRecord.principal_id`、`AssignmentRecord.role`、`AssignmentRecord.permissions`、`AssignmentRecord.scope`、`AssignmentRecord.version` | implemented | P04 ListAssignments，按精确资源读取真实 PG 授权。 |
| `IdentitySetAssignmentRequest`、`IdentitySetAssignmentRequest.context`、`IdentitySetAssignmentRequest.assignment`、`IdentitySetAssignmentRequest.expected_version`、`IdentitySetAssignmentRequest.assignment_id`、`IdentitySetAssignmentResponse`、`IdentitySetAssignmentResponse.assignment`、`AssignmentSpec`、`AssignmentSpec.principal_id`、`AssignmentSpec.role`、`AssignmentSpec.permissions`、`AssignmentSpec.scope` | implemented | P04 Identity.SetAssignment 与作用域投影，真实 mTLS/PG。 |
| `IdentityRevokeAssignmentRequest`、`IdentityRevokeAssignmentRequest.context`、`IdentityRevokeAssignmentRequest.assignment_id`、`IdentityRevokeAssignmentRequest.expected_version`、`IdentityRevokeAssignmentResponse`、`IdentityRevokeAssignmentResponse.version` | implemented | P04 Identity.RevokeAssignment 与作用域投影，真实 mTLS/PG。 |
| `IdentityWatchInvalidationsRequest`、`IdentityWatchInvalidationsRequest.context`、`IdentityWatchInvalidationsRequest.scope`、`IdentityWatchInvalidationsRequest.cursor`、`IdentityWatchInvalidationsResponse`、`IdentityWatchInvalidationsResponse.invalidation`、`InvalidationRecord`、`InvalidationRecord.cursor`、`InvalidationRecord.principal_id`、`InvalidationRecord.authorization_version`、`InvalidationRecord.scope`、`StreamCursor`、`StreamCursor.source_service`、`StreamCursor.epoch`、`StreamCursor.scope`、`StreamCursor.sequence` | implemented | P04 Identity.WatchInvalidations 与作用域投影，真实 mTLS/PG。 |
| `IdentityGetAuthorizationSnapshotRequest`、`IdentityGetAuthorizationSnapshotRequest.context`、`IdentityGetAuthorizationSnapshotRequest.scope`、`IdentityGetAuthorizationSnapshotRequest.after`、`IdentityGetAuthorizationSnapshotResponse`、`IdentityGetAuthorizationSnapshotResponse.snapshot`、`AuthorizationSnapshot`、`AuthorizationSnapshot.cursor`、`AuthorizationSnapshot.assignments`、`AuthorizationSnapshot.authorization_version` | implemented | P04 Identity.GetAuthorizationSnapshot 与作用域投影，真实 mTLS/PG。 |
| `IdentityWatchInvalidationsResponse.resync_required` | implemented | P04 失效流断档必须显式要求全量快照；生产 Identity RPC 投影，见 P04-B 记录 |
| `IdentityWatchInvalidationsResponse.cursor` | implemented | resync 信号携带持久 epoch/sequence 水位；生产 Identity RPC 投影，见 P04-B 记录 |
| `RuntimeGetInstallationRequest`、`RuntimeGetInstallationRequest.context`、`RuntimeGetInstallationRequest.resource` | implemented | P05 既有安装与实例的精确读取，真实 Runtime/Identity mTLS 与 PG。 |
| `RuntimeGetInstallationResponse`、`RuntimeGetInstallationResponse.installation`、`RuntimeGetInstallationResponse.observed_at` | implemented | P05 既有安装与实例的精确读取，真实 Runtime/Identity mTLS 与 PG。 |
| `InstallationRecord`、`InstallationRecord.installation_id`、`InstallationRecord.plugin_type_id`、`InstallationRecord.manifest`、`InstallationRecord.version`、`InstallationRecord.state` | implemented | P05 既有安装与实例的精确读取，真实 Runtime/Identity mTLS 与 PG。 |
| `RuntimeGetInstanceRequest`、`RuntimeGetInstanceRequest.context`、`RuntimeGetInstanceRequest.resource` | implemented | P05 既有安装与实例的精确读取，真实 Runtime/Identity mTLS 与 PG。 |
| `RuntimeGetInstanceResponse`、`RuntimeGetInstanceResponse.instance`、`RuntimeGetInstanceResponse.observed_at` | implemented | P05 既有安装与实例的精确读取，真实 Runtime/Identity mTLS 与 PG。 |
| `InstanceRecord`、`InstanceRecord.instance_id`、`InstanceRecord.installation_id`、`InstanceRecord.externally_managed`、`InstanceRecord.state` | implemented | P05 既有安装与实例的精确读取，真实 Runtime/Identity mTLS 与 PG。 |
| `VersionedResourceState`、`VersionedResourceState.resource`、`VersionedResourceState.version`、`VersionedResourceState.lifecycle`、`VersionedResourceState.desired`、`VersionedResourceState.desired_config_version`、`VersionedResourceState.applied_config_version`、`VersionedResourceState.observed_at` | implemented | P05 既有安装与实例的精确读取，真实 Runtime/Identity mTLS 与 PG。 |
| `RuntimeGetInstanceConfigRequest`、`RuntimeGetInstanceConfigRequest.context`、`RuntimeGetInstanceConfigRequest.resource` | implemented | P05 既有安装与实例的精确读取，真实 Runtime/Identity mTLS 与 PG。 |
| `RuntimeGetInstanceConfigResponse`、`RuntimeGetInstanceConfigResponse.config` | implemented | P05 既有安装与实例的精确读取，真实 Runtime/Identity mTLS 与 PG。 |
| `InstanceConfiguration`、`InstanceConfiguration.instance`、`InstanceConfiguration.desired_version`、`InstanceConfiguration.values`、`InstanceConfiguration.applied_version`、`InstanceConfiguration.apply_state`、`InstanceConfiguration.observed_at` | implemented | P05 既有安装与实例的精确读取，真实 Runtime/Identity mTLS 与 PG。 |
| `RuntimeGetAccountRequest`、`RuntimeGetAccountRequest.context`、`RuntimeGetAccountRequest.resource`、`RuntimeGetAccountResponse`、`RuntimeGetAccountResponse.account`、`RuntimeGetAccountResponse.observed_at`、`RuntimeGetAdapterRequest`、`RuntimeGetAdapterRequest.context`、`RuntimeGetAdapterRequest.resource`、`RuntimeGetAdapterResponse`、`RuntimeGetAdapterResponse.adapter`、`RuntimeGetAdapterResponse.observed_at`、`RuntimeUpdateAccountRequest`、`RuntimeUpdateAccountRequest.context`、`RuntimeUpdateAccountRequest.account_id`、`RuntimeUpdateAccountRequest.display_name`、`RuntimeUpdateAccountRequest.expected_version`、`RuntimeUpdateAccountResponse`、`RuntimeUpdateAccountResponse.account` | implemented | P05 既有账号与适配器精确范围的目标 Runtime/Identity mTLS、PG16.4 验收（`p05_runtime_exact_accounts`）；其余字段按各自证据登记 |
| `AccountRecord`、`AccountRecord.account_id`、`AccountRecord.platform`、`AccountRecord.platform_account_id`、`AccountRecord.display_name`、`AccountRecord.version`、`AccountRecord.state`、`AdapterRecord`、`AdapterRecord.resource`、`AdapterRecord.version`、`AdapterRecord.last_registration`、`AdapterRecord.last_registered_at`、`AdapterRecord.observed_at`、`AdapterRecord.current_fence`、`AdapterRecord.lifecycle`、`AdapterRegistrationMetadata`、`AdapterRegistrationMetadata.implementation`、`AdapterRegistrationMetadata.implementation_version`、`AdapterRegistrationMetadata.protocol_version` | implemented | P05 既有账号与适配器精确范围的目标 Runtime/Identity mTLS、PG16.4 验收（`p05_runtime_exact_accounts`）；其余字段按各自证据登记 |
| `RuntimeGetOperationRequest` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeGetOperationResponse` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeBeginDeleteRequest` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeBeginDeleteResponse` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeRestoreResourceRequest` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeRestoreResourceResponse` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationRef` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationProgress` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `Operation` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeGetOperationRequest.context` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeGetOperationRequest.operation` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeGetOperationResponse.operation` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeBeginDeleteRequest.context` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeBeginDeleteRequest.resource` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeBeginDeleteRequest.expected_version` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeBeginDeleteResponse.operation` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeRestoreResourceRequest.context` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeRestoreResourceRequest.resource` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeRestoreResourceRequest.expected_version` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeRestoreResourceResponse.operation` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationRef.owner_service` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationRef.operation_id` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationRef.resource` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationProgress.participant` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationProgress.state` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationProgress.reason` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationProgress.next_attempt_at` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `Operation.ref` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `Operation.version` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `Operation.state` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `Operation.execution_outcome` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `Operation.participants` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `Operation.observed_at` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `Operation.reason` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `Operation.next_attempt_at` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `Operation.stalled` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationState.OPERATION_STATE_UNSPECIFIED` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationState.OPERATION_ACCEPTED` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationState.OPERATION_RUNNING` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationState.OPERATION_BLOCKED` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationState.OPERATION_SUCCEEDED` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationState.OPERATION_FAILED` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationState.OPERATION_CANCELLED` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `OperationState.OPERATION_EXPIRED` | implemented | P13a/P13c：Runtime 生命周期应用/RPC/参与者进度（P13 target 证据见模块报告） |
| `RuntimeBeginDeleteRequest/Response`、`RuntimeRestoreResourceRequest/Response`、`RuntimeGetOperationRequest/Response`、`Operation`、`OperationRef`、`OperationProgress` 及其字段、`OperationState.*`、`operations.getOperation` | implemented | P13a/P13c：Runtime 生命周期 BeginDelete/Restore/GetOperation 应用+RPC+参与者进度已接线，真 PG16.4 应用测试与真实双进程 Runtime↔Identity e2e 覆盖；Host/Scheduler 参与者真实完成需 P08b-3b/P10b 命令回执，未覆盖保持 blocked 语义 |

| `AdapterBridge.Connect`、`PluginBridge.Connect` | implemented | P06 外部 Bridge 两流：真实 TLS gRPC + 真实 Runtime/PG 进程验收注册、fence/proof、refresh、事件投递与停机；HostBridge 仍 not_implemented |
| `semantic.bridge.connection_authorization`、`semantic.bridge.external_listener`、`semantic.bridge.plugin_delivery_lease` | implemented | P06：bootstrap→ConnectionAuthorization/refresh、server-verified TLS 外部监听、插件 attempt 租约/ACK/Renew |
| `DeliveryDisposition.DELIVERY_ACKNOWLEDGED`、`DeliveryDisposition.DELIVERY_DUPLICATE`、`DeliveryDisposition.DELIVERY_STALE`、`DeliveryDisposition.DELIVERY_RENEWED`、`DeliveryDisposition.DELIVERY_REJECTED` | implemented | P06 生产投递回执映射 |
| `RegisterAdapterRequest.adapter_instance_id`、`RegisterAdapterResponse.fence`、`RegisterAdapterResponse.effective_capabilities`、`RegisterAdapterResponse.authorization`、`AdapterToCore.fence`、`AdapterToCore.connection_proof`、`AdapterToCore.refresh_authorization`、`CoreToAdapter.fence`、`CoreToAdapter.authorization_result` | implemented | P06 AdapterBridge 注册/授权/refresh 帧 |
| `RegisterPluginRequest.installation_id`、`RegisterPluginRequest.execution_generation`、`RegisterPluginResponse.fence`、`RegisterPluginResponse.desired_config_version`、`RegisterPluginResponse.authorization`、`PluginToCore.fence`、`PluginToCore.connection_proof`、`PluginToCore.renew_delivery`、`PluginToCore.refresh_authorization`、`CoreToPlugin.fence`、`CoreToPlugin.delivery_attempt`、`CoreToPlugin.delivery_receipt`、`CoreToPlugin.authorization_result`、`PluginEventAck.attempt` | implemented | P06 PluginBridge 注册/投递/ACK/续租帧 |
| `DeliveryAttempt.event_id`、`DeliveryAttempt.attempt_id`、`DeliveryAttempt.fence`、`DeliveryAttempt.started_at`、`DeliveryAttempt.lease_until`、`DeliveryAttempt.attempt_number`、`DeliveryReceipt.attempt`、`DeliveryReceipt.disposition`、`DeliveryReceipt.failure`、`ConnectionAuthorization.fence`、`ConnectionAuthorization.proof`、`ConnectionAuthorization.issued_at`、`ConnectionAuthorization.expires_at`、`ConnectionAuthorization.refresh_credential`、`ConnectionAuthorization.refresh_generation`、`ConnectionAuthorization.principal_id`、`ConnectionAuthorization.credential_version`、`ConnectionAuthorization.authorization_version`、`RefreshConnectionAuthorization.request_id`、`RefreshConnectionAuthorization.fence`、`RefreshConnectionAuthorization.proof`、`RefreshConnectionAuthorization.refresh_credential`、`RefreshConnectionAuthorization.refresh_generation`、`ConnectionAuthorizationResult.request_id`、`ConnectionAuthorizationResult.authorization`、`ConnectionAuthorizationResult.error` | implemented | P06 连接授权/投递 attempt/receipt 公共帧 |

| `CoreToAdapter.shutdown`、`CoreToPlugin.shutdown`、`ShutdownNotice.reason`、`ShutdownNotice.drain_timeout_ms`、`semantic.transport.graceful_shutdown` | implemented | P06 生产 Runtime：停止信号后向活动 bridge 流发送有界 ShutdownNotice（drain_timeout_ms=1000），真实 TLS 验收断言客户端收到该帧；reference 不发送 |

| `EventEnvelope.message_received` | implemented | P06 target：真实 NoneBot Adapter bridge client 生成、Runtime inbox 接收并 fan-out 给真实 SDK 插件；ACL/lease/ACK 见 p06_client_e2e |
| `operations.listAuditEntries`、`operations.listLogs` | implemented | P12c-6 Control 只读聚合 Identity/Runtime ListAudit/ListLogs（真实 TLS 401/400/403/503 验收 + gateway 合并/分页/过滤单测）；owner 数据与保留仍归各服务，WS logs 频道延期。OpenAPI 该 4 个带 `x-owner-rpc` 的 REST operation 仍按 target_api 规则标 `x-contract-status: not_implemented`（target 转发已实现，REST 契约 marker 不标 implemented） |
| `operations.beginDeleteResource`、`operations.restoreResource` | implemented | P12c-7 Control REST 生命周期删除/恢复：runtime owner 委托（resource.manage；account 用 account.manage）、显式 resource scope、Idempotency-Key+expected_version、202+operation ref、BLOCKED 非失败；P13a Runtime 语义 + Control gateway 单测/真实 TLS 拒绝验收。OpenAPI REST marker 按 target_api 规则保持 `x-contract-status: not_implemented` |



| `ActionEnvelope`、`ActionResult`、`PluginActionCancel`、`PluginActionQuery`、`PluginActionRequest`、`PluginActionResponse`、`ActionMetadata`、`ActionExecutionEvidence`、`ActionReceipt`、`RuntimeQueryActionResultRequest`、`RuntimeQueryActionResultResponse` | implemented | P07a-C 真实 Runtime/PG16.4/TLS 桥帧验收（submit→dispatch→result/evidence→receipt/query，含幂等/配额/重连/EXPIRED 迟到） |
| `ActionEnvelope.action_id`、`ActionEnvelope.deadline`、`ActionEnvelope.idempotency_key`、`ActionEnvelope.send_message`、`ActionEnvelope.target`、`ActionEnvelope.target_source`、`ActionEnvelope.trace_id`、`ActionResult.completed_at`、`ActionResult.error`、`ActionResult.platform_message_id`、`ActionResult.status`、`AdapterToCore.action_result`、`CoreToAdapter.action`、`CoreToPlugin.action_response`、`PluginActionCancel.action_id`、`PluginActionCancel.deadline`、`PluginActionCancel.request_id`、`PluginActionCancel.trace_id`、`PluginActionQuery.action_id`、`PluginActionQuery.deadline`、`PluginActionQuery.idempotency_key`、`PluginActionQuery.request_id`、`PluginActionQuery.trace_id`、`PluginActionRequest.action`、`PluginActionRequest.request_id`、`PluginActionResponse.error`、`PluginActionResponse.request_id`、`PluginActionResponse.result`、`PluginToCore.action`、`PluginToCore.action_cancel`、`PluginToCore.action_query`、`ActionMetadata.action_id`、`ActionMetadata.owner_instance_id`、`ActionMetadata.account_id`、`ActionMetadata.status`、`ActionMetadata.execution_outcome`、`ActionMetadata.updated_at`、`ActionEnvelope.dispatch_attempt_id`、`ActionEnvelope.dispatch_sequence`、`ActionResult.execution_outcome`、`ActionResult.dispatch_attempt_id`、`AdapterToCore.action_evidence`、`CoreToAdapter.action_receipt`、`ActionExecutionEvidence.action_id`、`ActionExecutionEvidence.dispatch_attempt_id`、`ActionExecutionEvidence.execution_outcome`、`ActionExecutionEvidence.observed_at`、`ActionExecutionEvidence.last_error`、`ActionExecutionEvidence.platform_reference`、`ActionReceipt.action_id`、`ActionReceipt.dispatch_attempt_id`、`ActionReceipt.disposition`、`ActionReceipt.error`、`ActionReceipt.result`、`ActionMetadata.kind`、`ActionMetadata.target_account_id`、`ActionMetadata.target_conversation_id`、`ActionMetadata.created_at`、`ActionMetadata.deadline`、`ActionMetadata.retain_until`、`ActionMetadata.idempotency_key`、`ActionMetadata.platform_message_id`、`ActionMetadata.dispatch_attempt_id`、`ActionMetadata.error`、`RuntimeQueryActionResultRequest.context`、`RuntimeQueryActionResultRequest.account_scope`、`RuntimeQueryActionResultRequest.action_id`、`RuntimeQueryActionResultRequest.idempotency_key`、`RuntimeQueryActionResultRequest.owner_instance_id`、`RuntimeQueryActionResultResponse.result`、`RuntimeQueryActionResultResponse.execution_outcome`、`RuntimeQueryActionResultResponse.metadata` | implemented | P07a-C 真实 Runtime/PG16.4/TLS 桥帧验收（submit→dispatch→result/evidence→receipt/query，含幂等/配额/重连/EXPIRED 迟到） |
| `ActionEnvelope.send_message` | implemented | P07a-C 真实 Runtime/PG16.4/TLS 桥帧验收（submit→dispatch→result/evidence→receipt/query，含幂等/配额/重连/EXPIRED 迟到） |
| `ModelChatRequest`、`ModelChatResponse`、`ModelMessage` | implemented | P11e 真实 TLS PluginBridge fixture（注册插件 + fence/proof + HostRequest.model_chat）：成功/重复 logical_call_id 不重复调 Provider/UNKNOWN→QUERY_SAME_INTENT/429/超时/撤销委托/校验错误；Model 侧两事务与 Provider 映射由 P11a/P11b 测试覆盖 |
| `HostRequest.model_chat`、`HostResponse.model_chat` | implemented | P11e PluginBridge ingress 真实 wire fixture；host.model.v1 协商与 model.chat 授权按 P11c/registration grants |
| `ModelChatRequest.model`、`ModelChatRequest.messages`、`ModelChatRequest.logical_call_id`、`ModelChatResponse.text`、`ModelChatResponse.input_tokens`、`ModelChatResponse.output_tokens`、`ModelMessage.role`、`ModelMessage.text` | implemented | P11e bridge fixture 字段级断言（logical_call_id 原样透传、重放复用、响应 text/tokens 映射） |

| `ActionReceiptDisposition.ACTION_RECEIPT_RECORDED`、`ActionReceiptDisposition.ACTION_RECEIPT_DUPLICATE`、`ActionReceiptDisposition.ACTION_RECEIPT_EVIDENCE_ONLY`、`Permission.PERMISSION_MESSAGE_SEND`、`Permission.PERMISSION_MESSAGE_REACT`、`Permission.PERMISSION_MESSAGE_DELETE`、`Permission.PERMISSION_MEMBER_MUTE`、`Permission.PERMISSION_MEMBER_KICK`、`Permission.PERMISSION_REQUEST_APPROVE` | implemented | P07a-C 真实 Runtime/PG16.4/TLS 桥帧验收（submit→dispatch→result/evidence→receipt/query，含幂等/配额/重连/EXPIRED 迟到） |
| `semantic.action.deadline_max_300s`、`semantic.action.result_execution_outcome`、`semantic.action.dispatch_attempt_correlation`、`semantic.action.evidence_late_result`、`semantic.action.quota_persistence` | implemented | P07a-C 真实 Runtime/PG16.4/TLS 桥帧验收（submit→dispatch→result/evidence→receipt/query，含幂等/配额/重连/EXPIRED 迟到） |
除上述明确override外，target所有非废弃业务声明均为not_implemented，包括旧reference已有的Action/Host/认证/WS能力及其余目标owner RPC（按target_spec动态计数）；mTLS监听返回UNIMPLEMENTED不算业务实现。API已有x-contract-status列表与下表、proto内联CONTRACT_STATUS保留声明/历史reference语境；不可据其单独判断target可用性。客户端与面板须依据target执行范围及真实生产者验收，不得用历史测试通过自动启用。
| `NamespaceRecord`、`StorageQuota`、`SharedMember`、`RuntimeCreateSharedNamespaceRequest`、`RuntimeCreateSharedNamespaceResponse`、`RuntimeGetSharedNamespaceRequest`、`RuntimeGetSharedNamespaceResponse`、`RuntimeSetSharedMembersRequest`、`RuntimeSetSharedMembersResponse`、`RuntimeDisableSharedNamespaceRequest`、`RuntimeDisableSharedNamespaceResponse` | implemented | P09a-2：共享 namespace 管理（Create/Get/SetMembers/Disable）纯应用+PG 四表+Runtime owner RPC 适配已接线，真 PG16.4 fixture 验证；PluginBridge storage 帧与 HTTP 操作面仍 not_implemented |
| `NamespaceRecord.created_at`、`NamespaceRecord.disabled`、`NamespaceRecord.installation_id`、`NamespaceRecord.namespace_id`、`NamespaceRecord.updated_at`、`NamespaceRecord.version`、`NamespaceScope.installation_shared`、`RuntimeCreateSharedNamespaceRequest.context`、`RuntimeCreateSharedNamespaceRequest.expected_version`、`RuntimeCreateSharedNamespaceRequest.installation_id`、`RuntimeCreateSharedNamespaceResponse.namespace`、`RuntimeDisableSharedNamespaceRequest.context`、`RuntimeDisableSharedNamespaceRequest.expected_version`、`RuntimeDisableSharedNamespaceRequest.namespace_id`、`RuntimeDisableSharedNamespaceResponse.operation`、`RuntimeDisableSharedNamespaceResponse.version`、`RuntimeGetSharedNamespaceRequest.context`、`RuntimeGetSharedNamespaceRequest.namespace_id`、`RuntimeGetSharedNamespaceResponse.members`、`RuntimeGetSharedNamespaceResponse.namespace`、`RuntimeGetSharedNamespaceResponse.quota`、`RuntimeSetSharedMembersRequest.context`、`RuntimeSetSharedMembersRequest.expected_version`、`RuntimeSetSharedMembersRequest.installation_id`、`RuntimeSetSharedMembersRequest.members`、`RuntimeSetSharedMembersRequest.namespace_id`、`RuntimeSetSharedMembersResponse.operation`、`RuntimeSetSharedMembersResponse.version`、`SharedMember.can_read`、`SharedMember.can_write`、`SharedMember.instance_id`、`StorageQuota.installation_bytes_limit`、`StorageQuota.installation_bytes_used`、`StorageQuota.installation_keys_limit`、`StorageQuota.installation_keys_used`、`StorageQuota.namespace_bytes_limit`、`StorageQuota.namespace_bytes_used`、`StorageQuota.namespace_keys_limit`、`StorageQuota.namespace_keys_used` | implemented | P09a-2 target-scope 字段：namespace 记录/成员/配额投影与 Context/version 条件由 Runtime 实现；插件 storage 帧仍 not_implemented |
| `StorageEntry`、`StorageCondition`、`StorageCompareAndSetRequest`、`StorageCompareAndSetResponse` | implemented | P09b：PluginBridge `HostRequest` storage 帧 ingress（真 TLS bridge + 真 PG16.4 e2e），私有 namespace 服务端派生、共享 condition/成员门禁、TTL/配额/收据；unary Runtime storage 方法仍不对外 |
| `HostRequest.storage_compare_and_set`、`HostResponse.storage_compare_and_set`、`NamespaceScope.instance_private`、`StorageGetRequest.scope`、`StorageGetResponse.version`、`StorageGetResponse.expires_at`、`StorageSetRequest.scope`、`StorageSetRequest.condition`、`StorageSetResponse.version`、`StorageSetResponse.expires_at`、`StorageDeleteRequest.scope`、`StorageDeleteRequest.condition`、`StorageDeleteResponse.deleted`、`StorageDeleteResponse.version`、`StorageEntry.key`、`StorageEntry.value`、`StorageEntry.version`、`StorageEntry.expires_at`、`StorageCondition.absent`、`StorageCondition.version`、`StorageCompareAndSetRequest.key`、`StorageCompareAndSetRequest.scope`、`StorageCompareAndSetRequest.value`、`StorageCompareAndSetRequest.condition`、`StorageCompareAndSetRequest.expires_at`、`StorageCompareAndSetResponse.entry` | implemented | P09b target-scope 字段：bridge 帧 scope/condition/entry/version/TTL 转换与稳定错误映射；显式共享 namespace id 与 unary storage 字段保持 not_implemented |
| `semantic.storage.scope_resolution`、`semantic.storage.cas_condition`、`semantic.storage.quota_persistent`、`semantic.storage.receipt_capacity`、`semantic.namespace.shared_membership` | implemented | P09b 语义：bridge 认证/授权+成员双门禁、CAS 条件、持久配额、收据容量/不淘汰有效行；证据见真 TLS bridge fixture |
| `ModelMetadata`、`ModelConfiguration`、`ModelCallRecord`、`ModelGetModelMetadataRequest`、`ModelGetModelMetadataResponse`、`ModelListModelMetadataRequest`、`ModelListModelMetadataResponse`、`ModelUpdateModelRequest`、`ModelUpdateModelResponse`、`ModelRotateProviderSecretRequest`、`ModelRotateProviderSecretResponse`、`ModelGetCallStatusRequest`、`ModelGetCallStatusResponse`、`ModelGetOperationRequest`、`ModelGetOperationResponse`、`ModelGetModelConfigurationRequest`、`ModelGetModelConfigurationResponse`、`ModelDisableModelRequest`、`ModelDisableModelResponse`、`ModelEnableModelRequest`、`ModelEnableModelResponse`、`ModelResolveModelCallRequest`、`ModelResolveModelCallResponse`、`ModelGatewayChatRequest`、`ModelGatewayChatResponse`、`ModelMetadata.model_id`、`ModelMetadata.alias`、`ModelMetadata.config_version`、`ModelMetadata.state`、`ModelMetadata.provider_kind`、`ModelConfiguration.model_id`、`ModelConfiguration.alias`、`ModelConfiguration.provider_kind`、`ModelConfiguration.endpoint`、`ModelConfiguration.provider_model`、`ModelConfiguration.timeout_ms`、`ModelConfiguration.version`、`ModelCallRecord.logical_call_id`、`ModelCallRecord.owner_instance_id`、`ModelCallRecord.model_id`、`ModelCallRecord.config_version`、`ModelCallRecord.state`、`ModelCallRecord.execution_outcome`、`ModelCallRecord.input_tokens`、`ModelCallRecord.output_tokens`、`ModelCallRecord.observed_at`、`ModelCallRecord.reason`、`ModelGetModelMetadataRequest.context`、`ModelGetModelMetadataRequest.model_id`、`ModelGetModelMetadataRequest.scope`、`ModelGetModelMetadataResponse.model`、`ModelListModelMetadataRequest.context`、`ModelListModelMetadataRequest.scope`、`ModelListModelMetadataRequest.page`、`ModelListModelMetadataResponse.models`、`ModelListModelMetadataResponse.page`、`ModelUpdateModelRequest.context`、`ModelUpdateModelRequest.config`、`ModelUpdateModelRequest.expected_version`、`ModelUpdateModelResponse.model`、`ModelUpdateModelResponse.operation`、`ModelRotateProviderSecretRequest.context`、`ModelRotateProviderSecretRequest.model_id`、`ModelRotateProviderSecretRequest.new_secret`、`ModelRotateProviderSecretRequest.expected_version`、`ModelRotateProviderSecretResponse.version`、`ModelRotateProviderSecretResponse.operation`、`ModelGatewayChatRequest.context`、`ModelGatewayChatRequest.logical_call_id`、`ModelGatewayChatRequest.model_id`、`ModelGatewayChatRequest.messages`、`ModelGatewayChatResponse.result`、`ModelGatewayChatResponse.call`、`ModelGetCallStatusRequest.context`、`ModelGetCallStatusRequest.logical_call_id`、`ModelGetCallStatusRequest.scope`、`ModelGetCallStatusResponse.call`、`ModelGetOperationRequest.context`、`ModelGetOperationRequest.operation`、`ModelGetOperationResponse.operation`、`ModelGetModelConfigurationRequest.context`、`ModelGetModelConfigurationRequest.model_id`、`ModelGetModelConfigurationRequest.scope`、`ModelGetModelConfigurationResponse.config`、`ModelGetModelConfigurationResponse.secret_configured`、`ModelGetModelConfigurationResponse.observed_at`、`ModelConfiguration.state`、`ModelConfiguration.max_concurrency`、`ModelCallRecord.secret_version`、`ModelCallRecord.usage_known`、`ModelCallRecord.may_have_billed`、`ModelCallRecord.provider_request_id`、`ModelCallRecord.latency_ms`、`ModelCallRecord.disposition`、`ModelGatewayChatResponse.disposition`、`ModelGetModelConfigurationResponse.secret_version`、`ModelDisableModelRequest.context`、`ModelDisableModelRequest.model_id`、`ModelDisableModelRequest.expected_version`、`ModelDisableModelResponse.model`、`ModelEnableModelRequest.context`、`ModelEnableModelRequest.model_id`、`ModelEnableModelRequest.expected_version`、`ModelEnableModelResponse.model`、`ModelResolveModelCallRequest.context`、`ModelResolveModelCallRequest.logical_call_id`、`ModelResolveModelCallRequest.expected_version`、`ModelResolveModelCallRequest.disposition`、`ModelResolveModelCallResponse.call`、`ResourceKind.RESOURCE_MODEL`、`Permission.PERMISSION_MODEL_CALL_INSPECT`、`Permission.PERMISSION_MODEL_CHAT`、`Permission.PERMISSION_MODEL_MANAGE`、`Permission.PERMISSION_MODEL_READ`、`ModelCallDisposition.MODEL_CALL_DISPOSITION_UNSPECIFIED`、`ModelCallDisposition.NOT_EXECUTED`、`ModelCallDisposition.EXECUTED_NOT_BILLED`、`ModelCallDisposition.EXECUTED_BILLED`、`ModelCallDisposition.ABANDONED`、`semantic.model.two_phase_unknown`、`semantic.model.duplicate_call_replay`、`semantic.model.request_hash`、`semantic.model.secret_rotation_pinning`、`semantic.model.provider_error_mapping`、`semantic.model.usage_known_billing`、`semantic.model.instance_delegation` | implemented | P11d：Model service 生产 RPC（P11a/b/c 应用+PG/RPC/Provider/调用者校验）与 Control REST 转发证据；ModelService 11 方法已实现，Control REST 端点仍按 operations not_implemented 另行登记。 |
| `ModelChatDisposition.MODEL_CHAT_DISPOSITION_UNSPECIFIED`、`ModelChatDisposition.FRESH_RESULT`、`ModelChatDisposition.REPLAY_IN_PROGRESS`、`ModelChatDisposition.REPLAY_TERMINAL` | implemented | P11d：ModelService Chat 的 FRESH_RESULT/REPLAY_IN_PROGRESS/REPLAY_TERMINAL 处置由 model_rpc + 重复调用重放测试证据支持；插件 HostRequest.model_chat 的 target 仍另行登记。 |
| `operations.updateModel`、`operations.getModelMetadata`、`operations.getModelConfiguration`、`operations.rotateProviderSecret`、`operations.disableModel`、`operations.enableModel`、`operations.getModelCallStatus`、`operations.resolveModelCall`、`operations.listModels` | implemented | P11d-2：Control Model 管理/查询 REST 转发（metadata/list/lifecycle/configuration/secret/call UNKNOWN/resolve）经真实 Control 进程 + 真 TLS Identity/Model fixture 验收；`operations.getOperation` 仍 not_implemented（仅 model owner 分支已接线）。 |

| `IdentityAuditFilter`、`IdentityAuditRecord`、`IdentityListAuditRequest`、`IdentityListAuditResponse`、`IdentityListLogsRequest`、`IdentityListLogsResponse`、`IdentityLogRecord`、`RuntimeAuditFilter`、`RuntimeAuditRecord`、`RuntimeListAuditRequest`、`RuntimeListAuditResponse`、`RuntimeListLogsRequest`、`RuntimeListLogsResponse`、`RuntimeLogRecord`、`IdentityAuditFilter.actor`、`IdentityAuditFilter.occurred_from`、`IdentityAuditFilter.occurred_until`、`IdentityAuditFilter.page`、`IdentityAuditFilter.resource`、`IdentityAuditFilter.scope`、`IdentityAuditRecord.actor`、`IdentityAuditRecord.authorization_version`、`IdentityAuditRecord.entry_id`、`IdentityAuditRecord.method`、`IdentityAuditRecord.occurred_at`、`IdentityAuditRecord.rejection`、`IdentityAuditRecord.request_id`、`IdentityAuditRecord.resource`、`IdentityAuditRecord.target_account_id`、`IdentityAuditRecord.target_conversation_id`、`IdentityAuditRecord.trace_id`、`IdentityListAuditRequest.context`、`IdentityListAuditRequest.filter`、`IdentityListAuditResponse.checked_at`、`IdentityListAuditResponse.entries`、`IdentityListAuditResponse.page`、`IdentityListLogsRequest.context`、`IdentityListLogsRequest.filter`、`IdentityListLogsResponse.checked_at`、`IdentityListLogsResponse.entries`、`IdentityListLogsResponse.page`、`IdentityLogRecord.actor`、`IdentityLogRecord.component`、`IdentityLogRecord.entry_id`、`IdentityLogRecord.message`、`IdentityLogRecord.method`、`IdentityLogRecord.occurred_at`、`IdentityLogRecord.request_id`、`IdentityLogRecord.resource`、`IdentityLogRecord.severity`、`IdentityLogRecord.trace_id`、`RuntimeAuditFilter.actor`、`RuntimeAuditFilter.occurred_from`、`RuntimeAuditFilter.occurred_until`、`RuntimeAuditFilter.page`、`RuntimeAuditFilter.resource`、`RuntimeAuditFilter.scope`、`RuntimeAuditRecord.actor`、`RuntimeAuditRecord.authorization_version`、`RuntimeAuditRecord.entry_id`、`RuntimeAuditRecord.method`、`RuntimeAuditRecord.occurred_at`、`RuntimeAuditRecord.rejection`、`RuntimeAuditRecord.request_id`、`RuntimeAuditRecord.resource`、`RuntimeAuditRecord.target_account_id`、`RuntimeAuditRecord.target_conversation_id`、`RuntimeAuditRecord.trace_id`、`RuntimeListAuditRequest.context`、`RuntimeListAuditRequest.filter`、`RuntimeListAuditResponse.checked_at`、`RuntimeListAuditResponse.entries`、`RuntimeListAuditResponse.page`、`RuntimeListLogsRequest.context`、`RuntimeListLogsRequest.filter`、`RuntimeListLogsResponse.checked_at`、`RuntimeListLogsResponse.entries`、`RuntimeListLogsResponse.page`、`RuntimeLogRecord.actor`、`RuntimeLogRecord.component`、`RuntimeLogRecord.entry_id`、`RuntimeLogRecord.message`、`RuntimeLogRecord.method`、`RuntimeLogRecord.occurred_at`、`RuntimeLogRecord.request_id`、`RuntimeLogRecord.resource`、`RuntimeLogRecord.severity`、`RuntimeLogRecord.trace_id` | implemented | P12e：Identity/Runtime 只读 owner audit/log 查询投影（audit.read/log.read + Control catalog scope，真实 PG16.4 keyset 分页/过滤/脱敏测试）；OpenAPI /logs /audit 聚合仍 not_implemented，Control 接线由 P12c 后续完成 |
| `TaskRecord`、`OccurrenceRecord`、`SchedulerCreateTaskRequest`、`SchedulerCreateTaskResponse`、`SchedulerCancelTaskRequest`、`SchedulerCancelTaskResponse`、`SchedulerGetTaskRequest`、`SchedulerGetTaskResponse`、`SchedulerListTasksRequest`、`SchedulerListTasksResponse`、`SchedulerGetOccurrenceRequest`、`SchedulerGetOccurrenceResponse`、`SchedulerGetOperationRequest`、`SchedulerGetOperationResponse` | implemented | P10g：P10a Scheduler 任务/occurrence 观测与 owner mTLS SchedulerService 六方法；应用/RPC/PG v9/三进程真 mTLS 交接已实测（见 [architecture-p10-scheduler.md](../reports/modules/architecture-p10-scheduler.md)） |
| `TaskRecord.task_id`、`TaskRecord.owner_instance_id`、`TaskRecord.installation_id`、`TaskRecord.target`、`TaskRecord.payload`、`TaskRecord.run_at`、`TaskRecord.state`、`TaskRecord.version`、`TaskRecord.occurrence_id`、`OccurrenceRecord.occurrence_id`、`OccurrenceRecord.task_id`、`OccurrenceRecord.state`、`OccurrenceRecord.handoff`、`OccurrenceRecord.accepted_event_id`、`OccurrenceRecord.acknowledged_at`、`SchedulerCreateTaskRequest.context`、`SchedulerCreateTaskRequest.task_id`、`SchedulerCreateTaskRequest.target`、`SchedulerCreateTaskRequest.payload`、`SchedulerCreateTaskRequest.run_at`、`SchedulerCreateTaskRequest.local_time`、`SchedulerCreateTaskResponse.task`、`SchedulerCancelTaskRequest.context`、`SchedulerCancelTaskRequest.task_id`、`SchedulerCancelTaskRequest.scope`、`SchedulerCancelTaskResponse.task`、`SchedulerGetTaskRequest.context`、`SchedulerGetTaskRequest.task_id`、`SchedulerGetTaskRequest.scope`、`SchedulerGetTaskResponse.task`、`SchedulerListTasksRequest.context`、`SchedulerListTasksRequest.scope`、`SchedulerListTasksRequest.page`、`SchedulerListTasksResponse.tasks`、`SchedulerListTasksResponse.page`、`SchedulerGetOccurrenceRequest.context`、`SchedulerGetOccurrenceRequest.occurrence_id`、`SchedulerGetOccurrenceRequest.scope`、`SchedulerGetOccurrenceResponse.occurrence`、`SchedulerGetOperationRequest.context`、`SchedulerGetOperationRequest.operation`、`SchedulerGetOperationResponse.operation`、`TaskRecord.created_at`、`TaskRecord.updated_at`、`TaskRecord.cancel_requested`、`TaskRecord.reason`、`OccurrenceRecord.attempts`、`OccurrenceRecord.last_error`、`OccurrenceRecord.created_at`、`OccurrenceRecord.updated_at`、`SchedulerListTasksRequest.state`、`TimerTriggered.occurrence_id` | implemented | P10g：上述 Scheduler 请求/响应与 TaskRecord/OccurrenceRecord 字段 + TimerTriggered.occurrence_id 由 Scheduler/Runtime 持久化与投影实现，证据见 P10 模块报告 |
| `semantic.scheduler.owner_derivation`、`semantic.scheduler.cancel_window`、`semantic.scheduler.handoff_expiry`、`semantic.scheduler.timer_occurrence_identity` | implemented | P10g：owner 派生/取消窗口/交接过期/occurrence 身份规则由 Scheduler/Runtime 应用实现，p10_scheduler_service 与 p10_timer_e2e 覆盖 |
| `operations.getTask` | implemented | P10h：Control Scheduler 任务 REST GET /api/v1/tasks/{id} 成功链经真实 Control HTTPS/TLS + Scheduler owner fixture 验收（`tests/architecture/p10_scheduler_rest.py`，CTest p10_scheduler_rest）；owner scope/task 投影转发，不替代真实 Scheduler/PG 进程 |
| `operations.cancelTask` | implemented | P10h：Control POST /api/v1/tasks/{id}/cancel 成功链（200 + owner task 结果、同 key 重复 cancel、owner CONFLICT、Control NOT_FOUND）经真实 Control HTTPS/TLS + owner fixture 验收；REST 契约响应为 200 而非 202，契约未改 |
| `operations.getOccurrence` | implemented | P10i：Control `GET /api/v1/occurrences/{id}` 经 Scheduler owner `GetOccurrence` 转发，显式单资源 `task.read` 委派 scope；真实 Control HTTPS/TLS + Scheduler owner fixture 成功链（`tests/architecture/p10_scheduler_rest.py`，CTest `p10_scheduler_rest`）覆盖投影、400/401/409 与 owner 拒绝；REST OpenAPI marker 仍按 owner-RPC 规则标 not_implemented，target 执行范围已翻转 |
| `ReactMessage`、`DeleteMessage`、`MuteMember`、`KickMember`、`ApproveRequest`、`ActionPolicy`、`RuntimeGetActionPolicyRequest`、`RuntimeGetActionPolicyResponse`、`RuntimeUpdateActionPolicyRequest`、`RuntimeUpdateActionPolicyResponse` | implemented | P07f-2 target flip：Action payload 五种（react/delete/mute/kick/approve）与 ActionPolicy owner RPC 消息已由 target Runtime 生产；P07f-1 真实 TLS/PG 桥接 policy gate 与 dispatch→result→receipt 证据，P07f-2 提交此执行范围登记 |
| `ApproveRequest.approve`、`ApproveRequest.reason`、`ApproveRequest.request_id`、`DeleteMessage.conversation`、`DeleteMessage.message_id`、`KickMember.group_id`、`KickMember.reason`、`KickMember.user_id`、`MuteMember.duration_seconds`、`MuteMember.group_id`、`MuteMember.user_id`、`ReactMessage.conversation`、`ReactMessage.message_id`、`ReactMessage.reaction` | implemented | P07f-2 target flip：Action payload 字段的 wire/域校验与适配器 dispatch 已实现（真实 bridge acceptance/adapter e2e 证据） |
| `ActionPolicy.account_id`、`ActionPolicy.mute_enabled`、`ActionPolicy.kick_enabled`、`ActionPolicy.allowed_group_ids`、`ActionPolicy.mute_daily_budget`、`ActionPolicy.kick_daily_budget`、`ActionPolicy.version`、`RuntimeGetActionPolicyRequest.context`、`RuntimeGetActionPolicyRequest.account_id`、`RuntimeGetActionPolicyResponse.policy`、`RuntimeUpdateActionPolicyRequest.context`、`RuntimeUpdateActionPolicyRequest.policy`、`RuntimeUpdateActionPolicyRequest.expected_version`、`RuntimeUpdateActionPolicyResponse.policy` | implemented | P07f-2 target flip：ActionPolicy 消息/字段与 RuntimeService Get/UpdateActionPolicy owner RPC 接线（P07f-1 RPC/应用源 + policy gate 测试）；Control REST 转发仍 not_implemented |
| `ActionEnvelope.react_message`、`ActionEnvelope.delete_message`、`ActionEnvelope.mute_member`、`ActionEnvelope.kick_member`、`ActionEnvelope.approve_request` | implemented | P07f-2 target flip：ActionEnvelope 五种 payload oneof 的 target producer/consumer 已实现 |
| `operations.getActionPolicy`、`operations.updateActionPolicy` | implemented | P07f-2 target flip：RuntimeService Get/UpdateActionPolicy owner 方法已接线；Control REST operation 仍 not_implemented |
| `IdentityPrincipalSummary`、`IdentityListPrincipalsRequest`、`IdentityListPrincipalsResponse`、`IdentityPrincipalSummary.principal_id`、`IdentityPrincipalSummary.binding`、`IdentityPrincipalSummary.principal_kind`、`IdentityPrincipalSummary.version`、`IdentityPrincipalSummary.state`、`IdentityPrincipalSummary.roles`、`IdentityListPrincipalsRequest.context`、`IdentityListPrincipalsRequest.page`、`IdentityListPrincipalsRequest.state_filter`、`IdentityListPrincipalsResponse.principals`、`IdentityListPrincipalsResponse.page`、`IdentityListPrincipalsResponse.checked_at`、`operations.listPrincipals` | implemented | P12l：Identity `ListPrincipals` 人类主体目录（Control platform peer、identity.principal.read、获授资源范围过滤、principal-id keyset 分页、checked_at/roles）经真实 PG16.4 + 真 mTLS RPC fixture 与 Control 转发验收；OpenAPI x-target-contract-status 按认证投影规则仍 not_implemented，REST x-contract-status 已翻 |
| `IdentityLoginRequest`、`IdentityLoginRequest.context`、`IdentityLoginRequest.username`、`IdentityLoginRequest.password` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
| `IdentityLoginResponse`、`IdentityLoginResponse.tokens`、`IdentityLoginResponse.checked_at` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
| `IdentityRefreshSessionRequest`、`IdentityRefreshSessionRequest.context`、`IdentityRefreshSessionRequest.refresh_token` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
| `IdentityRefreshSessionResponse`、`IdentityRefreshSessionResponse.tokens`、`IdentityRefreshSessionResponse.checked_at` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
| `IdentityValidateSessionRequest`、`IdentityValidateSessionRequest.context`、`IdentityValidateSessionRequest.access_token` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
| `IdentityValidateSessionResponse`、`IdentityValidateSessionResponse.principal`、`IdentityValidateSessionResponse.session`、`IdentityValidateSessionResponse.assignments`、`IdentityValidateSessionResponse.authorization_version`、`IdentityValidateSessionResponse.checked_at` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
| `IdentityGetPrincipalRequest`、`IdentityGetPrincipalRequest.context`、`IdentityGetPrincipalRequest.access_token` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
| `IdentityGetPrincipalResponse`、`IdentityGetPrincipalResponse.principal`、`IdentityGetPrincipalResponse.assignments`、`IdentityGetPrincipalResponse.authorization_version`、`IdentityGetPrincipalResponse.checked_at` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
| `IdentityListSessionsRequest`、`IdentityListSessionsRequest.context`、`IdentityListSessionsRequest.principal_id`、`IdentityListSessionsRequest.page` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
| `IdentityListSessionsResponse`、`IdentityListSessionsResponse.sessions`、`IdentityListSessionsResponse.page`、`IdentityListSessionsResponse.checked_at` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
| `IdentityRevokeSessionRequest`、`IdentityRevokeSessionRequest.context`、`IdentityRevokeSessionRequest.session_id` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
| `IdentityRevokeSessionResponse`、`IdentityRevokeSessionResponse.session`、`IdentityRevokeSessionResponse.checked_at` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
| `SessionRecord`、`SessionRecord.session_id`、`SessionRecord.principal_id`、`SessionRecord.rotation`、`SessionRecord.expires_at`、`SessionRecord.revoked`、`SessionRecord.created_at` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
| `TokenPair`、`TokenPair.session_id`、`TokenPair.access_token`、`TokenPair.refresh_token`、`TokenPair.access_expires_at`、`TokenPair.refresh_expires_at`、`TokenPair.rotation` | implemented | P04 会话 RPC，真实 Control mTLS、Identity、PG 与字段边界测试。 |
<!-- execution-scopes:end -->

**P13h-1 Scheduler 平台 peer 授权语义注记（2026-11-03）**：`SchedulerService.ListTasks` 与
`CancelTask` 在原有 “Runtime with instance delegation or scoped Control” caller 之外，新增
**仅这两个方法**的 Runtime 平台 peer 路径：真实入站 mTLS 叶 SAN `spiffe://kiarabot/runtime…`
命中 Scheduler 部署级 `runtime_peers` allowlist（证书 SHA-256 + instance/installation/account +
`task.read`/`task.schedule`）后，不需要用户 delegation；ListTasks 只返回该 instance 且同
installation/account 的任务，CancelTask 额外核对 task owner/installation/account，跨实例/
跨安装/跨账号拒绝。其他 Scheduler 方法、以及这两个方法的 Control/用户 delegation 路径保持原
语义。无新增 proto 字段/权限编号/RPC 集合。


### P12k REST marker 审计（2026-11-02，main@3f43c8f）

REST `x-contract-status` 不是 target 执行范围的第二份状态：带 `x-owner-rpc` 的 operation 按
`target_api` 规则保持 `not_implemented`（即使 target override/证据为 implemented，共 59 项，含全部
Model 管理 REST）；无 `x-owner-rpc` 且有真实成功链证据的 `listModels` 已在本任务翻为 implemented
（target `operations.listModels` override/evidence 保持一致）。分类与脚本见 `doc/06-control-api.md`
P12k 节与 `scripts/quality/check-contract-status-consistency.rb`；C 类 19 项无证据保持原状。

### P05f 核对注记（2026-11-02，main@439fb9e）

P05f 只读核对了 Identity/Runtime 的 target 状态并补退出证据，未修改 proto/OpenAPI/contract_status。结论：
P04-A 管理面与 P12e ListAudit/ListLogs 已登记 implemented；P04 认证链、
P05 Runtime 资源方法、Identity ListPrincipals 等有本机测试证据但 target 未翻（逐项 evidence 未登记/
生产环境和 P05b 未验收），清单与理由见 `reports/modules/architecture-p04-identity.md` 与
`architecture-p05-runtime.md` 的 P05f 段。`bootstrap_credential` 字段级、P05b 实例委托与
`HostRequest.storage_compare_and_set` 仍保持 not_implemented。

检查器继续核对每个Proto字段/oneof、枚举、频道/事件、OpenAPI操作及原未实现项的内联注释、API列表、文档行和证据，不删除原门禁。历史Core逻辑证据路径只通过`core/reference/manifest.json`的明确source→reference映射解析并核对Git blob，未登记/缺失/字节变化/重引入/路径逃逸或符号链接均失败。target已实现项另需当前生产source及target测试，reference、旧客户端或测试内字符串不能证明target生产能力。

<!-- contract-status:start -->
| 契约项 | 状态 | 说明 |
| --- | --- | --- |
| `EventEnvelope.message_sent`、`EventEnvelope.group_member_changed`、`EventEnvelope.plugin_status_changed` | not_implemented | 唯一适配器只产生 `MessageReceived`、`FriendRequest` 与 `AdapterStatusChanged`；Core 对其余事件只有接收/订阅映射。插件订阅这些事件收不到数据。 |
| `ActionEnvelope.schedule_task`、`ScheduleTask.task_id`、`ScheduleTask.schedule`、`ScheduleTask.payload` | deprecated | 旧 Action 调度与 `host.tasks.v1` 重复且语义不等价（见 `doc/13`），v1 继续以 `CAPABILITY_NOT_SUPPORTED` 拒绝；调度只走 `host.tasks.v1`。 |
| `SendMessage.reply`、`SendMessage.reply_to_message_id` | not_implemented | Core 与适配器都显式拒绝；v1 的 Action 面包含 `send_message`、`react_message`、`delete_message`、`mute_member`、`kick_member` 与 `approve_request`。 |
| `MessageSegment.Type.AUDIO`、`MessageSegment.Type.VIDEO`、`MessageSegment.Type.FILE`、`MessageSegment.Type.LOCATION`、`ConversationRef.Type.CHANNEL`、`PluginState.PLUGIN_STATE_STARTING`、`PluginState.PLUGIN_STATE_STOPPING` | not_implemented | 适配器把非 text/image/at/reply 段降级为 `UNKNOWN`；CHANNEL 会话被拒绝；运行时状态机不产生 STARTING/STOPPING。 |
| `Error.details`、`Account.display_name` | not_implemented | 无生产/消费点；REST `Error.details` 保持空对象。 |
| `RuntimeBindAdapterResponse.bootstrap_credential`、`RuntimeCreateInstanceResponse.bootstrap_credential` | not_implemented | P06 外部 Bridge 一次性 bootstrap 凭据（P05 在 BindAdapter/CreateInstance 时生成并只返回一次；CreateInstance 仅 externally_managed=true）；Runtime 只存 SHA-256，调用方不得持久化或记录，P06 实现前不返回。 |
| `PageRequest.page_size`、`PageRequest.page_token`、`PageInfo.next_page_token`、`PageInfo.page_size`、`Health.state`、`Health.component`、`Health.version`、`Health.checked_at`、`Health.dependencies` | not_implemented | REST 使用 OpenAPI 自己的分页/健康 schema，v1 无代码引用这些 Protobuf 消息。 |
| `PluginManifest.dependencies`、`PluginManifest.config_schema` | not_implemented | 非空值被 `CAPABILITY_NOT_SUPPORTED` 拒绝；`config_schema` 只接受空串或 `{}`。 |
| `CoreToAdapter.shutdown`、`CoreToPlugin.shutdown`、`ShutdownNotice.reason`、`ShutdownNotice.drain_timeout_ms`、`semantic.transport.graceful_shutdown` | not_implemented | 帧已定义但 v1 Core 不发送；关闭流仍按“结果未知”处理，见“优雅停机”。 |
| `semantic.stream.plugin_updated_health_state` | not_implemented | `plugin.updated` 只推送 `state`，不推送 `health_state`。 |
| `events.health.updated` | not_implemented | health 投影固定 `HEALTHY` 单组件，事件永不产生；`/ws/health` 只会停在 SESSION_READY。 |
| `operations.getActionByIdempotencyKey`、`operations.getActionResult`、`operations.listGroups` | not_implemented | REST Action 查询没有写入方（无 `control/<user>` owner），恒 404；`/groups` 无 Groups 分支，恒空页。 |
| `semantic.action.deadline_max_300s`、`semantic.action.target_source_id_charset` | not_implemented | deadline 只校验正时间戳，无 300 秒上限；Action 的 `target_source` 用比入口更窄的字符集。 |
| `semantic.adapter.backpressure_retryable_rate_limited` | not_implemented | 契约要求队列满返回可重试 `RATE_LIMITED`，实现返回 `retryable=false` 且不关流。 |
| `semantic.bridge.connection_authorization`、`semantic.bridge.external_listener`、`semantic.bridge.plugin_delivery_lease` | not_implemented | P06 目标：Runtime 外部 Bridge 监听（server-verified TLS）、bootstrap→`ConnectionAuthorization`/fence/proof/refresh、插件投递 attempt 租约与 `DELIVERY_STALE`/`DELIVERY_DUPLICATE`；reference 无外部监听、连接授权和 attempt，P06 实现后按 target scope 翻转。 |
| `semantic.host.unknown_operation_error` | not_implemented | 契约要求未知 Host operation 返回 `CAPABILITY_NOT_SUPPORTED`，实现先返回 `INVALID_ARGUMENT`。 |
| `semantic.rest.action_query_terminal_retention`、`semantic.rest.action_query_deadline_expired` | not_implemented | REST 查询未实现 168 小时终态保留与到期 `EXPIRED` 迁移；语义已冻结，待 control-api 接线。 |
| `semantic.grpc.business_error_mapping` | deprecated | 旧文档的 `CONFLICT→ALREADY_EXISTS/ABORTED` 等业务错误映射已废止；业务错误以 in-band `Error` 帧为准，见“错误模型”。 |
| `semantic.rest.admin_role_wildcard` | deprecated | 旧 `admin: ['*']` 通配授权已废止；角色改为显式权限列表，任何默认角色都不含 `message.read`。 |
| `GroupMemberChanged` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `MessageSent` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginStatusChanged` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `GroupMemberChanged.change_type` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `GroupMemberChanged.group_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `GroupMemberChanged.user_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `MessageSent.action_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `MessageSent.conversation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `MessageSent.message` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `MessageSent.message_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginStatusChanged.lifecycle_state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginStatusChanged.plugin_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginStatusChanged.reason` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginStatusChanged.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginStatusChanged.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RegisterAdapterRequest.adapter_instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RegisterAdapterRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RegisterAdapterResponse.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RegisterAdapterResponse.effective_capabilities` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AdapterToCore.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `CoreToAdapter.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RequestContext.request_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RequestContext.trace_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RequestContext.deadline` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RequestContext.idempotency_key` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RequestContext.delegation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceRef.kind` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceRef.id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceScope.resources` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Fence.instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Fence.execution_generation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Fence.connection_generation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Fence.runtime_epoch` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `StreamCursor.source_service` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `StreamCursor.epoch` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `StreamCursor.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `StreamCursor.sequence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationRef.owner_service` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationRef.operation_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationRef.resource` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `FailureContext.acceptance` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `FailureContext.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `FailureContext.recovery` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `FailureContext.retry_after_ms` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `FailureContext.current_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RpcFailure.error` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RpcFailure.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationProgress.participant` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationProgress.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationProgress.reason` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationProgress.next_attempt_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Operation.ref` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Operation.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Operation.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Operation.execution_outcome` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Operation.participants` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Operation.observed_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Operation.reason` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Operation.next_attempt_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Operation.stalled` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HandoffContext.source_service` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HandoffContext.message_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HandoffContext.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HandoffContext.causation_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HandoffContext.resource` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HandoffContext.resource_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HandoffContext.payload_sha256` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HandoffContext.created_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HandoffContext.valid_until` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryAttempt.event_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryAttempt.attempt_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryAttempt.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryAttempt.started_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryAttempt.lease_until` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryAttempt.attempt_number` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryReceipt.attempt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryReceipt.disposition` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryReceipt.failure` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VersionedResourceState.resource` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VersionedResourceState.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VersionedResourceState.lifecycle` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VersionedResourceState.desired` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VersionedResourceState.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VersionedResourceState.registered` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VersionedResourceState.desired_config_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VersionedResourceState.applied_config_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VersionedResourceState.process_outcome` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VersionedResourceState.observed_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VersionedResourceState.source_service` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SnapshotPage.page_size` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SnapshotPage.page_token` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SnapshotPage.snapshot_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SnapshotPage.next_page_token` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SnapshotPage.expires_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RegisterPluginRequest.installation_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RegisterPluginRequest.execution_generation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RegisterPluginRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RegisterPluginResponse.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RegisterPluginResponse.desired_config_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginToCore.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginToCore.renew_delivery` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `CoreToPlugin.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `CoreToPlugin.delivery_attempt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `CoreToPlugin.delivery_receipt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginEventAck.attempt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginConfig.config_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginConfig.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginConfigAck.config_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginConfigAck.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionStatus.ACTION_STATUS_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionStatus.ACCEPTED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionStatus.SUCCEEDED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionStatus.FAILED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionStatus.EXPIRED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionStatus.CANCELLED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AdapterEventAck.Status.STATUS_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AdapterEventAck.Status.PERSISTED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AdapterEventAck.Status.DUPLICATE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AdapterEventAck.Status.REJECTED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ErrorCode.ERROR_CODE_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ErrorCode.INVALID_ARGUMENT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ErrorCode.UNAUTHENTICATED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ErrorCode.PERMISSION_DENIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ErrorCode.NOT_FOUND` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ErrorCode.CONFLICT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ErrorCode.CAPABILITY_NOT_SUPPORTED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ErrorCode.RATE_LIMITED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ErrorCode.TIMEOUT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ErrorCode.UPSTREAM_UNAVAILABLE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ErrorCode.PLUGIN_UNAVAILABLE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ErrorCode.INTERNAL` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HealthState.HEALTH_STATE_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HealthState.HEALTHY` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HealthState.DEGRADED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HealthState.UNHEALTHY` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `EventDeliveryMode.EVENT_DELIVERY_MODE_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `EventDeliveryMode.AT_LEAST_ONCE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ConversationRef.Type.TYPE_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ConversationRef.Type.PRIVATE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ConversationRef.Type.GROUP` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `MessageSegment.Type.SEGMENT_TYPE_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `MessageSegment.Type.TEXT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `MessageSegment.Type.IMAGE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `MessageSegment.Type.MENTION` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `MessageSegment.Type.REPLY` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `MessageSegment.Type.UNKNOWN` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `GroupMemberChanged.ChangeType.CHANGE_TYPE_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `GroupMemberChanged.ChangeType.JOINED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `GroupMemberChanged.ChangeType.LEFT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `GroupMemberChanged.ChangeType.PROMOTED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `GroupMemberChanged.ChangeType.DEMOTED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_KIND_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_ACCOUNT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_ADAPTER` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_PLUGIN_TYPE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_INSTALLATION` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_INSTANCE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_HOST` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_NAMESPACE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_TASK` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_MODEL` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_PRINCIPAL` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_SESSION` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_ACTION` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_OPERATION` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_CREDENTIAL` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ResourceKind.RESOURCE_ASSIGNMENT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationState.OPERATION_STATE_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationState.OPERATION_ACCEPTED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationState.OPERATION_RUNNING` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationState.OPERATION_BLOCKED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationState.OPERATION_SUCCEEDED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationState.OPERATION_FAILED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationState.OPERATION_CANCELLED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OperationState.OPERATION_EXPIRED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionOutcome.EXECUTION_OUTCOME_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionOutcome.EXECUTION_NOT_STARTED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionOutcome.EXECUTION_KNOWN_SUCCEEDED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionOutcome.EXECUTION_KNOWN_FAILED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionOutcome.EXECUTION_UNKNOWN` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AcceptanceState.ACCEPTANCE_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AcceptanceState.NOT_ACCEPTED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AcceptanceState.DURABLY_ACCEPTED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AcceptanceState.ACCEPTANCE_UNKNOWN` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RecoveryAction.RECOVERY_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RecoveryAction.CORRECT_INPUT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RecoveryAction.REAUTHENTICATE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RecoveryAction.DO_NOT_RETRY` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RecoveryAction.QUERY_SAME_INTENT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RecoveryAction.RETRY_SAME_ID` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RecoveryAction.RESYNC_SNAPSHOT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RecoveryAction.MANUAL_REVIEW` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `LifecycleState.LIFECYCLE_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `LifecycleState.LIFECYCLE_ACTIVE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `LifecycleState.LIFECYCLE_DISABLED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `LifecycleState.LIFECYCLE_DELETING` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `LifecycleState.LIFECYCLE_DELETED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `LifecycleState.LIFECYCLE_RESTORING` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `LifecycleState.LIFECYCLE_PURGED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DesiredState.DESIRED_STATE_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DesiredState.DESIRED_RUNNING` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DesiredState.DESIRED_STOPPED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `FixedRole.FIXED_ROLE_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `FixedRole.ROLE_ADMIN` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `FixedRole.ROLE_OPERATOR` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `FixedRole.ROLE_VIEWER` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryDisposition.DELIVERY_DISPOSITION_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryDisposition.DELIVERY_ACKNOWLEDGED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryDisposition.DELIVERY_DUPLICATE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryDisposition.DELIVERY_STALE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryDisposition.DELIVERY_RENEWED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeliveryDisposition.DELIVERY_REJECTED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginState.PLUGIN_STATE_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginState.PLUGIN_STATE_DISABLED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginState.PLUGIN_STATE_READY` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginState.PLUGIN_STATE_RUNNING` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginState.PLUGIN_STATE_STOPPED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginState.PLUGIN_STATE_UNAVAILABLE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginState.PLUGIN_STATE_FAILED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ScheduledTaskState.SCHEDULED_TASK_STATE_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ScheduledTaskState.SCHEDULED_TASK_PENDING` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ScheduledTaskState.SCHEDULED_TASK_RUNNING` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ScheduledTaskState.SCHEDULED_TASK_RETRYING` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ScheduledTaskState.SCHEDULED_TASK_COMPLETED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ScheduledTaskState.SCHEDULED_TASK_FAILED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ScheduledTaskState.SCHEDULED_TASK_CANCELLED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |

| `HostPackage` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostRecord` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ProcessSpec` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionRecord` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostCommandReceipt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostRegisterHostRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostRegisterHostResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostReportInventoryRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostReportInventoryResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostApplyDesiredProcessRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostApplyDesiredProcessResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostStopExecutionRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostStopExecutionResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostDrainHostRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostDrainHostResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostGetExecutionRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostGetExecutionResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostWatchExecutionsRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostWatchExecutionsResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostToRuntime` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeToHost` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PrincipalRecord` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SessionRecord` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TokenPair` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `CredentialDelivery` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AssignmentRecord` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DelegationProof` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AuthorizationDecision` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InvalidationRecord` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AuthorizationSnapshot` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VerificationKey` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityLoginRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityLoginResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRefreshSessionRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRefreshSessionResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityValidateSessionRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityValidateSessionResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetPrincipalRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetPrincipalResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListSessionsRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListSessionsResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeSessionRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeSessionResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRegisterPrincipalRequest` | not_implemented | P04-A 应用/PG/RPC 已接线（实现中），待完整门禁与真实 PG/TLS 验收后翻转；见 doc/02 P04 章节与模块报告。 |
| `IdentityRegisterPrincipalResponse` | not_implemented | P04-A 应用/PG/RPC 已接线（实现中），待完整门禁与真实 PG/TLS 验收后翻转；见 doc/02 P04 章节与模块报告。 |
| `IdentityRotateCredentialRequest` | not_implemented | P04-A 应用/PG/RPC 已接线（实现中），待完整门禁与真实 PG/TLS 验收后翻转；见 doc/02 P04 章节与模块报告。 |
| `IdentityRotateCredentialResponse` | not_implemented | P04-A 应用/PG/RPC 已接线（实现中），待完整门禁与真实 PG/TLS 验收后翻转；见 doc/02 P04 章节与模块报告。 |
| `IdentityRevokeCredentialRequest` | not_implemented | P04-A 应用/PG/RPC 已接线（实现中），待完整门禁与真实 PG/TLS 验收后翻转；见 doc/02 P04 章节与模块报告。 |
| `IdentityRevokeCredentialResponse` | not_implemented | P04-A 应用/PG/RPC 已接线（实现中），待完整门禁与真实 PG/TLS 验收后翻转；见 doc/02 P04 章节与模块报告。 |
| `IdentityGetPrincipalStatusRequest` | not_implemented | P04-A 应用/PG/RPC 已接线（实现中），待完整门禁与真实 PG/TLS 验收后翻转；见 doc/02 P04 章节与模块报告。 |
| `IdentityGetPrincipalStatusResponse` | not_implemented | P04-A 应用/PG/RPC 已接线（实现中），待完整门禁与真实 PG/TLS 验收后翻转；见 doc/02 P04 章节与模块报告。 |
| `IdentityIssueDelegationRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityIssueDelegationResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityExchangeDelegationRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityExchangeDelegationResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityCheckAuthorizationRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityCheckAuthorizationResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListAssignmentsRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListAssignmentsResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentitySetAssignmentRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentitySetAssignmentResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeAssignmentRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeAssignmentResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityWatchInvalidationsRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityWatchInvalidationsResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetAuthorizationSnapshotRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetAuthorizationSnapshotResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetVerificationKeysRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetVerificationKeysResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelMetadata` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelConfiguration` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelCallRecord` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGetModelMetadataRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGetModelMetadataResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelListModelMetadataRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelListModelMetadataResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelUpdateModelRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelUpdateModelResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelRotateProviderSecretRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelRotateProviderSecretResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGatewayChatRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGatewayChatResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGetCallStatusRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGetCallStatusResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AccountRecord` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AdapterBinding` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InstallationRecord` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InstanceRecord` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `NamespaceScope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `StorageEntry` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `StorageCondition` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SharedMember` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionMetadata` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeadLetter` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TimerAcceptance` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionPolicy` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateAccountRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateAccountResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateAccountRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateAccountResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableAccountRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableAccountResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeBindAdapterRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeBindAdapterResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUnbindAdapterRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUnbindAdapterResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRegisterAdapterRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRegisterAdapterResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeHeartbeatRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeHeartbeatResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimePublishEventRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimePublishEventResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeValidateManifestRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeValidateManifestResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateInstallationRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateInstallationResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateInstanceRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateInstanceResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetDesiredStateRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetDesiredStateResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateConfigRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateConfigResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRegisterPluginRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRegisterPluginResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeHeartbeatPluginRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeHeartbeatPluginResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeClaimEventRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeClaimEventResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeliverEventRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeliverEventResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAckDeliveryRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAckDeliveryResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRenewDeliveryRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRenewDeliveryResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSubmitActionRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSubmitActionResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeQueryActionRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeQueryActionResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCancelActionRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCancelActionResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListActionMetadataRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListActionMetadataResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeInspectActionMetadataRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeInspectActionMetadataResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetStorageRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetStorageResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetStorageRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetStorageResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeleteStorageRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeleteStorageResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCompareAndSetStorageRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCompareAndSetStorageResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetSharedMembersRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetSharedMembersResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableSharedNamespaceRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableSharedNamespaceResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAcceptTimerTriggerRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAcceptTimerTriggerResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetResourceStateRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetResourceStateResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetOperationRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetOperationResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListDeadLettersRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListDeadLettersResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeReplayDeliveryRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeReplayDeliveryResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeBeginDeleteRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeBeginDeleteResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRestoreResourceRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRestoreResourceResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TaskRecord` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OccurrenceRecord` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCreateTaskRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCreateTaskResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCancelTaskRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCancelTaskResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetTaskRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetTaskResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerListTasksRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerListTasksResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetOccurrenceRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetOccurrenceResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetOperationRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetOperationResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostPackage.plugin_type_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostPackage.package_sha256` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostPackage.launch_profile` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostPackage.allowed_argument_names` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostRecord.host_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostRecord.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostRecord.packages` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostRecord.capabilities` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostRecord.observed_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ProcessSpec.instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ProcessSpec.installation_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ProcessSpec.execution_generation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ProcessSpec.desired_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ProcessSpec.desired_state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ProcessSpec.package_sha256` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ProcessSpec.launch_profile` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ProcessSpec.arguments` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ProcessSpec.environment_names` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionRecord.host_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionRecord.instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionRecord.execution_generation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionRecord.desired_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionRecord.outcome` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionRecord.process_marker` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionRecord.observed_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ExecutionRecord.reason` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostCommandReceipt.command_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostCommandReceipt.persisted` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostCommandReceipt.execution` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostCommandReceipt.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostRegisterHostRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostRegisterHostRequest.host` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostRegisterHostResponse.host` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostReportInventoryRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostReportInventoryRequest.host_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostReportInventoryRequest.packages` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostReportInventoryRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostReportInventoryResponse.host` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostApplyDesiredProcessRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostApplyDesiredProcessRequest.handoff` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostApplyDesiredProcessRequest.command_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostApplyDesiredProcessRequest.process` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostApplyDesiredProcessResponse.receipt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostStopExecutionRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostStopExecutionRequest.handoff` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostStopExecutionRequest.command_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostStopExecutionRequest.host_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostStopExecutionRequest.instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostStopExecutionRequest.execution_generation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostStopExecutionRequest.grace_ms` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostStopExecutionResponse.receipt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostDrainHostRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostDrainHostRequest.handoff` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostDrainHostRequest.command_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostDrainHostRequest.host_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostDrainHostRequest.grace_ms` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostDrainHostResponse.receipt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostGetExecutionRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostGetExecutionRequest.host_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostGetExecutionRequest.instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostGetExecutionRequest.execution_generation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostGetExecutionResponse.execution` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostWatchExecutionsRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostWatchExecutionsRequest.host_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostWatchExecutionsRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostWatchExecutionsRequest.cursor` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostWatchExecutionsResponse.execution` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostWatchExecutionsResponse.cursor` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostWatchExecutionsResponse.resync_required` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostToRuntime.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostToRuntime.registration` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostToRuntime.inventory` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostToRuntime.receipt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `HostToRuntime.observation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeToHost.apply` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeToHost.stop` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeToHost.drain` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeToHost.registered` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeToHost.failure` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PrincipalRecord.principal_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PrincipalRecord.binding` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PrincipalRecord.principal_kind` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PrincipalRecord.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PrincipalRecord.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PrincipalRecord.roles` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PrincipalRecord.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SessionRecord.session_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SessionRecord.principal_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SessionRecord.rotation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SessionRecord.expires_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SessionRecord.revoked` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TokenPair.session_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TokenPair.access_token` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TokenPair.refresh_token` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TokenPair.access_expires_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TokenPair.refresh_expires_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TokenPair.rotation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `CredentialDelivery.credential_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `CredentialDelivery.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `CredentialDelivery.new_secret` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `CredentialDelivery.delivery_receipt_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `CredentialDelivery.expires_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AssignmentRecord.assignment_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AssignmentRecord.principal_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AssignmentRecord.role` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AssignmentRecord.permissions` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AssignmentRecord.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AssignmentRecord.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DelegationProof.proof` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DelegationProof.audience` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DelegationProof.expires_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DelegationProof.business_hops` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AuthorizationDecision.allowed` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AuthorizationDecision.authorization_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AuthorizationDecision.checked_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AuthorizationDecision.denial` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InvalidationRecord.cursor` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InvalidationRecord.principal_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InvalidationRecord.authorization_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InvalidationRecord.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InvalidationRecord.resync_required` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AuthorizationSnapshot.cursor` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AuthorizationSnapshot.assignments` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AuthorizationSnapshot.authorization_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VerificationKey.key_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VerificationKey.ed25519_public_key` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VerificationKey.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `VerificationKey.valid_until` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityLoginRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityLoginRequest.username` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityLoginRequest.password` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityLoginResponse.tokens` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRefreshSessionRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRefreshSessionRequest.refresh_token` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRefreshSessionResponse.tokens` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityValidateSessionRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityValidateSessionRequest.access_token` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityValidateSessionResponse.principal` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityValidateSessionResponse.session` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetPrincipalRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetPrincipalRequest.access_token` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetPrincipalResponse.principal` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListSessionsRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListSessionsRequest.principal_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListSessionsRequest.page` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListSessionsResponse.sessions` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListSessionsResponse.page` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeSessionRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeSessionRequest.session_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeSessionResponse.session` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRegisterPrincipalRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRegisterPrincipalRequest.principal_kind` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRegisterPrincipalRequest.binding` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRegisterPrincipalRequest.allowed_services` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRegisterPrincipalResponse.principal` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRotateCredentialRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRotateCredentialRequest.credential_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRotateCredentialRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRotateCredentialResponse.delivery` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeCredentialRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeCredentialRequest.credential_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeCredentialRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeCredentialResponse.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetPrincipalStatusRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetPrincipalStatusRequest.principal_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetPrincipalStatusResponse.principal` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityIssueDelegationRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityIssueDelegationRequest.subject_proof` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityIssueDelegationRequest.audience` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityIssueDelegationRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityIssueDelegationRequest.permissions` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityIssueDelegationRequest.client_certificate_sha256` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityIssueDelegationResponse.delegation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityExchangeDelegationRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityExchangeDelegationRequest.parent_proof` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityExchangeDelegationRequest.audience` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityExchangeDelegationRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityExchangeDelegationRequest.permissions` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityExchangeDelegationRequest.client_certificate_sha256` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityExchangeDelegationResponse.delegation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityCheckAuthorizationRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityCheckAuthorizationRequest.subject_proof` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityCheckAuthorizationRequest.full_method` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityCheckAuthorizationRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityCheckAuthorizationRequest.known_authorization_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityCheckAuthorizationResponse.decision` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListAssignmentsRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListAssignmentsRequest.principal_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListAssignmentsRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListAssignmentsRequest.page` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListAssignmentsResponse.assignments` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityListAssignmentsResponse.page` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentitySetAssignmentRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentitySetAssignmentRequest.assignment` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentitySetAssignmentRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentitySetAssignmentResponse.assignment` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeAssignmentRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeAssignmentRequest.assignment_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeAssignmentRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityRevokeAssignmentResponse.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityWatchInvalidationsRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityWatchInvalidationsRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityWatchInvalidationsRequest.cursor` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityWatchInvalidationsResponse.invalidation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetAuthorizationSnapshotRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetAuthorizationSnapshotRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetAuthorizationSnapshotRequest.after` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetAuthorizationSnapshotResponse.snapshot` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetVerificationKeysRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetVerificationKeysRequest.known_key_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetVerificationKeysResponse.keys` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `IdentityGetVerificationKeysResponse.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelMetadata.model_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelMetadata.alias` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelMetadata.config_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelMetadata.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelMetadata.provider_kind` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelConfiguration.model_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelConfiguration.alias` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelConfiguration.provider_kind` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelConfiguration.endpoint` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelConfiguration.provider_model` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelConfiguration.timeout_ms` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelConfiguration.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelCallRecord.logical_call_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelCallRecord.owner_instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelCallRecord.model_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelCallRecord.config_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelCallRecord.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelCallRecord.execution_outcome` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelCallRecord.input_tokens` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelCallRecord.output_tokens` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelCallRecord.observed_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelCallRecord.reason` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGetModelMetadataRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGetModelMetadataRequest.model_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGetModelMetadataRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGetModelMetadataResponse.model` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelListModelMetadataRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelListModelMetadataRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelListModelMetadataRequest.page` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelListModelMetadataResponse.models` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelListModelMetadataResponse.page` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelUpdateModelRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelUpdateModelRequest.config` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelUpdateModelRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelUpdateModelResponse.model` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelUpdateModelResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelRotateProviderSecretRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelRotateProviderSecretRequest.model_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelRotateProviderSecretRequest.new_secret` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelRotateProviderSecretRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelRotateProviderSecretResponse.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelRotateProviderSecretResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGatewayChatRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGatewayChatRequest.logical_call_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGatewayChatRequest.model_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGatewayChatRequest.messages` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGatewayChatResponse.result` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGatewayChatResponse.call` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGetCallStatusRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGetCallStatusRequest.logical_call_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGetCallStatusRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ModelGetCallStatusResponse.call` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AccountRecord.account_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AccountRecord.platform` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AccountRecord.platform_account_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AccountRecord.display_name` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AccountRecord.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AccountRecord.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AdapterBinding.account_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AdapterBinding.adapter_instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AdapterBinding.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InstallationRecord.installation_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InstallationRecord.plugin_type_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InstallationRecord.manifest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InstallationRecord.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InstallationRecord.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InstanceRecord.instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InstanceRecord.installation_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InstanceRecord.host_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InstanceRecord.externally_managed` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `InstanceRecord.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `NamespaceScope.instance_private` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `NamespaceScope.installation_shared_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `StorageEntry.key` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `StorageEntry.value` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `StorageEntry.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `StorageEntry.expires_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `StorageCondition.absent` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `StorageCondition.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SharedMember.instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SharedMember.can_read` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SharedMember.can_write` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionMetadata.action_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionMetadata.owner_instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionMetadata.account_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionMetadata.status` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionMetadata.execution_outcome` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionMetadata.updated_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeadLetter.delivery_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeadLetter.last_attempt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `DeadLetter.reason` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TimerAcceptance.occurrence_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TimerAcceptance.event_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TimerAcceptance.duplicate` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TimerAcceptance.persisted_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionPolicy.account_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionPolicy.mute_enabled` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionPolicy.kick_enabled` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionPolicy.allowed_group_ids` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionPolicy.mute_daily_budget` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionPolicy.kick_daily_budget` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `ActionPolicy.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateAccountRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateAccountRequest.platform` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateAccountRequest.platform_account_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateAccountRequest.display_name` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateAccountResponse.account` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateAccountRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateAccountRequest.account_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateAccountRequest.display_name` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateAccountRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateAccountResponse.account` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableAccountRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableAccountRequest.account_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableAccountRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableAccountResponse.account` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableAccountResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeBindAdapterRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeBindAdapterRequest.binding` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeBindAdapterRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeBindAdapterResponse.binding` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeBindAdapterResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUnbindAdapterRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUnbindAdapterRequest.binding` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUnbindAdapterRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUnbindAdapterResponse.binding` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUnbindAdapterResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRegisterAdapterRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRegisterAdapterRequest.registration` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRegisterAdapterResponse.registration` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeHeartbeatRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeHeartbeatRequest.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeHeartbeatResponse.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeHeartbeatResponse.received_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimePublishEventRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimePublishEventRequest.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimePublishEventRequest.event` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimePublishEventResponse.receipt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeValidateManifestRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeValidateManifestRequest.manifest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeValidateManifestResponse.valid` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeValidateManifestResponse.violations` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateInstallationRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateInstallationRequest.plugin_type_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateInstallationRequest.manifest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateInstallationResponse.installation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateInstanceRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateInstanceRequest.installation_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateInstanceRequest.host_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateInstanceRequest.externally_managed` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCreateInstanceResponse.instance` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetDesiredStateRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetDesiredStateRequest.instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetDesiredStateRequest.desired_state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetDesiredStateRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetDesiredStateResponse.instance` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetDesiredStateResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateConfigRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateConfigRequest.resource` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateConfigRequest.config` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateConfigRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateConfigResponse.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateConfigResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRegisterPluginRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRegisterPluginRequest.registration` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRegisterPluginResponse.registration` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeHeartbeatPluginRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeHeartbeatPluginRequest.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeHeartbeatPluginResponse.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeHeartbeatPluginResponse.received_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeClaimEventRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeClaimEventRequest.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeClaimEventResponse.attempt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeliverEventRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeliverEventRequest.attempt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeliverEventResponse.attempt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeliverEventResponse.event` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAckDeliveryRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAckDeliveryRequest.attempt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAckDeliveryRequest.accepted` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAckDeliveryRequest.error` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAckDeliveryResponse.receipt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRenewDeliveryRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRenewDeliveryRequest.attempt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRenewDeliveryResponse.receipt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSubmitActionRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSubmitActionRequest.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSubmitActionRequest.action` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSubmitActionResponse.result` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSubmitActionResponse.execution_outcome` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeQueryActionRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeQueryActionRequest.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeQueryActionRequest.action_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeQueryActionRequest.idempotency_key` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeQueryActionResponse.result` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeQueryActionResponse.execution_outcome` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCancelActionRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCancelActionRequest.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCancelActionRequest.action_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCancelActionResponse.result` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCancelActionResponse.execution_outcome` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListActionMetadataRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListActionMetadataRequest.account_scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListActionMetadataRequest.page` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListActionMetadataResponse.actions` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListActionMetadataResponse.page` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeInspectActionMetadataRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeInspectActionMetadataRequest.account_scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeInspectActionMetadataRequest.action_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeInspectActionMetadataResponse.action` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetStorageRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetStorageRequest.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetStorageRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetStorageRequest.key` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetStorageResponse.entry` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetStorageRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetStorageRequest.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetStorageRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetStorageRequest.key` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetStorageRequest.value` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetStorageRequest.expires_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetStorageResponse.entry` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeleteStorageRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeleteStorageRequest.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeleteStorageRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeleteStorageRequest.key` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeleteStorageRequest.condition` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeleteStorageResponse.deleted` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDeleteStorageResponse.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCompareAndSetStorageRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCompareAndSetStorageRequest.fence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCompareAndSetStorageRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCompareAndSetStorageRequest.key` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCompareAndSetStorageRequest.value` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCompareAndSetStorageRequest.condition` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCompareAndSetStorageRequest.expires_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeCompareAndSetStorageResponse.entry` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetSharedMembersRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetSharedMembersRequest.namespace_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetSharedMembersRequest.installation_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetSharedMembersRequest.members` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetSharedMembersRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetSharedMembersResponse.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeSetSharedMembersResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableSharedNamespaceRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableSharedNamespaceRequest.namespace_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableSharedNamespaceRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableSharedNamespaceResponse.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeDisableSharedNamespaceResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAcceptTimerTriggerRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAcceptTimerTriggerRequest.handoff` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAcceptTimerTriggerRequest.occurrence_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAcceptTimerTriggerRequest.task_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAcceptTimerTriggerRequest.owner_instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAcceptTimerTriggerRequest.schedule_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAcceptTimerTriggerRequest.target` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAcceptTimerTriggerRequest.payload` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeAcceptTimerTriggerResponse.receipt` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetResourceStateRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetResourceStateRequest.resource` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetResourceStateResponse.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetOperationRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetOperationRequest.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetOperationResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListDeadLettersRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListDeadLettersRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListDeadLettersRequest.page` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListDeadLettersResponse.deliveries` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeListDeadLettersResponse.page` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeReplayDeliveryRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeReplayDeliveryRequest.delivery_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeReplayDeliveryRequest.target_instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeReplayDeliveryRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeReplayDeliveryResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeBeginDeleteRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeBeginDeleteRequest.resource` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeBeginDeleteRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeBeginDeleteResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRestoreResourceRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRestoreResourceRequest.resource` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRestoreResourceRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeRestoreResourceResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TaskRecord.task_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TaskRecord.owner_instance_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TaskRecord.installation_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TaskRecord.target` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TaskRecord.payload` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TaskRecord.run_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TaskRecord.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TaskRecord.version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `TaskRecord.occurrence_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OccurrenceRecord.occurrence_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OccurrenceRecord.task_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OccurrenceRecord.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OccurrenceRecord.handoff` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OccurrenceRecord.accepted_event_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `OccurrenceRecord.acknowledged_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCreateTaskRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCreateTaskRequest.task_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCreateTaskRequest.target` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCreateTaskRequest.payload` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCreateTaskRequest.run_at` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCreateTaskRequest.local_time` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCreateTaskResponse.task` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCancelTaskRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCancelTaskRequest.task_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCancelTaskRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerCancelTaskResponse.task` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetTaskRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetTaskRequest.task_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetTaskRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetTaskResponse.task` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerListTasksRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerListTasksRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerListTasksRequest.page` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerListTasksResponse.tasks` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerListTasksResponse.page` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetOccurrenceRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetOccurrenceRequest.occurrence_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetOccurrenceRequest.scope` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetOccurrenceResponse.occurrence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetOperationRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetOperationRequest.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `SchedulerGetOperationResponse.operation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_UNSPECIFIED` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_ACCOUNT_MANAGE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_ACTION_EXECUTE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_ACTION_INSPECT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_ADAPTER_CONNECT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_DELIVERY_MANAGE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_DELIVERY_PROCESS` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_HOST_EXECUTE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_HOST_READ` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_HOST_REGISTER` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_IDENTITY_ASSIGNMENT_MANAGE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_IDENTITY_ASSIGNMENT_READ` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_IDENTITY_AUTHORIZATION_CHECK` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_IDENTITY_DELEGATION_ISSUE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_IDENTITY_INVALIDATION_READ` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_IDENTITY_KEYS_READ` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_IDENTITY_PRINCIPAL_MANAGE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_IDENTITY_PRINCIPAL_READ` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_IDENTITY_SESSION_CREATE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_IDENTITY_SESSION_READ` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_IDENTITY_SESSION_REVOKE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_MODEL_CALL_INSPECT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_MODEL_CHAT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_MODEL_MANAGE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_MODEL_READ` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_NAMESPACE_MANAGE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_PLUGIN_CONNECT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_PLUGIN_MANAGE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_RESOURCE_MANAGE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_RESOURCE_READ` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_STORAGE_NAMESPACE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_TASK_READ` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_TASK_SCHEDULE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_TIMER_ACCEPT` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |

| `RuntimeGetActionPolicyRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetActionPolicyResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateActionPolicyRequest` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateActionPolicyResponse` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetActionPolicyRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetActionPolicyRequest.account_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeGetActionPolicyResponse.policy` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateActionPolicyRequest.context` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateActionPolicyRequest.policy` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateActionPolicyRequest.expected_version` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `RuntimeUpdateActionPolicyResponse.policy` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_ACCOUNT_POLICY_MANAGE` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Permission.PERMISSION_ACCOUNT_POLICY_READ` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.createAccount` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.updateAccount` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.disableAccount` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.bindAdapter` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.unbindAdapter` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.createInstallation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.createInstance` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.setInstanceDesiredState` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.updateInstanceConfig` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.getResourceState` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.beginDeleteResource` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.restoreResource` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.getOperation` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.listActionMetadata` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.getActionPolicy` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.updateActionPolicy` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.setSharedMembers` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.disableSharedNamespace` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.listDeadLetters` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.replayDelivery` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.getTask` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.cancelTask` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.getOccurrence` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.getModelMetadata` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.updateModel` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.rotateProviderSecret` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.listAssignments` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.setAssignment` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.revokeAssignment` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.registerPrincipal` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.getPrincipalStatus` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.rotateCredential` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `operations.revokeCredential` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `channels.operations` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `events.operation.updated` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `Health` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `PageInfo` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `PageRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ScheduleTask` | deprecated | 目标契约补全声明，尚无生产实现。 |
| `ShutdownNotice` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RequestContext` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ResourceRef` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ResourceScope` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `Fence` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `StreamCursor` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `OperationRef` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `FailureContext` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RpcFailure` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `OperationProgress` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `Operation` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `HandoffContext` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `DeliveryAttempt` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `DeliveryReceipt` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `VersionedResourceState` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `SnapshotPage` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ModelGetOperationRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ModelGetOperationResponse` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ModelGetModelConfigurationRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ModelGetModelConfigurationResponse` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ModelGetOperationRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ModelGetOperationRequest.operation` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ModelGetOperationResponse.operation` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ModelGetModelConfigurationRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ModelGetModelConfigurationRequest.model_id` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ModelGetModelConfigurationRequest.scope` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ModelGetModelConfigurationResponse.config` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ModelGetModelConfigurationResponse.secret_configured` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ModelGetModelConfigurationResponse.observed_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `operations.getModelConfiguration` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `SnapshotInfo` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRegistrationMetadata` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRecord` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `HostRegistrationRecord` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `InstanceConfiguration` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetAccountRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetAccountResponse` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAccountsRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAccountsResponse` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstallationRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstallationResponse` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstallationsRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstallationsResponse` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstanceRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstanceResponse` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstancesRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstancesResponse` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetAdapterRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetAdapterResponse` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAdaptersRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAdaptersResponse` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetHostRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetHostResponse` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListHostsRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListHostsResponse` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstanceConfigRequest` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstanceConfigResponse` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `SnapshotInfo.snapshot_id` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `SnapshotInfo.observed_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `SnapshotInfo.expires_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `SnapshotInfo.authorization_version` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `SnapshotInfo.owner_service` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `SnapshotInfo.page` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRegistrationMetadata.implementation` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRegistrationMetadata.implementation_version` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRegistrationMetadata.protocol_version` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRegistrationMetadata.declared_capabilities` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRegistrationMetadata.effective_capabilities` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRecord.resource` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRecord.version` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRecord.last_registration` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRecord.bindings` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRecord.online` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRecord.last_registered_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRecord.observed_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRecord.current_fence` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterRecord.lifecycle` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `HostRegistrationRecord.host` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `HostRegistrationRecord.lifecycle` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `HostRegistrationRecord.online` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `HostRegistrationRecord.last_heartbeat_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `HostRegistrationRecord.observed_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `InstanceConfiguration.instance` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `InstanceConfiguration.desired_version` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `InstanceConfiguration.values` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `InstanceConfiguration.applied_version` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `InstanceConfiguration.apply_state` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `InstanceConfiguration.delivery_id` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `InstanceConfiguration.fence` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `InstanceConfiguration.error` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `InstanceConfiguration.observed_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetAccountRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetAccountRequest.resource` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetAccountResponse.account` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetAccountResponse.observed_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAccountsRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAccountsRequest.catalog` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAccountsRequest.page` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAccountsResponse.accounts` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAccountsResponse.snapshot` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstallationRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstallationRequest.resource` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstallationResponse.installation` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstallationResponse.observed_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstallationsRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstallationsRequest.catalog` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstallationsRequest.page` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstallationsResponse.installations` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstallationsResponse.snapshot` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstanceRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstanceRequest.resource` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstanceResponse.instance` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstanceResponse.observed_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstancesRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstancesRequest.catalog` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstancesRequest.page` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstancesResponse.instances` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListInstancesResponse.snapshot` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetAdapterRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetAdapterRequest.resource` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetAdapterResponse.adapter` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetAdapterResponse.observed_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAdaptersRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAdaptersRequest.catalog` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAdaptersRequest.page` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAdaptersResponse.adapters` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListAdaptersResponse.snapshot` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetHostRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetHostRequest.resource` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetHostResponse.host` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetHostResponse.observed_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListHostsRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListHostsRequest.catalog` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListHostsRequest.page` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListHostsResponse.hosts` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeListHostsResponse.snapshot` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstanceConfigRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstanceConfigRequest.resource` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RuntimeGetInstanceConfigResponse.config` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ResourceKind.RESOURCE_CATALOG` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `Permission.PERMISSION_ACCOUNT_READ` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `Permission.PERMISSION_PLUGIN_READ` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `Permission.PERMISSION_ADAPTER_READ` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `Permission.PERMISSION_PLUGIN_CONFIG_READ` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConfigApplyState.CONFIG_APPLY_STATE_UNSPECIFIED` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConfigApplyState.CONFIG_APPLY_PENDING` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConfigApplyState.CONFIG_APPLY_APPLIED` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConfigApplyState.CONFIG_APPLY_REJECTED` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConfigApplyState.CONFIG_APPLY_UNAVAILABLE` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `operations.getAccount` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `operations.listAccounts` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `operations.getInstallation` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `operations.listInstallations` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `operations.getInstance` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `operations.listInstances` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `operations.getAdapterInstance` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `operations.listAdapterInstances` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `operations.getHost` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `operations.listHosts` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `operations.getInstanceConfig` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialStatus` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityGetCredentialStatusRequest` | not_implemented | P04-A 应用/PG/RPC 已接线（实现中），待完整门禁与真实 PG/TLS 验收后翻转；见 doc/02 P04 章节与模块报告。 |
| `IdentityGetCredentialStatusResponse` | not_implemented | P04-A 应用/PG/RPC 已接线（实现中），待完整门禁与真实 PG/TLS 验收后翻转；见 doc/02 P04 章节与模块报告。 |
| `PrincipalRecord.display_permissions` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `SessionRecord.created_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityLoginResponse.checked_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityRefreshSessionResponse.checked_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityValidateSessionResponse.assignments` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityValidateSessionResponse.authorization_version` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityValidateSessionResponse.checked_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityGetPrincipalResponse.assignments` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityGetPrincipalResponse.authorization_version` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityGetPrincipalResponse.checked_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityListSessionsResponse.checked_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityRevokeSessionResponse.checked_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityRegisterPrincipalResponse.initial_credential` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialStatus.credential_id` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialStatus.principal_id` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialStatus.binding` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialStatus.version` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialStatus.state` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialStatus.created_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialStatus.expires_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialStatus.checked_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityGetCredentialStatusRequest.context` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityGetCredentialStatusRequest.credential_id` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentityGetCredentialStatusResponse.credential` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialState.CREDENTIAL_STATE_UNSPECIFIED` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialState.CREDENTIAL_PENDING` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialState.CREDENTIAL_ACTIVE` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialState.CREDENTIAL_REVOKED` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CredentialState.CREDENTIAL_EXPIRED` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `Permission.PERMISSION_MESSAGE_READ` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `operations.getCredentialStatus` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AssignmentSpec` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AssignmentSpec.principal_id` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AssignmentSpec.role` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AssignmentSpec.permissions` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AssignmentSpec.scope` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `IdentitySetAssignmentRequest.assignment_id` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorization` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RefreshConnectionAuthorization` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorizationResult` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RegisterAdapterResponse.authorization` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterToCore.connection_proof` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `AdapterToCore.refresh_authorization` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CoreToAdapter.authorization_result` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorization.fence` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorization.proof` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorization.issued_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorization.expires_at` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorization.refresh_credential` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorization.refresh_generation` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorization.principal_id` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorization.credential_version` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorization.authorization_version` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RefreshConnectionAuthorization.request_id` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RefreshConnectionAuthorization.fence` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RefreshConnectionAuthorization.proof` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RefreshConnectionAuthorization.refresh_credential` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RefreshConnectionAuthorization.refresh_generation` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorizationResult.request_id` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorizationResult.authorization` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ConnectionAuthorizationResult.error` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `RegisterPluginResponse.authorization` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `PluginToCore.connection_proof` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `PluginToCore.refresh_authorization` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `CoreToPlugin.authorization_result` | not_implemented | 目标契约补全声明，尚无生产实现。 |
| `ActionEnvelope.dispatch_attempt_id`、`ActionEnvelope.dispatch_sequence`、`ActionResult.execution_outcome`、`ActionResult.dispatch_attempt_id`、`AdapterToCore.action_evidence`、`CoreToAdapter.action_receipt`、`ActionExecutionEvidence.action_id`、`ActionExecutionEvidence.dispatch_attempt_id`、`ActionExecutionEvidence.execution_outcome`、`ActionExecutionEvidence.observed_at`、`ActionExecutionEvidence.last_error`、`ActionExecutionEvidence.platform_reference`、`ActionReceipt.action_id`、`ActionReceipt.dispatch_attempt_id`、`ActionReceipt.disposition`、`ActionReceipt.error`、`ActionReceipt.result` | not_implemented | P07 目标 Action 桥流扩展：Runtime 下发前填 `dispatch_attempt_id/dispatch_sequence`；适配器用 `action_result` 上报终态、用 `action_evidence` 上报未执行/未知；Runtime 用 `action_receipt` 确认并让适配器标记本地 journal 已上报。`ActionResult.execution_outcome` 区分平台执行是否已知；不新增 ErrorCode。 |
| `ActionMetadata.kind`、`ActionMetadata.target_account_id`、`ActionMetadata.target_conversation_id`、`ActionMetadata.created_at`、`ActionMetadata.deadline`、`ActionMetadata.retain_until`、`ActionMetadata.idempotency_key`、`ActionMetadata.platform_message_id`、`ActionMetadata.dispatch_attempt_id`、`ActionMetadata.error` | not_implemented | P07 目标 Action 元数据观察字段（不含消息正文/平台秘密）；只读投影，P07 实现后由 Runtime 生产。 |
| `RuntimeListActionMetadataRequest.status`、`RuntimeListActionMetadataRequest.kind` | not_implemented | P07 目标可选过滤；缺省不按状态/类型过滤。 |
| `RuntimeQueryActionResultRequest.context`、`RuntimeQueryActionResultRequest.account_scope`、`RuntimeQueryActionResultRequest.action_id`、`RuntimeQueryActionResultRequest.idempotency_key`、`RuntimeQueryActionResultRequest.owner_instance_id`、`RuntimeQueryActionResultResponse.result`、`RuntimeQueryActionResultResponse.execution_outcome`、`RuntimeQueryActionResultResponse.metadata` | not_implemented | P07 目标 Control 查询：caller=scoped Control、permission=`action.inspect`、`account_scope` 恰含一个 `RESOURCE_ACCOUNT`；按幂等键查询必须同时给 `owner_instance_id`（业务幂等按插件实例隔离）。插件查询仍走 `PluginBridge`。 |
| `ActionKind.ACTION_KIND_UNSPECIFIED`、`ActionKind.ACTION_KIND_SEND_MESSAGE`、`ActionKind.ACTION_KIND_REACT_MESSAGE`、`ActionKind.ACTION_KIND_DELETE_MESSAGE`、`ActionKind.ACTION_KIND_MUTE_MEMBER`、`ActionKind.ACTION_KIND_KICK_MEMBER`、`ActionKind.ACTION_KIND_APPROVE_REQUEST` | not_implemented | P07 目标 Action 类型投影词表（wire 判别仍为 `ActionEnvelope.payload` oneof；`schedule_task` 不产生类型值）。 |
| `ActionReceiptDisposition.ACTION_RECEIPT_DISPOSITION_UNSPECIFIED`、`ActionReceiptDisposition.ACTION_RECEIPT_RECORDED`、`ActionReceiptDisposition.ACTION_RECEIPT_DUPLICATE`、`ActionReceiptDisposition.ACTION_RECEIPT_EVIDENCE_ONLY`、`ActionReceiptDisposition.ACTION_RECEIPT_CONFLICT` | not_implemented | P07 目标结果/证据回执处置：迟到结果在 Action 已 EXPIRED 时只记证据（EVIDENCE_ONLY），不改变终态。 |
| `Permission.PERMISSION_MESSAGE_SEND`、`Permission.PERMISSION_MESSAGE_REACT`、`Permission.PERMISSION_MESSAGE_DELETE`、`Permission.PERMISSION_MEMBER_MUTE`、`Permission.PERMISSION_MEMBER_KICK`、`Permission.PERMISSION_REQUEST_APPROVE` | not_implemented | P07 目标每-kind Action 权限：把已冻结的 `message.send/react/delete`、`member.mute/kick`、`request.approve` 字符串词表映射为封闭 `Permission` 枚举值（41–46），供 Identity 显式资源授予与 Runtime 校验；不改变现有 manifest 字符串词表，P07 实现前不授予/不校验。 |
| `semantic.action.result_execution_outcome`、`semantic.action.dispatch_attempt_correlation`、`semantic.action.evidence_late_result`、`semantic.action.control_query_scope`、`semantic.action.permission_vocabulary`、`semantic.action.quota_persistence`、`semantic.action.policy_gate` | not_implemented | P07 目标 Action 冻结语义（见下节）：状态×执行已知性矩阵、dispatch attempt 关联、迟到结果只记证据、Control 显式账号 scope、每-kind 权限词表、滑动窗口持久化、Mute/Kick fail-closed 策略；实现前 reference 行为仍是唯一生产语义。 |
| `HostToRuntime.fence`、`HostToRuntime.heartbeat`、`RuntimeToHost.fence`、`RuntimeToHost.heartbeat_ack`、`HostRegisterHostResponse.fence`、`ExecutionRecord.command_id`、`HostHeartbeat.fence`、`HostHeartbeat.sent_at`、`HostHeartbeat.cursor`、`HostHeartbeatAck.fence`、`HostHeartbeatAck.received_at`、`HostHeartbeatAck.heartbeat_interval_seconds`、`HostHeartbeatAck.resync_required` | not_implemented | P08 目标 HostBridge 帧：内部 mTLS 上每条注册后帧携带当前 Fence；注册响应分配 connection_generation/runtime_epoch；Host 每 5s 发 HostHeartbeat（cursor=已上报观察游标），Runtime 回 HostHeartbeatAck（15s 无心跳判疑似离线，resync_required 沿用 P06 语义）；ExecutionRecord.command_id 关联触发命令，空串表示非命令触发的自发变化，不伪造 ID。不使用 bootstrap/ConnectionAuthorization/connection_proof。 |
| `HostBridge.Connect` | not_implemented | P08 目标：HostBridge 只注册在 Runtime 内部 mTLS `grpc_listen`，Host 用部署签发的 mTLS 客户端证书认证；证书指纹映射到部署预注册 host_id（不新增 bootstrap/proof/refresh，也不新增权限/服务）。P08b-2b 已实现同步 bidi 传输服务端、registration/heartbeat/fence/allowlist 与旧 generation 拒绝（in-process conformance 通过）；P08b-3 已加 Runtime host_commands outbox/host_observations inbox、P05 同 UoW 命令入队、受管 bootstrap 生成（仅存 digest），但 HostBridge 实际下发、receipt/observation 持久化与真实 Host/PG e2e 仍未完成；reference runtime 未服务 HostBridge，故状态保持 not_implemented。 |
| `semantic.host.bridge_mtls_identity`、`semantic.host.connection_fence`、`semantic.host.heartbeat_offline`、`semantic.host.observation_identity`、`semantic.host.execution_recovery`、`semantic.host.config_apply_evidence` | not_implemented | P08 目标冻结语义：HostBridge 证书→预注册 host_id 身份与方向校验；注册代次/fence 单活动连接与旧代次拒绝；5s 心跳/15s 疑似离线（online=false 但 process_outcome 只转 UNKNOWN，不写 FAILED/已停止、不 bump version）；观察身份 `(host_id, connection_generation, cursor.sequence)`+command_id 与 source=Host inbox 去重；命令 journal 先持久后 spawn、marker 校验、同代次未知不重复 spawn、崩溃恢复重建 receipt；仅当前 fence+当前 config_version 的 ACK 推进持久 applied_version，delivery_id 易失、重连后 Unavailable。实现前 reference 行为仍是唯一生产语义。 |
| `Permission.PERMISSION_NAMESPACE_READ` | not_implemented | P09 目标共享 namespace 管理读权限（枚举值 47，承接 P07 的 41–46）：只用于 `RuntimeService.GetSharedNamespace`（scoped Control，带 exact installation scope），必须显式资源授予，admin/operator/viewer 均不默认持有；P07 的 41–46 与既有 `NAMESPACE_MANAGE=27` 不变。 |
| `NamespaceRecord.namespace_id`、`NamespaceRecord.installation_id`、`NamespaceRecord.version`、`NamespaceRecord.disabled`、`NamespaceRecord.created_at`、`NamespaceRecord.updated_at`、`StorageQuota.namespace_keys_used`、`StorageQuota.namespace_keys_limit`、`StorageQuota.namespace_bytes_used`、`StorageQuota.namespace_bytes_limit`、`StorageQuota.installation_keys_used`、`StorageQuota.installation_keys_limit`、`StorageQuota.installation_bytes_used`、`StorageQuota.installation_bytes_limit`、`RuntimeCreateSharedNamespaceRequest.context`、`RuntimeCreateSharedNamespaceRequest.installation_id`、`RuntimeCreateSharedNamespaceRequest.expected_version`、`RuntimeCreateSharedNamespaceResponse.namespace`、`RuntimeGetSharedNamespaceRequest.context`、`RuntimeGetSharedNamespaceRequest.namespace_id`、`RuntimeGetSharedNamespaceResponse.namespace`、`RuntimeGetSharedNamespaceResponse.members`、`RuntimeGetSharedNamespaceResponse.quota`、`NamespaceScope.installation_shared` | not_implemented | P09 目标共享 namespace 管理与读取：`NamespaceScope.installation_shared` 由服务端按已认证实例解析其安装的共享 namespace（插件不自报 owner，未创建返回 NOT_FOUND）；`CreateSharedNamespace`（namespace.manage、幂等、每安装最多一个共享 namespace，已存在 CONFLICT 后用 Get 读）与 `GetSharedNamespace`（namespace.read、exact installation scope、返回版本/禁用/成员/配额用量、不回键值正文）是既有 Runtime owner 方法族内新增，不新增服务/错误码。 |
| `StorageGetRequest.scope`、`StorageGetResponse.version`、`StorageGetResponse.expires_at`、`StorageSetRequest.scope`、`StorageSetRequest.condition`、`StorageSetResponse.version`、`StorageSetResponse.expires_at`、`StorageDeleteRequest.scope`、`StorageDeleteRequest.condition`、`StorageDeleteResponse.deleted`、`StorageDeleteResponse.version`、`StorageCompareAndSetRequest.key`、`StorageCompareAndSetRequest.scope`、`StorageCompareAndSetRequest.value`、`StorageCompareAndSetRequest.condition`、`StorageCompareAndSetRequest.expires_at`、`StorageCompareAndSetResponse.entry`、`HostRequest.storage_compare_and_set`、`HostResponse.storage_compare_and_set` | not_implemented | P09 目标插件 storage 帧扩展（PluginBridge `HostRequest.storage_*`；CAS 为 host_request/host_response oneof 第 17 项）：私有 scope 缺省=instance_private；共享由服务端解析；私有 Set 必须缺 condition，共享 Set/Delete/CAS 必须带 condition（absent=create、version=update/delete）；TTL 为绝对 UTC 且未来 ≤30 天；响应回 version/expires_at；配额/错误映射见存储契约（不新增 ErrorCode）。4 个 unary storage 方法为 internal_dispatcher，不对外注册。 |
| `TaskRecord.created_at`、`TaskRecord.updated_at`、`TaskRecord.cancel_requested`、`TaskRecord.reason`、`OccurrenceRecord.attempts`、`OccurrenceRecord.last_error`、`OccurrenceRecord.created_at`、`OccurrenceRecord.updated_at`、`SchedulerListTasksRequest.state`、`TaskResponse.version`、`TaskResponse.occurrence_id`、`TimerTriggered.occurrence_id` | not_implemented | P10 目标任务契约补全：TaskRecord 增加创建/更新时间、取消请求标记与 FAILED 稳定原因；OccurrenceRecord 增加交接尝试次数（1 起算）、稳定错误与时间；ListTasks 支持可选状态过滤；插件 TaskResponse 回 version/occurrence_id；TimerTriggered 增加 occurrence_id（Scheduler 生成并持久，`event_id := occurrence_id`，Runtime 以 inbox(source=scheduler, message=occurrence_id) 去重）。本地时间/任务状态类型从 plugins.proto 迁到 common.proto（FQN 不变），scheduler.proto 不再 import plugins.proto；不新增 RPC/枚举/错误码。 |
| `operations.createSharedNamespace`、`operations.getSharedNamespace` | not_implemented | P09 目标 REST 管理面：`POST/GET /installations/{installationId}/shared-namespace` 分别映射 `RuntimeService.CreateSharedNamespace`（`namespace.manage`、幂等、每安装一个共享 namespace，已存在 CONFLICT 后用 GET 读）与 `GetSharedNamespace`（`namespace.read`、exact installation scope，返回版本/禁用/成员/配额用量、不回键值正文）；目标行为与权限冻结，Control 接线留 P12，当前返回 not_implemented。 |
| `semantic.storage.scope_resolution`、`semantic.storage.cas_condition`、`semantic.storage.quota_persistent`、`semantic.storage.receipt_capacity`、`semantic.namespace.shared_membership`、`semantic.scheduler.owner_derivation`、`semantic.scheduler.cancel_window`、`semantic.scheduler.handoff_expiry`、`semantic.scheduler.timer_occurrence_identity` | not_implemented | P09/P10 目标冻结语义（见下节）：storage scope 由已认证实例解析、共享写必须 condition、配额/收据容量持久且保守拒绝、共享成员双门禁；任务 owner 由委托派生、取消窗口、交接 7 天/100 attempts 到期后人工恢复、TimerTriggered occurrence_id 与 event_id 约定。实现前 reference 行为仍是唯一生产语义。 |
| `ModelConfiguration.state`、`ModelConfiguration.max_concurrency`、`ModelCallRecord.secret_version`、`ModelCallRecord.usage_known`、`ModelCallRecord.may_have_billed`、`ModelCallRecord.provider_request_id`、`ModelCallRecord.latency_ms`、`ModelCallRecord.disposition`、`ModelGatewayChatResponse.disposition`、`ModelGetModelConfigurationResponse.secret_version`、`ModelChatRequest.logical_call_id` | not_implemented | P11 目标 Model 调用记录/配置投影：`secret_version` 固定首次事务快照供审计/在途核验，轮换只影响后续；`usage_known=false` 时 token 不是 Provider 权威用量；`may_have_billed` 为保守计费事实（UNKNOWN/超时/5xx 恢复 true，明确 4xx/429 false）；`provider_request_id` 只保留 ≤128 可见 ASCII 安全 id、绝不含秘密；`latency_ms` UNKNOWN 时为 0；`disposition` 仅在 Resolve 后出现；`ModelChatDisposition` 区分 FRESH/回放在途/回放终态（回放不再调用 Provider、不新增计费）；插件 `ModelChatRequest.logical_call_id` 为稳定 UUIDv4 调用身份（≠ request_id/idempotency_key）；`state`/`max_concurrency` 为只读配置投影与 1..64 并发上限。 |
| `ModelDisableModelRequest.context`、`ModelDisableModelRequest.model_id`、`ModelDisableModelRequest.expected_version`、`ModelDisableModelResponse.model`、`ModelEnableModelRequest.context`、`ModelEnableModelRequest.model_id`、`ModelEnableModelRequest.expected_version`、`ModelEnableModelResponse.model`、`ModelResolveModelCallRequest.context`、`ModelResolveModelCallRequest.logical_call_id`、`ModelResolveModelCallRequest.expected_version`、`ModelResolveModelCallRequest.disposition`、`ModelResolveModelCallResponse.call` | not_implemented | P11 目标管理 RPC：`DisableModel`/`EnableModel`（scoped Control、`model.manage`、幂等、expected_version CAS；删除归 P13）与 `ResolveModelCall`（scoped Control、`model.manage`、`expected_version` 必须匹配未决 UNKNOWN 当前版本、disposition 只接受 1–4、幂等键必填；处置后 call 变 FAILED 终态且不再触发 Provider，重复处置/非 UNKNOWN 为 CONFLICT）。 |
| `ModelChatDisposition.MODEL_CHAT_DISPOSITION_UNSPECIFIED`、`ModelChatDisposition.FRESH_RESULT`、`ModelChatDisposition.REPLAY_IN_PROGRESS`、`ModelChatDisposition.REPLAY_TERMINAL`、`ModelCallDisposition.MODEL_CALL_DISPOSITION_UNSPECIFIED`、`ModelCallDisposition.NOT_EXECUTED`、`ModelCallDisposition.EXECUTED_NOT_BILLED`、`ModelCallDisposition.EXECUTED_BILLED`、`ModelCallDisposition.ABANDONED` | not_implemented | P11 目标调用形状/UNKNOWN 处置词表：Chat 回放三态（FRESH 带 result；REPLAY_IN_PROGRESS/REPLAY_TERMINAL 只带 call 元数据、无正文、不重调 Provider）；UNKNOWN 处置四态（NOT_EXECUTED→may_have_billed=false、EXECUTED_NOT_BILLED→false、EXECUTED_BILLED/ABANDONED→true）；未处置保持 UNSPECIFIED。 |
| `semantic.model.two_phase_unknown`、`semantic.model.duplicate_call_replay`、`semantic.model.request_hash`、`semantic.model.secret_rotation_pinning`、`semantic.model.provider_error_mapping`、`semantic.model.usage_known_billing`、`semantic.model.instance_delegation` | not_implemented | P11 目标 Model 冻结语义（见下节）：两次事务（先意图→Provider→已知/UNKNOWN），Provider 已调用后除明确 4xx/429 外不得记 KNOWN_FAILED/未计费；同 subject+logical_call_id 不重复调用/计费，回放三态不重调 Provider；request_hash 服务端计算（同键异内容 CONFLICT）；在途固定 config/secret 版本、轮换只影响后续；Provider 错误映射继承既有 ErrorCode+acceptance/recovery/retry_after_ms；usage_known/may_have_billed 保守计费事实；Model 只接受 Runtime 实例委托并复核当前权威，owner=委托 subject。实现前 reference 行为仍是唯一生产语义。 |
| `Permission.PERMISSION_LOG_READ`、`Permission.PERMISSION_AUDIT_READ` | not_implemented | P12 target 权限声明：日志/审计敏感读权限，需显式授予；固定角色/创建策略不派生，尚无生产者或授权面接线（REST/WS 契约已登记，实际查询仍走 v1 reference 或显式不可用）。 |
| `IdentityPrincipalSummary.principal_id`、`IdentityPrincipalSummary.binding`、`IdentityPrincipalSummary.principal_kind`、`IdentityPrincipalSummary.version`、`IdentityPrincipalSummary.state`、`IdentityPrincipalSummary.roles`、`IdentityListPrincipalsRequest.context`、`IdentityListPrincipalsRequest.page`、`IdentityListPrincipalsRequest.state_filter`、`IdentityListPrincipalsResponse.principals`、`IdentityListPrincipalsResponse.page`、`IdentityListPrincipalsResponse.checked_at`、`operations.listPrincipals` | implemented | P12l：Identity `ListPrincipals` 只读人类主体目录（Control platform peer + identity.principal.read、当前获授资源范围过滤、stable principal-id keyset 分页、checked_at、roles 投影、排除服务/Host/插件身份）；真实 PG16.4 + 真 mTLS RPC fixture 与 Control `/api/v1/principals` 转发已接。OpenAPI x-target-contract-status marker 按 target_api 认证投影规则保持 not_implemented；REST x-contract-status 已翻 implemented。 |
| `Permission.PERMISSION_HEALTH_READ` | not_implemented | P12 target authorized aggregate `/api/v1/health`; anonymous liveness/readiness probes do not use it. |
| `operations.setInstancePluginState` | not_implemented | Q6 实例级插件启停：显式 `PUT /installations/{installationId}/instances/{instanceId}/plugin-state`，不得复用旧 `/plugins/{id}/enable`；202 是受理，期望/Host 观察/插件 ACK 三类事实分离。 |
| `operations.getInstanceConfiguration` | not_implemented | Q6 实例级配置读取：显式 installation/instance 路径，返回 Runtime 期望/Host 观察/插件 ACK 分离事实，密钥不返回。 |
| `operations.updateInstanceConfiguration` | not_implemented | Q6 实例级配置更新：显式 installation/instance 路径 + 幂等键/expected_version；202 受理不表示 Host/插件已应用。 |
| `operations.disableModel`、`operations.enableModel`、`operations.getModelCallStatus`、`operations.resolveModelCall` | not_implemented | P11d Control REST 契约：Model 生命周期/调用状态/UNKNOWN 处置转发端点，经单模型 scope 与精确权限；target 仍按 not_implemented 登记，Control 实现见 P11d 记录。 |
<!-- contract-status:end -->

`/ws/messages` 自 2026-10-31（W5）起为 `implemented`：只有被私有控制配置显式授予 `message.read` 的用户可订阅，事件 `message.updated` 仅含消息元数据（`message_id`、`conversation_id`、`direction`；v1 只产生 `received`），不推送正文、没有 REST 消息投影、也不构成审计；任何默认角色（含 admin）都不持有 `message.read`。授权模型见 `doc/08-operations.md`。

`ActionEnvelope.react_message` 自 2026-10-31（W6）起为 `implemented`：只接受群/私聊消息、平台中立 reaction token（`like`/`laugh`/`surprise`/`sad`/`angry`），需要插件权限 `message.react`，且适配器必须协商能力 `action.react_message.v1`；语义、错误码与“成功可无平台消息 ID”见 `doc/01-runtime-semantics.md`。

`ActionEnvelope.delete_message` 自 2026-10-31（W6c）起为 `implemented`：只允许撤回**本机器人账号自己发送过**的消息，需要插件权限 `message.delete` 与适配器能力 `action.delete_message.v1`；适配器维护已发送消息索引，未索引的平台消息 ID 返回 `PERMISSION_DENIED`，语义见 `doc/01-runtime-semantics.md`。

`ActionEnvelope.mute_member` 与 `ActionEnvelope.kick_member` 自 2026-10-31（W6e）起为 `implemented`：仅群聊，需要插件权限 `member.mute`/`member.kick` 与适配器能力 `action.mute_member.v1`/`action.kick_member.v1`；危险动作默认要求 `KIARABOT_ACTION_ALLOWED_CONVERSATIONS` 白名单与每日预算（`KIARABOT_ACTION_DAILY_LIMITS`），语义见 `doc/01-runtime-semantics.md` 与 `doc/08-operations.md`。

`ActionEnvelope.approve_request` 自 2026-10-31（W6f）起为 `implemented`：v1 只审批好友请求，需要插件权限 `request.approve` 与适配器能力 `action.approve_request.v1`；`request_id` 必须回填对应 `FriendRequest.request_id`，`reason` 只作为审计元数据，未知/过期 token 由适配器返回 `NOT_FOUND`。事件前置与语义见 `doc/01-runtime-semantics.md`。

## 目录

```text
proto/kiarabot/v1/
  common.proto
  events.proto
  actions.proto
  adapters.proto
  plugins.proto
  services.proto

api/
  openapi.yaml
  asyncapi.yaml
```

## 事件协议

事件类型只由 `EventEnvelope.payload` 的 `oneof` 判定，不再维护单独的 type 字符串。Action 同理；消费者使用 oneof discriminator 选择处理器，无法匹配 payload 时拒绝消息并返回 `INVALID_ARGUMENT`。REST 错误类型以 `api/openapi.yaml` 的 Error schema 为准，实时频道以 `api/asyncapi.yaml` 为准。

所有事件都使用统一外层：

```text
EventEnvelope
  event_id          全局唯一
  schema_version    协议版本
  occurred_at       发生时间
  trace_id          链路追踪 ID
  source            适配器和账号
  subject           用户和会话
  sequence          会话内单调递增序号（uint64，0 表示未提供）
  delivery_mode     投递语义（EventDeliveryMode，当前定义 AT_LEAST_ONCE）
  payload           事件内容（oneof discriminator）
```

`sequence` 的排序域由 Core 从 envelope 派生，不是独立字段：`(source.adapter_id, source.account_id, subject.conversation_id, 消息载荷 ConversationRef.type)`。群号与私聊号相同的两个会话因此各自独立；没有会话载荷的事件（如 `AdapterStatusChanged`）使用 `TYPE_UNSPECIFIED`，不与消息会话共享排序域。`TimerTriggered` 由 Core 生成且不占用会话排序域。客户端、插件和适配器都不得只凭 `conversation_id` 推断排序域。

第一批事件：

```text
MessageReceived
MessageSent
GroupMemberChanged
FriendRequest
TimerTriggered
AdapterStatusChanged
PluginStatusChanged
```

上表是事件词表，不是实现承诺：v1 端到端闭环 `MessageReceived`、`TimerTriggered`、`FriendRequest` 与 `AdapterStatusChanged`；其余三种见“契约实现状态”中的 `not_implemented` 行。客户端/插件不得订阅尚未实现的事件并假定能收到数据；Core 保留订阅映射只为后续接线。

`AdapterStatusChanged` 自 2026-11-02 起由 NoneBot 适配器产生：适配器在 Core 连接、平台连接/断开和平台动作失败的状态迁移时通过本机 outbox 上报 `adapter_id`、`state` 与 `reason`，状态不变不重复上报；Core 在 `core/src/application/adapter_ingress.cpp` 校验来源 `adapter_id` 与 `HealthState` 范围后持久化，插件须订阅能力 `event.adapter.status.changed` 且获授 `adapter.read`。`AdapterStatusChanged` 没有会话载荷，排序域使用 `TYPE_UNSPECIFIED`（见上文）。

`FriendRequest` 自 2026-10-31（W6f）起由 NoneBot 适配器产生：`user_id`、`comment`、`request_id`（适配器作用域的安全 token，不是平台 `flag`）；插件必须订阅能力 `event.friend.request` 且获授 `request.read`。审批使用 ApproveRequest Action 并回填同一 `request_id`；适配器在本机解析原始 `flag` 后调用 OneBot `set_friend_add_request`。v1 只映射好友请求，群邀请/加群请求没有契约事件，仍不支持。

消息内容必须使用平台无关的消息段：

```text
text
image
audio
video
file
mention
reply
location
unknown
```

其中 `audio`、`video`、`file`、`location` 在 v1 由适配器降级为 `unknown`（见状态表），不得假定适配器会透传。

平台不支持的类型由适配器报告能力错误，不能在 Core 中写平台判断。

## Action 协议

T04a 定义可选功能 `action.result.v1`：RegisterPluginRequest.supported_features（字段 3）申请，RegisterPluginResponse.enabled_features（字段 5）返回实际支持的交集。缺省为空，旧客户端仍只使用事件能力；新客户端连接旧 Core 时不得发送 Action 请求，应本地返回 CAPABILITY_NOT_SUPPORTED。Core 不得声明尚未接线的功能，未协商的 Action 帧明确拒绝。未知功能名不启用。

PluginToCore.action 保留字段 3，PluginActionRequest.action 保留字段 1，新增 request_id=2；新增 action_query=6、action_cancel=7；CoreToPlugin.action_response=7。响应带同一 request_id，outcome 恰好一个 result 或 error。提交/查询/取消均为一问一答；不承诺异步终态推送，客户端通过查询获得终态。请求 ID 在当前连接的在途操作中唯一，长度 1–128；trace_id 同样非空且最多 128。ID/幂等键沿用 ASCII 字母数字、点、下划线、连字符约束。

提交必须有 action_id、idempotency_key、trace_id、deadline 和已知 payload；身份来自认证插件连接，requested_by 为空时由 Core 填入，非空且不匹配则 PERMISSION_DENIED。幂等索引为调用方身份+Action 幂等键，与连接实例和 request_id 无关；相同键/相同规范化内容返回原 action_id 和状态，不同内容返回 CONFLICT。查询 PluginActionQuery 的 selector 必须恰好为 action_id 或 idempotency_key；查询和取消均要求未来 UTC deadline。不可见/不存在/保留期后记录统一 NOT_FOUND。取消仅对未派发、未到期 ACCEPTED 生效；已派发且结果未知返回 CONFLICT，已有终态返回原结果。

拒绝、查询超时、存储故障使用 response.error：INVALID_ARGUMENT、PERMISSION_DENIED、CAPABILITY_NOT_SUPPORTED、CONFLICT、NOT_FOUND、RATE_LIMITED、TIMEOUT、UPSTREAM_UNAVAILABLE、INTERNAL。它们不自动生成 Action FAILED。ActionResult 沿用已有状态；ACCEPTED 不带 completed_at/error，终态带 completed_at，FAILED 必须带稳定 Error；EXPIRED 使用 TIMEOUT，CANCELLED 不要求 error。断线、缺失响应、未知枚举或 NOT_FOUND 均不得解释为平台执行失败；重新协商后按同一调用方及幂等键查询。协议已定义不代表服务已实现，T04b 负责实际接线。

插件和 Core 不直接调用平台 API，而是产生 Action：

```text
SendMessage
DeleteMessage
ReactMessage
MuteMember
KickMember
ApproveRequest
ScheduleTask
```

Action 词表同样大于 v1 实现子集：`SendMessage` 的群/私聊纯文本、`ReactMessage` 的群/私聊表情回应、`DeleteMessage` 的 bot 自己消息撤回、`MuteMember`/`KickMember` 的群管理动作与 `ApproveRequest` 的好友请求审批可用；`ScheduleTask` 已 `deprecated`（旧调度契约，改用 `host.tasks.v1`），与 `reply`、`reply_to_message_id` 和非 text/image/at/reply 消息段一样被显式拒绝（`CAPABILITY_NOT_SUPPORTED`），见“契约实现状态”。`ActionEnvelope.deadline` 契约上限为 300 秒（未来 UTC），但 v1 只校验正时间戳、不强制该上限，超长 deadline 的 Action 不会提前过期（`semantic.action.deadline_max_300s` = not_implemented）。

REST Action 查询与 Bridge 查询共享同一份 Action 记录与语义：终态结果从最后一次状态更新起保留 604800 秒（7 天，不得少于 86400 秒）；`ACCEPTED` 且 `deadline <= now` 的记录在读取时迁移为 `EXPIRED`，其 `error.code` 为 `TIMEOUT`；缺失、已过期（保留期外）或不可见统一返回 `NOT_FOUND`；查询超时不改变 Action 状态。v1 的 REST 查询路径尚未接线（无 `control/<user>` owner 写入方、无保留期/EXPIRED 迁移），当前恒返回 `NOT_FOUND`；客户端不得把该 404 当作平台执行失败，连接中的插件应通过 Bridge 的 `PluginActionQuery` 查询。

每个 Action 都有：

```text
action_id
idempotency_key
deadline
requested_by
target
payload（由 oneof discriminator 唯一决定）
```

适配器执行后返回：

```text
ActionResult
  action_id
  status
  platform_message_id
  error（Error message，包含 code、message、details、request_id、trace_id 和 retryable）
```

## P07 目标 Action 契约（已冻结，未实现）

P06 之后外部插件 Action 只走 `PluginBridge.Connect`，适配器执行只走 `AdapterBridge.Connect`；unary `RuntimeService.SubmitAction/QueryAction/CancelAction` 是 runtime internal dispatcher/test 面（`target_spec.json` 标 `exposure=internal_dispatcher`），不对外。P07 目标补充：

- **下发与关联**：`CoreToAdapter.action` 仍为 `ActionEnvelope`；Runtime 在发送前填 `dispatch_attempt_id`（CSPRNG UUIDv4，重派发换新值）与 `dispatch_sequence`（1 起算递增）。插件提交必须留空/0，否则 `INVALID_ARGUMENT`。
- **结果与证据**：`ActionResult.execution_outcome`（并回带 `dispatch_attempt_id`）表示状态×执行已知性：`ACCEPTED`=`NOT_STARTED|UNKNOWN`，`SUCCEEDED`=`KNOWN_SUCCEEDED`，`FAILED`=`KNOWN_FAILED`，`EXPIRED`=`NOT_STARTED|UNKNOWN`，`CANCELLED`=`NOT_STARTED`。适配器用 `AdapterToCore.action_result` 上报终态（只允许 SUCCEEDED/FAILED）、用 `action_evidence` 上报 `NOT_STARTED/UNKNOWN` 非终态证据；Runtime 用 `CoreToAdapter.action_receipt` 回执 `RECORDED/DUPLICATE/EVIDENCE_ONLY/CONFLICT`，适配器收到 RECORDED/DUPLICATE/EVIDENCE_ONLY 才标记本地 journal 已上报，否则重连重发。
- **未知与迟到**：平台已执行但结果丢失保持 UNKNOWN，不自动重发平台；Action 已 EXPIRED 后的迟到结果只写 `adapter_result_evidence`+审计，不覆盖终态；已派发且未知的 Action 不跨 adapter_instance 重派（旧 Adapter 执行不确定不接管）。
- **取消与到期**：只有未派发 `ACCEPTED` 可取消；派发后 `CONFLICT`，不通知适配器、不谎称撤回。deadline 必须为未来 UTC 且 ≤300 秒（target 强制，`semantic.action.deadline_max_300s` 在实现前仍为 not_implemented）；提交时已过期持久化 `EXPIRED`+`NOT_STARTED`，不占配额、不调平台。终态从最后状态更新起保留 168 小时。
- **查询**：插件只查自身 owner（`PluginBridge` 的 `action_query`）；Control 用 `RuntimeService.QueryActionResult`（`action.inspect` + 恰含一个 `RESOURCE_ACCOUNT` 的显式 scope，按幂等键另需 `owner_instance_id`），不存在/不可见/超保留统一 `NOT_FOUND`，查询超时不改变状态。
- **权限/配额/策略**：每-kind 权限映射见上节；滑动窗口占用持久化（跨重启不重置，替代 reference 进程内窗口），Mute/Kick 另需 ActionPolicy（开关+群白名单+正预算，fail-closed；0 预算=禁用）与 UTC 日预算；取消/明确失败不自动退额；`RATE_LIMITED` 表示窗口/预算/容量，`PERMISSION_DENIED` 表示权限/白名单/策略，`CAPABILITY_NOT_SUPPORTED` 表示能力缺失，不新增错误码。
- 以上均为 target declaration（not_implemented）；reference 行为与差异只保留在 reference 章节与评审报告，不构成 target 验收。

## P08 目标 HostBridge 与受管执行契约（已冻结，未实现）

P08 的 Host 控制面复用 P06 冻结的两个监听：`HostBridge.Connect` 只注册在 Runtime 内部 mTLS `grpc_listen`，**不**注册在外部 `bridge_listen`；`HostService` 7 个方法只经该内部桥调用（`exposure=host_bridge`），不出现在 REST 绑定。Host 是平台服务，身份来自受控部署签发的 mTLS 客户端证书 + 部署预注册 allowlist（证书 SAN/指纹→`host_id`），**不使用** bootstrap 凭据、`ConnectionAuthorization`、`connection_proof` 或 refresh；`host.register` 等 proto 权限词表只用于方法目录语义，内部 mTLS 不套 Identity 用户授权。无新增服务/进程/权限/错误码。

- **帧与连接代次**：注册后每条 `HostToRuntime`/`RuntimeToHost` 帧带当前 `Fence`（`instance_id` 承载 `host_id`，`execution_generation` 必须缺省，`connection_generation` 由 Runtime 注册事务分配、`runtime_epoch` 为 Runtime 进程级随机值）。同一 `host_id` 新连接替换旧连接，旧 `connection_generation` 帧返回 `UNAUTHENTICATED`/`CONFLICT`；`HostRegisterHostResponse.fence` 返回本次连接 Fence。
- **心跳与离线**：Host 每 5s 发 `HostHeartbeat`（`sent_at`、`cursor`=已上报观察游标），Runtime 回 `HostHeartbeatAck`（`received_at`、`heartbeat_interval_seconds`、`resync_required`）。15s 无心跳只置 `online=false` 与 `process_outcome=UNKNOWN`，**不**写 FAILED/已停止、不 bump `HostRecord.version`（避免心跳风暴）；离线记录的历史版本不是当前执行事实。
- **执行观察**：不新增 `observation_id`；观察身份=`(host_id, connection_generation, cursor.sequence)`，命令结果额外用 `ExecutionRecord.command_id` 关联 `HostCommandReceipt.command_id`（空串=非命令触发的自发变化，不伪造 ID）。Runtime inbox 以 `source=Host, message=<host_id>:<connection_generation>:<sequence>` 去重；断线补播用 `resync_required`，不把缺失观察当失败。
- **四层状态**：L1 期望（Runtime PG `plugin_instances`/outbox/version）；L2 进程事实（Host 本地 SQLite `command_journal`/`managed_executions`/`execution_observations`）；L3 注册（P06 `RegisterPlugin`+Fence，P05 HostRegistration 投影）；L4 配置应用（P05 `InstanceConfiguration`+P06 delivery/ACK）。Host 离线不证明进程已停止；desired STOPPED + 旧观察 RUNNING 只到 UNKNOWN，绝不推断 KNOWN_SUCCEEDED。
- **执行与恢复**：Host 先持久 `command_id`+目标代次+payload hash 再事务外 `posix_spawn`；同键同 hash 重放返回原 receipt、异 hash `CONFLICT`；每次执行记录 marker=host_boot_id+pid+进程启动时刻（Linux `/proc/<pid>/stat` starttime / macOS `proc_pidinfo`），恢复先比 marker 再认领，不靠 PID 数字。崩溃在 spawn 后 receipt 前：启动回读 journal 重建确定 receipt，不重复 spawn；同 `execution_generation` 无法确认时命令转 Unknown 并拒绝再次 spawn，等 Runtime 提升 generation 后新命令。
- **跨主机 gate**：实例 `host` 变更前，旧 host 对当前 `execution_generation` 的最新观察必须终止（KNOWN_SUCCEEDED/FAILED）或有操作员显式处置记录，否则 operation `BLOCKED`（`HOST_OFFLINE_EXECUTION_UNKNOWN`/`OLD_EXECUTION_UNCONFIRMED`），不自动在别机启动；首轮无强制接管命令。
- **配置应用证据**：`desired_version/applied_version/apply_state/observed_at` 持久，`delivery_id` 只作最近一次连接投递证据（易失，不作为当前执行事实）。仅当前 fence 且 `config_version==desired_version` 的 ACK 推进 `applied_version`；旧 delivery/旧 config_version ACK 幂等 no-op；重启/断线后 `apply_state=UNAVAILABLE`，重连重新下发最新快照并重新 ACK（SDK 回调幂等）。
- **受管凭据与参数**：受管实例由部署为每个实例生成 opaque bootstrap（32B CSPRNG），以 `0600` 文件挂载到 Host；Runtime 部署配置只登记 SHA-256（同 P06 `bridge_credentials` 行）并可撤销，HostBridge 不传明文；Host spawn 时按 profile 允许列表从 `credential_env` 注入环境变量，SDK 读取后走 P06 `PluginBridge` 首帧 `authorization`（P06 消费路径不变）；文件/环境值不得进入 Runtime 存储/outbox/日志/观察/审计。`externally_managed=true` 实例仍走 P06 `CreateInstanceResponse.bootstrap_credential`。`arguments`/`environment_names` 保持 `repeated string`，冻结 `name=value` 白名单语法，无 wire 变更。
- 以上全部为 target declaration（not_implemented）；reference 无 Host/执行心跳/观察桥，reference 行为差异只保留在 reference 章节与评审报告，不构成 target 验收。

## P09/P10 目标 Storage、Namespace 与 Scheduler 契约（已冻结，未实现）

P09/P10 不新增服务/进程/监听/权限模型：storage 插件面只在 `PluginBridge` host_request 流帧，unary storage 方法为 runtime internal dispatcher；共享 namespace 管理与读是既有 Runtime owner 方法族新增；Scheduler 是既有服务，TimerTriggered 仍由 Runtime 产生。

- **P09 storage scope**：`NamespaceScope.instance_private` = 调用者稳定实例的私有 namespace；`installation_shared` 由服务端按已认证实例解析其安装的共享 namespace（未创建 → NOT_FOUND）；`installation_shared_id` 只用于内部/管理路径且必须等于调用者安装的共享 namespace，否则 PERMISSION_DENIED。插件不得自报 owner/installation。私有 scope 时 `NamespaceScope` 可缺省（=instance_private）。
- **CAS/condition**：共享 namespace 的 Set/Delete/CAS 必须带 `StorageCondition`（absent=create、version=update/delete），缺失 → INVALID_ARGUMENT；私有 Set 必须缺 condition（否则 INVALID_ARGUMENT），私有 Delete condition 可选（LWW），CAS 两种 scope 均可用；expected 不匹配 → CONFLICT，`FailureContext.current_version` 只回已授权范围版本。
- **TTL/配额/收据**：`expires_at` 绝对 UTC，必须 > DB now 且 ≤ now+30 天，越界 INVALID_ARGUMENT；过期键读取视为不存在，重放不续 TTL。每私有/共享 namespace 1024 键/1MiB、单值 ≤64KiB，安装聚合 8192 键/8MiB（加实例不能绕过）；写收据每实例 10000/安装 80000、7 天保留，复用通用 `command_receipts`（不新增 `storage_receipts` 表），容量满 RATE_LIMITED 且不淘汰有效记录。错误映射只用既有 ErrorCode（PERMISSION_DENIED/CONFLICT/RATE_LIMITED/INVALID_ARGUMENT/UPSTREAM_UNAVAILABLE/NOT_FOUND）。
- **共享成员/禁用**：`SetSharedMembers`/`DisableSharedNamespace`/`CreateSharedNamespace` 用 `namespace.manage`，`GetSharedNamespace` 用 `namespace.read`；均需 exact installation/namespace scope。插件访问共享 namespace 需 `storage.namespace` + 成员 `can_read/can_write` 双门禁；撤销以 namespace gate/成员版本同一行线性化，敏感共享读取每次在线检查 Identity。每安装至多一个共享 namespace（部分唯一索引）；必须先 Create 再 SetSharedMembers，插件首次写前未创建 → NOT_FOUND，不静默建。
- **P10 任务**：`task_id` 是 owner 内业务身份，`HostRequest.idempotency_key` 是命令幂等；owner_instance/installation 由 Runtime→Scheduler 实例委托 claims 派生，插件不可自报；同 task_id 异内容 CONFLICT，同内容返回当前状态，跨实例 NOT_FOUND。
- **时间/容量**：`run_at` 严格晚于 DB now 且 ≤ 可配置 horizon（默认 366 天）；`local_time` 年份 1970–2100、IANA 时区必须存在，DST gap/fold → INVALID_ARGUMENT，未知/路径型 zone → INVALID_ARGUMENT，tzdata 不可用 → UPSTREAM_UNAVAILABLE；换算以 DB UTC 为准。每实例 1024 / 每安装 8192 未终态任务，超限 RATE_LIMITED retryable=true，收据满也不淘汰有效记录。
- **状态映射与 COMPLETED**：wire `ScheduledTaskState` 与领域 TaskState/OperationState 映射冻结（PENDING/Pending/无 occurrence；RUNNING/Running/occurrence RUNNING；RETRYING/Retrying/occurrence BLOCKED；COMPLETED/Completed/occurrence SUCCEEDED；FAILED/Failed/occurrence FAILED 或 EXPIRED；CANCELLED/Cancelled/仅创建前）。Create 返回 PENDING；到期 claim 生成 occurrence 并持久 enqueue TimerTriggered 后 occurrence SUCCEEDED、task COMPLETED；**COMPLETED 只表示事件已持久入队，不代表插件回调成功或投递 ACK**。
- **取消窗口**：任务无触发意图（无 occurrence/outbox）→ 取消成功 CANCELLED 且保证不产生 occurrence；已进入交接（occurrence 存在且非 EXPIRED/FAILED）→ CONFLICT；已终态 → CONFLICT；同幂等键重复 cancel 返回原响应，重复到已 CANCELLED 返回当前记录。
- **交接/补偿**：trigger outbox 的 `HandoffContext.valid_until` 默认 7 天，attempts 上限 100；超窗/耗尽 → occurrence EXPIRED、task FAILED(reason=TIMEOUT, retryable=false)，不自动重建；人工通过 GetTask/GetOccurrence 观察后显式新建任务（运维流程见 doc/08）。Scheduler 以 `source=scheduler, message=occurrence_id` enqueue，Runtime 以 inbox 同键去重，重复交接返回 `TimerAcceptance.duplicate=true` 且不重复入队；`event_id := occurrence_id`。
- **TimerTriggered 来源**：只能由 Runtime 产生，适配器不可伪造；source 使用 Scheduler 服务主体（内部固定来源标识）。`AcceptTimerTrigger` 仍是 internal mTLS RuntimeService owner 方法（Scheduler 调用，不是外部 Bridge），不新增 exposure 枚举值。
- `CreateSharedNamespace`/`GetSharedNamespace` 为既有 Runtime owner 方法族新增（`namespace.manage`/`namespace.read`，exact installation scope，REST 映射保持 not_implemented 至 P12）；storage 4 个 unary 方法 `exposure=internal_dispatcher`；这 2 个新方法与 4 个 storage 方法、`HostService`/`SchedulerService` 的既有 caller/exposure 见 target_spec。以上全部 target declaration（not_implemented），reference 行为仍是唯一生产语义。

## P11 目标 Model 契约（已冻结，未实现）

P11 全部落在既有 ModelService/Model 进程内：无新增服务/进程/监听/权限/ErrorCode/存储所有权；外部插件仍只走 PluginBridge `HostRequest.model_chat`，Model 无外部 Bridge；Model 方法全部 `exposure=wire`（内部 mTLS owner RPC）。

- **调用身份**：Runtime 用 P04 交换出的 audience=Model 实例委托（chain `[Control,Runtime]`、direct_actor=Runtime、subject=插件主体）调用 Chat；Model 用 P04 `GetVerificationKeys` 缓存本地验签（aud/direct_actor/chain/`client_certificate`=实际 inbound Runtime 叶/`business_hops=2`/未过期），再向 Identity `CheckAuthorization(full_method=/kiarabot.v1.ModelService/Chat, scope={Model,model_id})` 复核当前权威；失败 `UNAUTHENTICATED`/`PERMISSION_DENIED`，不泄漏 Identity 细节。owner=委托 subject，不接受请求自报 owner/instance。
- **两次事务**：txn1 校验（context/deadline/messages 1..128/总 bytes ≤64KiB/role 闭集/logical_call_id UUIDv4/model ≤256）→在线授权与 model/alias 解析（禁用=`CAPABILITY_NOT_SUPPORTED`、秘密不可用=`UPSTREAM_UNAVAILABLE`）→per-model `max_concurrency` 满/DB 忙=`RATE_LIMITED + retry_after_ms∈[50,1000]` 且**不写记录**→服务端计算 request_hash→写 `model_calls`(state=STARTED, may_have_billed=false, secret/config 版本快照, retain≥7d)+audit(`model.chat.started`) 同 UoW 提交；commit=Unknown 不调用 Provider，返回 `UPSTREAM_UNAVAILABLE acceptance=UNKNOWN`，同 logical_call_id 查询。授权后 1 秒内必须开始 Provider 调用，超窗按 UNKNOWN 处理不再调用。
- **txn2 结果**：HTTP 200 单 choice 合法文本/usage→SUCCEEDED+KNOWN_SUCCEEDED、usage_known 按响应、may_have_billed=true、latency/provider_request_id；400/422→FAILED+KNOWN_FAILED+未计费（INVALID_ARGUMENT/CORRECT_INPUT）；401/403→FAILED+未计费（UPSTREAM_UNAVAILABLE/MANUAL_REVIEW，配置 DEGRADED）；404/405/410→CAPABILITY_NOT_SUPPORTED；429→FAILED+未计费（RATE_LIMITED/DO_NOT_RETRY，retry_after 截断 [0,60000]）；5xx/TLS/连接重置/响应超限/畸形 JSON/choices≠1/tool_calls/非法 usage→**BLOCKED+UNKNOWN+may_have_billed=true**（UPSTREAM_UNAVAILABLE 或 TIMEOUT，QUERY_SAME_INTENT）；等待/ deadline 超时→UNKNOWN+TIMEOUT。不变量：Provider 已调用后（txn1 已提交）除非明确 4xx/429 证据，任何路径不得写 KNOWN_FAILED/未计费；未决 UNKNOWN 只能经 Resolve 转终态，不得清理后盲重发。
- **重复 logical_call_id 回放**：hash 相同不进入 Provider，`disposition=REPLAY_IN_PROGRESS`（call RUNNING/UNKNOWN、无 result、必须查询）或 `REPLAY_TERMINAL`（终态/已处置，无 result、不重放正文、不新增计费）；hash 不同=`CONFLICT acceptance=DURABLY_ACCEPTED`（换新 call id 或修正内容）。
- **查询/处置**：`GetCallStatus` 按 subject 分权（Runtime 同委托只能查自身 subject；Control 需 `model.call.inspect`+显式 `{Model,model_id}` scope），返回状态/计量/disposition、不返回正文。`ResolveModelCall`（scoped Control、model.manage、幂等、expected_version CAS）只接受 disposition 1–4；NOT_EXECUTED→may_have_billed=false，EXECUTED_NOT_BILLED→false，EXECUTED_BILLED/ABANDONED→true；完成后 state=FAILED 终态、不再触发 Provider；重复处置/非 UNKNOWN=CONFLICT。管理命令（Update/Rotate/Disable/Enable/Resolve）复用 receipts/operations/审计，Chat 不造 operation。
- **密钥与版本**：根密钥由部署注入（`KIARABOT_MODEL_ROOT_KEY_FILE` 32B hex 0600/O_NOFOLLOW + `KIARABOT_MODEL_ROOT_KEY_VERSION`），**不入库、不与密文同表**；Provider secret 用 XChaCha20-Poly1305 AEAD（nonce 24B、AAD 含 model_id|secret_version|root_key_version|aead=1），只在 Model 调用 Provider 前解密、用完擦除；轮换只影响后续，在途固定 secret/config 版本；解密/根密钥故障→readiness Credentials Unavailable、Chat 不写记录返回 UPSTREAM_UNAVAILABLE。`GetModelConfiguration` 只回 secret_configured/secret_version，不回秘密。
- **管理生命周期**：`UpdateModel` 无 expected_version=创建（UUIDv4、唯一 alias、初始 DISABLED）、有=更新、已存在无版本=CONFLICT；`DisableModel`/`EnableModel`（scoped Control、model.manage、幂等、版本 CAS；删除归 P13）；Enable 后才可 Chat。
- **Provider/计费边界**：仅 `provider_kind="openai_chat_completions"`（HTTPS 任意主机或 http 仅 127.0.0.1；禁 userinfo/query/fragment/代理/重定向；TLS 系统根；Bearer 唯一认证；stream=false；单 choice；messages/结果 ≤64KiB、body ≤1MiB、timeout 1..30s）；错误只用既有 ErrorCode+acceptance/recovery/retry_after_ms；`may_have_billed` 是保守事实，真实账单需外部对账；不引入价格/货币/预算/扣费模型，不做流式/工具调用/多 choices/历史会话。
- **REST/P12**：`api/openapi.yaml` 的 target Model 端点保持 `not_implemented`；Disable/Enable/Resolve 的 REST 映射留 P12，本契约不改 API。
- 以上全部 target declaration（not_implemented），reference 行为仍是唯一生产语义；实现证据按 P11a–d 登记。

## 适配器注册协议

适配器启动后必须先注册：

```text
RegisterAdapterRequest
  adapter_id
  implementation
  implementation_version
  protocol_version
  accounts[]
  capabilities[]
```

Core 返回：

```text
RegisterAdapterResponse
  accepted
  assigned_instance_id
  required_protocol_version
  heartbeat_interval_seconds  心跳间隔秒数（uint32，字段 4）
  error（失败时返回）
```

## 插件协议

插件注册时发送 PluginManifest。Core 校验：

- 插件 ID 是否重复；
- API 版本是否兼容；
- 依赖是否满足；
- 权限是否允许；
- 配置 Schema 是否有效；
- 运行时是否被允许启动。

### 插件 Host API v1

T06a 在现有功能协商字段中增加 host.storage.v1 名称，不新增线协议字段；仅协商成功后使用 storage_get/set/delete。Core 原样关联 request_id，HostResponse.error 与 result 必须互斥。具体 TTL、限额和稳定错误见 doc/03-plugin-sdk.md；读取/写入必须获授 storage.namespace。失败状态不以空值代替；空 bytes 是合法存储值。其余 Host operation 在后续功能接线前明确 CAPABILITY_NOT_SUPPORTED。

`PluginBridge` 注册成功后，插件以 `HostRequest` 调用模型、私有存储和一次性任务服务；Core 使用相同 `request_id` 返回 `HostResponse`。请求必须携带非空 `request_id`、`trace_id` 和未来的 UTC `deadline`。`storage_set`、`storage_delete`、`task_create`、`task_cancel` 必须携带幂等键；同一插件和幂等键重放返回原结果，载荷不同返回 `CONFLICT`。响应必须恰好包含对应的结果或 `Error`，不得同时包含两者。插件身份来自已注册连接，不能由请求指定；所有调用均先经过 Core 权限检查。断线后未确认结果须由调用者以相同幂等键查询或重试，不能假定超时代表未执行。

- 模型请求包含模型名和角色/文本消息，角色仅允许 `system`、`user`、`assistant`；响应包含文本及输入、输出 token 数。API Key、上游响应正文和私密消息不得进入日志或错误详情。
- 存储的命名空间由 Core 根据插件身份绑定；键仅在该命名空间内有效。缺失或过期键返回 `NOT_FOUND`；删除不存在的键成功。可选过期时间为 UTC，大小和 schema 校验由 Core 策略决定。
- 任务属于调用插件，读取和取消其他插件的任务返回 `NOT_FOUND`。`run_at` 是 UTC；`local_time` 使用 IANA 时区的当地一次性时间。无效时区、模糊或不存在的当地时间返回 `INVALID_ARGUMENT`；不自动猜测夏令时偏移。任务状态由 `ScheduledTaskState` 定义。`ScheduleTask.schedule` 是原有 Action 字段，不在此处重新解释。
- 模型超时映射 `TIMEOUT`，限流映射 `RATE_LIMITED`，可重试的上游连接或服务故障映射 `UPSTREAM_UNAVAILABLE`，拒绝由 Core 映射 `PERMISSION_DENIED`；其他内部故障映射 `INTERNAL`。`retryable` 必须反映该次失败是否允许重试，不得包含密钥或上游原始内容。

## 权限、能力与订阅词表（v1 冻结）

以下名称是跨进程授权与功能协商的唯一词表。未知功能名不启用；未知订阅/能力名在注册时返回 `CAPABILITY_NOT_SUPPORTED`；未获授所需权限返回 `PERMISSION_DENIED`。词表与 `core/src/runtime/bridge_server.cpp`、`core/src/application/plugin_hub.cpp`、`core/src/application/action_router.cpp`、`core/src/application/host_dispatcher.cpp` 的实现映射一致，改动任一侧必须同时更新 `proto/tests/contract_status.json` 与本节。

<!-- vocabulary:start -->
功能协商名（`supported_features` / `enabled_features`，双向确认后才可用）：

```text
action.result.v1
host.storage.v1
host.model.v1
host.tasks.v1
plugin.config.v1
```

插件事件订阅能力名（写入 manifest `capabilities`），箭头后为所需权限：

```text
event.message.received       -> message.read
event.message.sent           -> message.read
event.group.member.changed   -> member.read
event.friend.request         -> request.read
event.adapter.status.changed -> adapter.read
event.timer.triggered        -> task.schedule
```

插件权限名（写入 manifest `permissions`，由宿主策略显式授予）：

```text
message.read
message.send
message.react
message.delete
member.read
member.mute
member.kick
request.read
request.approve
adapter.read
task.schedule
model.chat
storage.namespace
```

适配器能力名（写入 `RegisterAdapterRequest.capabilities`）：

```text
action.send_message.text.v1
action.react_message.v1
action.delete_message.v1
action.mute_member.v1
action.kick_member.v1
action.approve_request.v1
```
<!-- vocabulary:end -->

v1 不存在其他功能/权限名；`doc/03-plugin-sdk.md` 中更宽的规划词表不构成本契约承诺。`message.send`、`message.react`、`message.delete`、`member.mute`、`member.kick`、`request.approve` 只对 Action 生效且互不隐含；`message.read` 不授予任何控制面默认角色。

### P07 目标 Action 权限与能力映射（未实现）

P06 之后外部插件 Action 只走 `PluginBridge`；P07 目标的每-kind 授权把现有冻结字符串词表映射为封闭 `Permission` 枚举值（41–46），并要求 exact account/resource scope。P07 实现前该映射不生效，reference 路径仍按插件 manifest 权限字符串校验。

| Action kind | 权限字符串（manifest） | Permission 枚举值 | 适配器 capability |
| --- | --- | --- | --- |
| send_message（纯文本） | `message.send` | `PERMISSION_MESSAGE_SEND=41` | `action.send_message.text.v1` |
| react_message | `message.react` | `PERMISSION_MESSAGE_REACT=42` | `action.react_message.v1` |
| delete_message（bot 自撤回） | `message.delete` | `PERMISSION_MESSAGE_DELETE=43` | `action.delete_message.v1` |
| mute_member | `member.mute` | `PERMISSION_MEMBER_MUTE=44` | `action.mute_member.v1` |
| kick_member | `member.kick` | `PERMISSION_MEMBER_KICK=45` | `action.kick_member.v1` |
| approve_request | `request.approve` | `PERMISSION_REQUEST_APPROVE=46` | `action.approve_request.v1` |

`PERMISSION_ACTION_EXECUTE`/`PERMISSION_ACTION_INSPECT` 保留为方法级权限：unary `Submit/Query/CancelAction` 仅内部 dispatcher 使用（需要 `action.execute`），Control 查询需要 `action.inspect`。以上敏感权限不由默认角色派生，必须显式资源授予；Mute/Kick 另受账号 ActionPolicy 门禁。`schedule_task` 已 deprecated，不产生 kind/权限。

### P09/P10 目标 Storage/Namespace/Task 权限映射（未实现）

| 用途 | Permission 枚举值 | 作用域 / 方法 |
| --- | --- | --- |
| 插件 storage 读写/CAS | `PERMISSION_STORAGE_NAMESPACE=32`（既有） | 认证实例解析出的私有/共享 namespace；外部插件经 `PluginBridge` `HostRequest.storage_*` 帧调用，4 个 unary storage 方法仅 internal dispatcher |
| 共享成员/禁用管理 | `PERMISSION_NAMESPACE_MANAGE=27`（既有） | exact installation/namespace scope；`SetSharedMembers`、`DisableSharedNamespace`、`CreateSharedNamespace` |
| 共享 namespace 管理读 | `PERMISSION_NAMESPACE_READ=47`（P09 新增） | exact installation scope；仅 `GetSharedNamespace`（scoped Control） |
| 任务创建/取消 | `PERMISSION_TASK_SCHEDULE=34`（既有） | owner 由 Runtime 实例委托 claims 派生；`CreateTask`/`CancelTask` |
| 任务/occurrence/operation 查询 | `PERMISSION_TASK_READ=33`（既有） | exact instance/installation scope；`GetTask`/`ListTasks`/`GetOccurrence`/`GetOperation` |
| Runtime 接收 TimerTriggered | `PERMISSION_TIMER_ACCEPT=35`（既有） | Scheduler 服务主体（内部 mTLS owner service），不是外部 Bridge |

以上权限均不由默认角色派生，必须显式资源授予；`namespace.manage/read` 绑定 exact installation scope，Control 不得以 admin 隐式越权。外部插件的 storage/task 入站统一走 `PluginBridge` 流帧，unary storage 方法不对外。

## 错误模型

T08a 控制面 REST 新增会话查询/撤销及当前主体接口，具体响应、RBAC、幂等与分页在 OpenAPI x-control-policy 冻结；WebSocket 的关闭码/权限/撤销/重同步在 AsyncAPI x-control-session-policy 冻结。REST 503 还覆盖 PLUGIN_UNAVAILABLE 和 CAPABILITY_NOT_SUPPORTED；其余映射见 doc/06-control-api.md。这是同进程应用端口，不新增 Protobuf 管理 service，也不改变既有 gRPC 错误映射。

T06c 使用 host.tasks.v1，task.schedule 权限和 event.timer.triggered 订阅；task_get/cancel 无订阅要求但仍需权限。字段及状态数值不变；文档中的 PENDING/RUNNING/RETRYING/COMPLETED/FAILED/CANCELLED 均是 ScheduledTaskState 的 SCHEDULED_TASK_ 同名枚举缩写，不能与 ActionStatus 混用。非法日期/时区/DST/过期任务时间 INVALID_ARGUMENT，非所有者 NOT_FOUND，幂等/终态取消/旧 claim 冲突 CONFLICT，限额 RATE_LIMITED，存储错误按既有映射返回。创建/取消失败不产生事件；详细验收与取消边界见 doc/01-runtime-semantics.md。

T06b 增加现有功能协商名 host.model.v1，不新增字段。model_chat 使用对应 HostResponse.model_chat；模型未配置或未协商 CAPABILITY_NOT_SUPPORTED，未获授 model.chat 为 PERMISSION_DENIED。TIMEOUT 和不确定 UPSTREAM_UNAVAILABLE 不可自动重试（retryable=false），RATE_LIMITED 可稍后重试。模型不属于上述写操作幂等回放范围；详细限制和 HTTP 映射以 doc/05-infrastructure.md 的 T06b 实现范围为准。

错误必须包含稳定错误码，不让前端或插件解析自由文本：

```text
INVALID_ARGUMENT
UNAUTHENTICATED
PERMISSION_DENIED
NOT_FOUND
CONFLICT
CAPABILITY_NOT_SUPPORTED
RATE_LIMITED
TIMEOUT
UPSTREAM_UNAVAILABLE
PLUGIN_UNAVAILABLE
INTERNAL
```

错误码在 Protobuf、REST 和 WebSocket 中保持同名语义。两个 Bridge 的 gRPC 状态只用于认证、权限、参数和传输层：`UNAUTHENTICATED`、`PERMISSION_DENIED`、`INVALID_ARGUMENT`、`INTERNAL`、`CANCELLED`、`DEADLINE_EXCEEDED`。**业务错误（`CONFLICT`、`NOT_FOUND`、`RATE_LIMITED`、`TIMEOUT`、`UPSTREAM_UNAVAILABLE`、`PLUGIN_UNAVAILABLE`、`CAPABILITY_NOT_SUPPORTED`）不在 gRPC 状态中映射，而以 in-band `CoreToAdapter.error` / `CoreToPlugin.error` 或注册响应内的 `Error` 承载。**旧版本文档中的 `CONFLICT→ALREADY_EXISTS/ABORTED`、`RATE_LIMITED→RESOURCE_EXHAUSTED`、`TIMEOUT→DEADLINE_EXCEEDED` 业务映射已废止（`semantic.grpc.business_error_mapping` = deprecated），客户端不得按那些状态码判定业务结果。

跨协议的稳定枚举词汇必须逐项一致：

- `ErrorCode`：`ERROR_CODE_UNSPECIFIED`、`INVALID_ARGUMENT`、`UNAUTHENTICATED`、`PERMISSION_DENIED`、`NOT_FOUND`、`CONFLICT`、`CAPABILITY_NOT_SUPPORTED`、`RATE_LIMITED`、`TIMEOUT`、`UPSTREAM_UNAVAILABLE`、`PLUGIN_UNAVAILABLE`、`INTERNAL`；
- `HealthState`：`HEALTH_STATE_UNSPECIFIED`、`HEALTHY`、`DEGRADED`、`UNHEALTHY`；
- `PluginState`：`PLUGIN_STATE_UNSPECIFIED`、`PLUGIN_STATE_DISABLED`、`PLUGIN_STATE_STARTING`、`PLUGIN_STATE_READY`、`PLUGIN_STATE_RUNNING`、`PLUGIN_STATE_STOPPING`、`PLUGIN_STATE_STOPPED`、`PLUGIN_STATE_UNAVAILABLE`、`PLUGIN_STATE_FAILED`；
- `ActionStatus`：`ACTION_STATUS_UNSPECIFIED`、`ACCEPTED`、`SUCCEEDED`、`FAILED`、`EXPIRED`、`CANCELLED`。

这些枚举在 Protobuf、`api/openapi.yaml` 的 REST schema 和 `api/asyncapi.yaml` 的组件 schema 中使用相同名称；客户端必须保留未知值以兼容后续新增枚举。WebSocket 频道及 payload 定义以 AsyncAPI 为准。

已发布的 `PluginStatusChanged.state`（字段 3）保持为 `HealthState`；新增 `optional int32 lifecycle_state = 5`，其数值以 `plugins.proto` 中的 `PluginState` 为唯一来源，不重新定义枚举。缺失表示事件没有提供生命周期状态，显式 0 表示 `PLUGIN_STATE_UNSPECIFIED`；未知数值必须保留。健康状态为 `HEALTH_STATE_UNSPECIFIED` 表示未知，两个状态不能互相推断。旧消费者忽略新增字段，旧生产者不提供生命周期状态。

`events.proto` 不能导入依赖它的 `plugins.proto`；整数载体避免循环依赖，消费方需要显式校验或保留 `PluginState` 的数值，不能依赖生成代码的枚举类型检查。

## 标识符语法（v1 统一）

所有标识符非空；长度按 UTF-8 字节计。不同边界不再各自发明长度/字符集，统一按下表执行；`proto/tests/check_semantic_rules.py` 会对照本节与 OpenAPI/AsyncAPI 参数。

<!-- id-rules:start -->
| 标识符类别 | 长度上限 | 字符集/格式 | 使用边界 |
| --- | --- | --- | --- |
| Action `action_id`、Action/Host `idempotency_key`、`request_id`、`trace_id` | 128 | `[A-Za-z0-9._-]` | Protobuf Action/Host、Bridge 请求元数据、REST `ActionId` / `ActionLookupKey` |
| `adapter_id`、`adapter_instance_id`、`account_id` | 128 | `[A-Za-z0-9._:@-]` | 适配器注册/事件入口；Action `target_source` 目前使用更窄字符集（见状态表） |
| `conversation_id`、`message_id`、`event_id`、`task_id` | 256 | 可见 ASCII，不含空白与控制字符；适配器可在会话 ID 中使用 `group:` / `private:` 前缀 | 事件/Action/Host 存储键 |
| 插件 `id` | 64 | `^[a-z0-9][a-z0-9.-]*$` | manifest.id、REST `PluginId`；Bridge 暂容忍更长值，客户端必须按本节 |
| 流 `session_id` | 最小 32、最大 256 | 不透明秘密，不记录、不入 URL | AsyncAPI `SESSION_READY`、REST `SessionId` |
<!-- id-rules:end -->

Action `deadline` 为未来 UTC 时间，契约上限 300 秒；v1 仅校验正时间戳，上限强制执行是待接线项（`semantic.action.deadline_max_300s` = not_implemented）。

## 优雅停机与 drain

reference-era Core 关闭 `AdapterBridge` / `PluginBridge` 时直接取消或结束流，不发送 `ShutdownNotice`；P06 target Runtime 在停止信号后先发送有界 `ShutdownNotice`（reason/drain_timeout_ms=1000，单次写有 1s deadline），随后关闭监听与流。因此：

- 客户端收到流关闭（无论 gRPC 状态）都视为**未知结果**，不得推断事件未入库或 Action 未执行；
- 重连后必须重新注册，并按 `event_id` 幂等重投未确认事件、按认证调用方 + 幂等键查询未确认 Action；
- **P06 target 行为**：`ShutdownNotice.reason` 给出原因，`drain_timeout_ms` 给出在途处理上限（当前 1000ms）：客户端应停止领取新工作、在上限内完成并确认在途请求，然后正常关闭。未收到该帧时的行为与 reference 相同；旧客户端必须忽略未知 oneof 成员。

## 代码生成和字段校验

协议实现必须使用仓库根目录的命令：

```bash
cmake -S . -B build -DKIARABOT_GENERATE_PROTO=ON
cmake --build build --target kiarabot_proto
```

生成输出只能放在 `build/generated/`。CI 还必须生成 descriptor 并比较基线，检测字段编号复用和不兼容删除：

```bash
protoc --descriptor_set_out=build/kiarabot-v1.pb \
  --include_imports -I proto proto/kiarabot/v1/*.proto
```

字段规则：标识符长度/字符集按上文“标识符语法”表；时间戳使用 UTC；`page_size` 默认 20、最大 100；`deadline` 最长 5 分钟（300 秒，强制执行待接线）；消息段总大小默认 1 MiB。

## 兼容性

- Protobuf 使用 `kiarabot.v1`、`kiarabot.v2` 版本空间；
- 新增字段不影响旧客户端；
- 删除字段前先标记 deprecated；
- 已删除的字段编号永不复用；
- 枚举新增值时客户端必须允许未知值；
- 协议变更必须有兼容性测试。

### reserved / deprecated 可执行规则

- 删除任何字段或枚举值前，先在 `.proto` 标记 `[deprecated = true]`，并在 `proto/tests/contract_status.json` 登记为 `deprecated`、在本文档状态表出现；`check_implementation_status.py` 会拒绝只写注释或只改文档的不一致。
- 实际删除时，字段号/枚举号必须写入 `reserved`（字段用数字或范围，枚举用名称或数值范围）；`reserved` 不得与现存字段号/枚举值重叠，编号永不复用。`check_semantic_rules.py` 校验当前 `.proto` 的 reserved 语法与重叠；CI 可用时 descriptor 兼容性检查再做跨版本比较。
- 枚举禁止 `allow_alias`（重复数值会让“重编号”不可检测）；v1 的名称/数值冻结在 `proto/tests/enum_baseline.json`，新增值允许但必须在同一变更更新基线，删除或改名直接失败。
- 未实现的已声明项不能以“未来会实现”为理由进入 `implemented`；状态以运行接线与测试证据为准，见“契约实现状态”。

## 不属于本层的内容

- 不实现 WebSocket 连接；
- 不解析 OneBot 原始 JSON；
- 不实现数据库；
- 不实现插件逻辑；
- 不实现控制面板页面；
- 不保存运行时状态。

## 验收标准

- Python 和 C++ 都能生成客户端；
- 一条事件可以从适配器发送到 Core，再转换为 Action；
- 重复 `event_id` 不会造成重复处理；
- 未知字段不会导致旧客户端崩溃；
- 错误码和协议版本有自动化测试；
- 枚举名称/数值基线、契约实现状态清单、`x-contract-status` 与文档状态表的一致性由可执行门禁检查；依赖不可用时以未运行报告，不能当作通过。

## 代码启动句

> 请只创建 `proto/kiarabot/v1` 的 Protobuf 协议和生成测试，先实现事件、Action、适配器注册、插件注册及稳定错误码，不实现任何平台或业务逻辑。

## 桥接连接认证（T01b）

两个 Connect RPC 的 metadata 必须恰好包含一个 `authorization: Bearer <token>`。凭据由宿主配置绑定角色（adapter/plugin）与资源 ID，不从请求取得授权身份。缺失、重复、错误格式或未知凭据返回 gRPC UNAUTHENTICATED；角色不匹配或注册 ID 冒用返回 PERMISSION_DENIED。错误详情 `Status.error_details()` 为序列化的 `kiarabot.v1.Error`，客户端使用 code 判定，不解析自由文本。

未配置相应 T02a/T03a 业务入口时，已认证连接的首条请求返回 `CAPABILITY_NOT_SUPPORTED` / gRPC INTERNAL；不会发送 accepted、持久化确认或心跳成功。空消息返回 INVALID_ARGUMENT，客户端正常半关闭无消息返回 OK；取消及 deadline 使用 gRPC 传输状态 CANCELLED/DEADLINE_EXCEEDED，不代表业务成功。关闭服务取消等待中的流并等待处理器退出。已接线的注册和权限约束见 T02a/T03a 对应章节。

## T02a 注册与 inbox 确认

启用持久化接收的 AdapterBridge 首帧必须是 RegisterAdapterRequest。协议版本固定 kiarabot.v1；账号至少一个，必须 enabled 且属于 bootstrap 的 adapter_instance 的 A1 账号绑定（事件帧同样按该绑定校验，跨账号注入以 PERMISSION_DENIED 终止且不持久化/不 fan-out）；自报账号及 Capability 不授予权限。注册成功返回 accepted、`assigned_instance_id` = bootstrap credential 的 resource_id（BindAdapter 传入的 adapter instance，不是随机新 id）、required_protocol_version 和 30 秒心跳间隔；auth/输入类失败以稳定 gRPC status 终止，业务拒绝返回 accepted=false 和稳定 Error 后正常结束流。新注册替换同身份旧实例，旧连接不能继续入库。

CoreToAdapter.event_ack 使用新编号 4。AdapterEventAck 包含 event_id、trace_id、status、error、retry_after_ms：PERSISTED 表示 inbox/会话水位/审计和幂等记录已原子提交，DUPLICATE 表示同内容已提交，REJECTED 表示本次未接收。只有前两者是确认；不是插件 ACK 或 Action 完成。

内容或顺序冲突使用 CONFLICT，错误 schema/缺失字段使用 INVALID_ARGUMENT，无账号授权或伪造 Core 内部事件使用 PERMISSION_DENIED，队列满 RATE_LIMITED，存储不可用 UPSTREAM_UNAVAILABLE，损坏/内部失败 INTERNAL。RATE_LIMITED、存储暂时故障及版本提交冲突可重试，retry_after_ms=1000；内容冲突不可重试。传输超时/取消依旧使用 gRPC 状态，确认丢失不能推断未入库。未装配 ActionRouter 时 ActionResult 明确拒绝 CAPABILITY_NOT_SUPPORTED / gRPC INTERNAL；T04b 装配后按已认证适配器归属验证并持久化结果。未配置持久化入口时保留 T01b 拒绝语义。

## 配置下发闭环

配置通过 plugin.config.v1 / PluginConfig / PluginConfigAck 下发并按连接确认。REST GET 的 delivery 区分持久化与运行实例应用，PUT/幂等重放只确认持久化。状态、超时、重连、错误及事件门控的完整规则见 [PluginSDK 配置下发闭环](03-plugin-sdk.md#配置下发闭环t08b-后续)。协议唯一来源为 proto/kiarabot/v1/plugins.proto，REST 为 api/openapi.yaml；无新增错误码或 HTTP/gRPC 映射。

## P01 目标公共类型（尚未实现）

`platform.proto` 定义资源类型、稳定实例 Fence、RequestContext、Operation/OperationRef、FailureContext、HandoffContext、StreamCursor 与 DeliveryAttempt。所有新增项均为 not_implemented。内部层必须映射为独立值类型，不能依赖生成消息。目标 ID 使用小写 UUIDv4，原 event/action/task ID 沿来源规则；版本、执行代次、连接代次必须正值，presence 缺失或 0 均 INVALID_ARGUMENT。正文自报身份不可信；认证绑定由传输验证。

操作采用 OPERATION_ACCEPTED/RUNNING/BLOCKED/SUCCEEDED/FAILED/CANCELLED/EXPIRED；BLOCKED 非终态且带 reason/next_attempt_at；stalled 仅观测标记。执行结果采用 EXECUTION_NOT_STARTED/KNOWN_SUCCEEDED/KNOWN_FAILED/UNKNOWN，与操作状态独立。目标 gRPC 映射以 target_spec.json 为准：CONFLICT→ABORTED，CAPABILITY_NOT_SUPPORTED→UNIMPLEMENTED；流错误帧与 unary status details 都保留稳定码及关联 ID。Unary RpcFailure 包含受理事实、恢复建议和操作引用；无法收到响应仍按原意图查询。

## 目标方法元数据扩展（proto 唯一来源）

`proto/kiarabot/v1/policy.proto` 的 `google.protobuf.MethodOptions` 扩展是方法权限与在线授权的唯一权威：`required_permission`、`requires_idempotency`、`default_deadline_ms`、`max_deadline_ms`、`requires_online_authorization`，以及 P04-B 新增的 `repeated ResourceKind scope_kinds = 51006`。`scope_kinds` 只标注**资源种类语义已冻结**的消费者方法：`ModelService.Chat`、`UpdateModel`、`RotateProviderSecret` 标注 `RESOURCE_MODEL`；P05 follow-up 为 Runtime exact 资源方法补 `RuntimeService.GetAccount/UpdateAccount/DisableAccount/BindAdapter/UnbindAdapter`→`RESOURCE_ACCOUNT`、`GetAdapter`→`RESOURCE_ADAPTER`、`GetInstallation`→`RESOURCE_INSTALLATION`、`GetInstance/GetInstanceConfig/SetDesiredState/UpdateConfig`→`RESOURCE_INSTANCE`。P12m 为原生栈暴露的 4 个 owner 方法补登记：`RuntimeService.GetActionPolicy/UpdateActionPolicy`→`RESOURCE_ACCOUNT`（ActionPolicy 归账号所有）、`RuntimeService.QueryActionResult`→`RESOURCE_ACCOUNT`（P07 `action.inspect` 查询的请求载体是恰含一个 `RESOURCE_ACCOUNT` 的 account scope；target_spec 原无 scope 定义，按该现有请求语义选定并登记）、`ModelService.ListModelMetadata`→`RESOURCE_MODEL`（对照 Chat）。RuntimeService.GetOperation 已登记 ACCOUNT、ADAPTER、INSTALLATION、INSTANCE、HOST；在线授权后按持久化的完整 OperationRef 核对归属，不匹配返回 NOT_FOUND。Create/List/GetResourceState 等尚无单一 exact scope 或应用仍传空 scope 的方法**不标注**，由 descriptor-driven resolver 返回空并由上层 fail closed，不按方法名、请求字段或权限反推 scope；后续补 scope 必须同时改应用授权输入并登记 target_spec。`proto/tests/target_spec.json` 只镜像 reviewed 标注供 `target_contract_test.py` 校验，descriptor 仍是第一权威。禁止手写方法→scope 表。

## P01 五服务方法与权限（尚未实现）

目标消息、方法和权限的机器可读索引是 `proto/tests/target_spec.json`；RPC 唯一来源仍为对应 `.proto`，`policy.proto` 的 descriptor options 将封闭 Permission、是否需业务幂等、默认/最大等待 deadline 和在线核验写到每个方法。不得把所有 Identity 方法向所有服务开放：Login/Refresh/人类 Session 入口仅 Control，其他方法须 mTLS 服务及逐方法范围授权。登录不要求已有最终用户会话，但要求可信 Control、限频/审计。CheckAuthorization 500ms/最大1秒；普通查询2秒/最大5秒；命令5秒/最大30秒；Chat 总预算30秒且不自动重发供应商。Chat logical_call_id 单独防重，不套普通命令重放正文。

服务方法均有独立请求/结果类型；开放 Struct 仅用于插件配置，不能用于通用 operation dispatch。Runtime 私有存储 scope 由身份解析，共享 CAS 用 absent/version oneof；SetStorage 只允许私有覆盖，共享写和删除要求 StorageCondition，version 必须正值，absent 必须 true。Conflict 的 RpcFailure.context.current_version 仅在已授权范围返回版本。共享成员需同时获 Identity 授予和 namespace gate 允许。Get/UpdateActionPolicy 提供已批准的账号 Mute/Kick 启用、白名单和正预算配置，不扩展其他 payload。

五服务全部为目标声明、无生产接线。Host 包只能使用已预置摘要/launch_profile，arguments 与 environment_names 必须命中部署白名单，不能传任意路径/shell；command_id 与 HandoffContext 持久幂等。Scheduler CreateTask 的 owner 由受限实例委托派生，时间 oneof 只接受 UTC/IANA 一次性，取消在触发交接开始后冲突。Model Chat 仅 system/user/assistant 文本，128条/64KiB输入、1MiB上游响应/64KiB结果；call 查询仅状态与计量，不存正文历史。RotateProviderSecret/RotateCredential 的秘密输入/一次性交付不得进入日志/outbox，收据加密且复核当前轮次。

### Model owner查询补全（目标未实现）

Model.GetOperation补齐UpdateModel/RotateProviderSecret返回operation后的权威查询链，owner_service必须model；服务端对照已存operation的resource，不信客户端以另一scope重贴标签。GetModelConfiguration需model.manage在exact Model资源上在线核验，返回无凭据的ModelConfiguration、secret_configured和权威observed_at。endpoint不可承载/回显URL凭据；供应商secret只走已有机密更新入口。未知/不可见统一NOT_FOUND，依赖故障UPSTREAM_UNAVAILABLE，超时TIMEOUT且不改业务状态。两方法是typed RPC，尚无生产实现；对应HTTP配置读取与多owner操作路由以OpenAPI为唯一来源。

### Runtime typed资源读取（目标未实现）

Runtime增加Account/Installation/Instance/Adapter/Host五类Get/List和GetInstanceConfig，不用通用state或Struct代替资源record。Get必须exact正确kind与server UUID；List的catalog.kind只能RESOURCE_CATALOG，目录必须离线登记为runtime及该资源族，空目录/all/错kind一律INVALID_ARGUMENT或当前授权拒绝，不表示全资源。目录许可不授予任何child；owner按当前本人具体授权资源相交，其他人的资源/总量不泄漏。新增Permission只追加account.read/plugin.read/adapter.read/plugin.config.read，Host复用host.read；固定角色的能力仍需逐资源grant，不默认message.read。

新列表以PageRequest输入，0页大小取20、最多100，token最多1024；返回SnapshotInfo含owner、权威observed_at、authorization_version、UUID快照和不晚于10分钟的expires_at/PageInfo。每页当前检查，scope/权限版本/页大小改变使cursor失效。新AdapterRecord把最后有效注册metadata与当前online/Fence分开，offline不带current_fence，未知实现不伪装NoneBot2；HostRegistrationRecord只是Runtime登记投影，不宣称OS进程已停。关联Ref不授予访问关联对象。

InstanceConfiguration返回desired/applied独立版本、值和应用状态；applied_version可0，REJECTED必须稳定Error，秘密只引用/脱敏，不回显token。未ACK最新配置不变成已应用；离线记录历史版本并非当前执行事实。所有读取默认2秒/最多5秒且在线授权；不存在/不可见NOT_FOUND，Identity/owner依赖不可用UPSTREAM_UNAVAILABLE，不返回成功空页。

## Identity 当前投影与凭据状态（目标未实现）

Identity 的 ValidateSession/GetPrincipal 返回当前主体的逐条 AssignmentRecord、authorization_version 和权威 UTC checked_at。每条绑定独立计算权限与 exact scope，不能把显示 roles/permissions 与全部 scope 做笛卡尔积授权；PrincipalRecord.roles/display_permissions 仅是当前绑定的显示并集。固定角色只用于 human；显式权限使用 role=UNSPECIFIED 加非空、去重的封闭 Permission 列表，两种授予依据不能混用。message.read/action.inspect 等敏感权限不由默认角色或创建资源自动授予。空 scope 不代表全局。

SetAssignment 输入 AssignmentSpec：创建不带 assignment_id/expected_version，Identity 生成 UUID/version=1；更新二者同时提供且版本为正，主体不可更换，并核验旧、新 scope 并集上的当前授予权限。目标尚未发布，原开发快照的 AssignmentRecord 请求已统一替换为 Spec，不提供旧请求桥接。

SessionRecord.created_at 是 Identity 持久保存的首次创建时间，刷新/撤销不改写。会话状态以同响应 checked_at 判定：revoked 优先 REVOKED，其次 checked_at >= expires_at 为 EXPIRED，否则 ACTIVE。Login/Refresh 的剩余 access TTL 为 max(0,floor(access_expires_at-checked_at))；Control 不用本地时间、token iat 或占位时间补齐。缺失权威字段、非当前绑定或授权依赖不可用必须拒绝，不能伪造空权限/成功。

RegisterPrincipal 同事务创建 Disabled Principal 与无秘密 PENDING 凭据引用（allowed_services 作为 Pending audiences 持久化）；尚未发放的 PENDING 不得伪造 expires_at。首轮成功的 RotateCredential（WorkloadBootstrap 或 human Password）在同一事务激活 principal；RevokeCredential 撤销后该凭据的 Login/Validate 当前凭据门禁失败。WorkloadBootstrap 交付一次 opaque token（24h），human Password 交付一次服务端临时密码并重新派生 verifier，CertificateBinding 轮换明确 CAPABILITY_NOT_SUPPORTED。expected_version 进入 keyed request commitment，同键异版本 CONFLICT；未授权与不存在统一 NOT_FOUND，一个精确 binding 只对应一个 Principal。P04-A 应用/仓储/RPC 已接线；P05f 已在 `main@439fb9e` 通过本机完整门禁与真实 PG16.4/TLS 测试（28 项 `p04_identity_*` + `p04_control_auth_https`），但 target 契约状态仍按独立 target evidence 逐项登记，未批量翻转；未翻项见 [P04 报告](../reports/modules/architecture-p04-identity.md)。

## 外部 Bridge 连接授权（目标未实现）

采用已批准的最小三载荷 ConnectionAuthorization / RefreshConnectionAuthorization / ConnectionAuthorizationResult，唯一来源 platform.proto 与 adapters.proto/plugins.proto。Adapter/Plugin 成功登记必须带 authorization，且与响应 Fence 一致；注册后入站帧必须带完整 Fence 和当前 connection_proof（续期正文 proof/Fence 也须与帧一致）。反向 refresh 结果按 request_id 返回恰好一个新 authorization 或稳定 Error，不能空成功。缺少 proof/授权不允许旧流回退。

proof/refresh_credential 是独立 CSPRNG 至少256位 opaque秘密，绑定实际已认证 TLS 流、principal、完整 Fence、Runtime epoch、credential_version；不能把客户端自报证书/Fence 当绑定。Runtime只保存摘要和当前受信连接状态，不把秘密写入公开投影、日志或outbox。bootstrap仅登记换取受限证明，不授权Action/Model/存储等业务。内部受限委托仍是Ed25519 JWT，平台owner监听仍mTLS；外部Bridge在Runtime进程内使用独立可选监听（见下节），不修改P02 owner mTLS监听。

issued_at/expires_at 为受信UTC，名义寿命最多56秒；含既定2秒节点偏差和最多2秒处理容差总预算不超过60秒。服务端同时以单调期限和受信UTC约束，每秒检查到期、撤销、凭据版本和资源gate；客户端到期前10秒单飞续期。高风险操作仍逐次在线授权。Identity不可用不能刷新或无限延长旧流，停止新副作用并返回UPSTREAM_UNAVAILABLE；不伪造PERMISSION_DENIED。

refresh只接受当前实际流、未过期未撤销的proof/refresh及正refresh_generation，在线验证当前Identity主体/凭据状态与gate后原子轮换二者和generation；旧材料不可再次换发。材料/流/Fence失配或过期UNAUTHENTICATED并关闭流，已认证请求generation冲突CONFLICT，明确失权PERMISSION_DENIED，依赖故障UPSTREAM_UNAVAILABLE，期限TIMEOUT。失败不轮换、不复活失效授权。refresh无持久秘密收据；响应丢失视授权未知，关闭旧流，经仍有效的登记凭据重新bootstrap产生新connection_generation，禁止重放旧refresh猜测结果。

授权过期停止新投递、入站受理、续租和副作用，不把已有Action/Model事实变FAILED。auth刷新不续Delivery租约；ACK/Renew仍核验有效attempt、完整Fence及现有事实规则。适配器本地已执行结果在断线后通过新认证连接按原account/action ID交回，不能据失效proof新执行。所有新增字段/帧target均not_implemented，oracle和wire往返不证明生产授权或并发正确性。

### P06 外部 Bridge 监听与 bootstrap（已冻结，未实现）

P06 的拓扑冻结为：外部 Adapter/Plugin 的注册/事件/ACK/续租只走 `AdapterBridge.Connect` / `PluginBridge.Connect` 双流；`RuntimeService` 的 `RegisterAdapter`、`Heartbeat`、`PublishEvent`、`RegisterPlugin`、`HeartbeatPlugin`、`ClaimEvent`、`DeliverEvent`、`AckDelivery`、`RenewDelivery` 是内部 dispatcher/测试面（`target_spec.json` 标 `exposure=internal_dispatcher`），不暴露给外部客户端，P06 不实现。外部监听是同进程内新增的可选 `bridge_listen`（server-verified TLS，不请求客户端证书）；`grpc_listen` 的平台 owner mTLS 监听与既有服务不变。监听配置、fail-closed 规则和运维要求见 `doc/08`。

bootstrap 凭据由 Runtime 逐实例生成（32字节 CSPRNG opaque 文本，Runtime 只存 SHA-256），经一次响应返回给 Control/部署：Adapter 用 `RuntimeBindAdapterResponse.bootstrap_credential`（字段3），externally_managed 插件实例用 `RuntimeCreateInstanceResponse.bootstrap_credential`（字段2）；Host 受管实例不带该字段。外部客户端以 gRPC metadata `authorization: Bearer <bootstrap_credential>`（恰好一个，T01b 语义）连外部监听，首帧 `RegisterAdapterRequest`/`RegisterPluginRequest` 必须与凭据解析出的资源/代次一致，成功响应返回 `Fence` + `ConnectionAuthorization`。此后每帧必须带当前 `Fence` 与 `connection_proof`；失配/过期/撤销走 gRPC `UNAUTHENTICATED`（终止流）或 `PERMISSION_DENIED`（明确失权），业务拒绝仍用 in-band `Error`。refresh 只允许同一实际流单飞轮换 proof/refresh_generation；失败/丢响应不重放旧材料，客户端关闭旧流并用仍有效的 bootstrap 重连，产生新 connection_generation。Runtime 重启换 runtime_epoch，旧流/fence 全部失效。

投递语义：`delivery_id` 定义为 `event_id`，实例由 `target_instance_id`（ReplayDelivery）或 `last_attempt.fence.instance_id`（DeadLetter）限定，不新增字段；`ReplayDelivery.expected_version` 在 P06 必须未设置或 0。ACK/Renew 与 attempt 过期/重复竞争的结果以 `DeliveryReceipt.disposition` 表达：当前 active attempt 首次成功 ACK=`DELIVERY_ACKNOWLEDGED`、同 attempt 重复成功=`DELIVERY_DUPLICATE`、旧/过期 attempt=`DELIVERY_STALE`（不改新尝试）、插件拒绝=`DELIVERY_REJECTED`（按 error.retryable 决定退避或死信）、续租成功/重复=`DELIVERY_RENEWED`。死信与重放语义见运行时语义 P01/P06 节。以上字段/语义在 P06 实现前均为 target not_implemented。

## P04 target transport codec（独立边界工具）

`runtime/platform/target_codec.hpp/.cpp` 仅在外层把内部 target 值类型与生成消息/gRPC 转换；domain/application/ports 不引入生成类型。公开转换返回 Outcome，非法/缺失值和未知/UNSPECIFIED 枚举返回 InvalidArgument，异常转 Internal，不抛出边界。消息 presence 属于包含它的调用边界：必须先检查 has_*，不得把缺失 Timestamp 当 epoch 或缺失 ResourceRef 当默认资源；显式存在的 Unix epoch 合法。

TimePoint 使用已确定的 signed int64 epoch nanoseconds；Timestamp 规范 nanos 为0..999999999，负时间采用向下取整 seconds，精确双向转换。超出 Protobuf 年份或内部 int64 纳秒范围均拒绝，不截断、不饱和。ResourceKind 和封闭 Permission 均按名称逐项显式映射，尤其 Catalog 内部15/wire16，禁止数字cast；未知权限不降级。目标资源 ID 依据doc21第4.1节“标识与版本”要求小写UUIDv4（不是仅凭OpenAPI format:uuid推断），Action 保留128字节[A-Za-z0-9._-]，Task 保留256字节可见ASCII来源规则；空值拒绝。

稳定错误映射严格使用 target_spec.json.error_mapping。make_error 只构造稳定码、合法request_id/trace_id和显式retryable的Error成员，不构造受理事实。target owner unary唯一binary详情为序列化kiarabot.v1.RpcFailure：to_grpc_failure/from_grpc_failure经标准grpc-status-details-bin（Status.error_details）完整携带error和context。前文T01b的Error-only仅是历史/reference Connect格式，target unary拒绝该格式且无自动fallback。from_http_failure验证完整RpcFailure，from_http_error仅验证Error成员。解码要求稳定码和gRPC/HTTP状态一致；缺error/context、未知或UNSPECIFIED acceptance/recovery、显式current_version=0均拒绝。存在operation时须有runtime/scheduler/model登记owner、UUIDv4 operation_id和完整合法资源；Unknown允许无operation，不能为Login/Refresh虚构操作。当前operation查询只由Runtime/Scheduler/Model提供（target_spec完整方法集及doc21第2节），Identity登录/秘密收据不创建operation。这里校验的是结构；owner仍须结合命令和持久事实核验acceptance/recovery组合，不能因枚举合法就让Unknown换新键或按CorrectInput重做副作用。Error.details仍须空，retry_after/current_version/operation/acceptance/recovery完整保留，503/UNAVAILABLE歧义由Error.code消除。

此工具不验证认证、当前授权、资源存在性或owner/gate，不实现owner RPC，也不改变任何能力的 target 状态。CMake/传输接线由主集成负责，独立测试和检查见 reports/modules/architecture-p04-target-codec.md；doc18只读。

## P04 短期委托证明的重试边界

Identity IssueDelegation / ExchangeDelegation 的唯一线契约 requires_idempotency=false。
两者只检查当前来源凭据、会话、调用服务和精确权限，然后签发短期证明；不受理业务命令、
不扣减配额、不消费一次性凭据，也不产生 Action/Model 等业务副作用。重试必须重新授权，
可以返回新 proof ID/签名/期限；不承诺返回同一 JWT，不把 JWT 写入普通七日收据或伪装成
CredentialDelivery。证明仍最多 56 秒正文期限与 2 秒时钟容差，业务跳数最多两跳。
Login、RefreshSession、RotateCredential、RevokeSession 等既有幂等/恢复要求保持原契约。
未来若引入配额扣减或一次性消费，必须先建立独立的幂等命令契约及事务收据。

## P04 Identity RPC 边界（应用已接生产组合根，实现中）

`runtime/platform/identity_rpc.hpp/.cpp` 的 make_identity_rpc 借用 IdentitySecurityApplication 并持有组合根注入 PeerAuthenticator；适配 Login/RefreshSession/ValidateSession/GetPrincipal/RevokeSession/ListSessions，并通过可选 `IdentityRpcOptions{.principal=&app}` 追加 RegisterPrincipal/RotateCredential/RevokeCredential/GetPrincipalStatus/GetCredentialStatus 五个主体/凭据管理方法。RPC 层只做字段转换与稳定错误映射，业务与事务在 `IdentityPrincipalApplication`；未注入 principal 能力时五个方法返回 typed CAPABILITY_NOT_SUPPORTED，其余 Identity 方法继续稳定拒绝。应用已接生产组合根（Identity daemon）；P05f 已在 `main@439fb9e` 通过本机完整门禁与真实 PG16.4/TLS 测试，但 target 契约状态未翻转（target evidence 逐项登记，不把本机证据自动视为生产部署）；未翻项与证据见 [P04 报告](../reports/modules/architecture-p04-identity.md)。

每次从真实ServerContext.auth_context提取受限叶证据，再调用必须核对当前登记/凭据/SAN的可信认证回调，并核对回调的主体、证书指纹、平台角色和凭据版本。六方法要求Control peer；不相信body actor/peer、不把CA有效当业务授权。纯应用仍须复核当前Identity状态、self Principal列表和exact Session撤销权限。认证回调为同步借用契约，生产者须有自身有界I/O并遵守实际取消/deadline；边界在其前后检查预算，不声称能抢占任意阻塞回调。

请求context需小写UUIDv4 request_id/trace_id（doc21§4.1），畸形关联生成新CSPRNG错误ID；幂等键至多128字节封闭字符且写方法非空。取body期限、真实gRPC期限与5秒上限最早者；未给传输期限时默认2秒。该边界预算比通用owner descriptor上限更紧，不允许正文延长。密码/token/委托正文只借用SecretView到同步应用返回，不记录秘密/异常正文。回调/应用异常分别保留未调用或Unknown语义，CSPRNG或分配故障无法形成安全关联时仅返回脱敏传输INTERNAL，不伪造ID或业务结果。

成功必须是应用已提交结果，局部完整投影后才返回；checked_at/created_at/expiry/version来自应用，不补本机时间或token iat。缺持久binding、时间、版本或不合法token结果返回Internal，不造占位。Assignment投影调用Core project_assignment，固定角色显示权限调用default_role_allows；不跨assignment拼scope，角色行必须同role无explicit，explicit行各资源权限集合必须相同，无法无损表达则Internal。角色与权限仅显示并集，真正授权仍依赖逐条exact绑定。List输入拒绝expires_at/next_page_token等输出字段，opaque page_token可单独续页，显式snapshot_id须合法且与cursor绑定，当前权限/快照恢复由应用裁定。

Core Failure无损映射RpcFailure的acceptance/recovery/operation/retry_after，未持有的current_version不伪造。ReadCurrent→RESYNC_SNAPSHOT、Backoff→RETRY_SAME_ID、QuerySameIdentity→QUERY_SAME_INTENT、Diagnose→MANUAL_REVIEW；Unknown保留Unknown，不得携带换新身份/修正输入重做副作用建议。畸形应用失败或投影保守返回Internal/Unknown及原身份查询，决不假称NotAccepted。标准grpc-status-details-bin仅RpcFailure，无Error-only回退。

P04 会话验收使用 PostgreSQL、真实 Identity 和注册的 Control 客户端证书。Control HTTPS 验证登录、刷新、当前会话、会话列表和撤销。刷新后旧 access token 返回 401，撤销后 access 和 refresh token 均返回 401，撤销不存在的会话返回 404。独立的 mTLS 客户端验证 GetPrincipal 的当前主体、授予和授权版本，并检查无效 access token 返回 UNAUTHENTICATED。ListSessions 的快照按 (created_at, session_id) 升序，分页前固定顺序。

六方法RPC恢复补正：同键Login/Refresh恢复已提交的原TokenPair时，access已过期但refresh仍有效可原样返回（checked_at可能已超过access期限，Control剩余TTL clamp至0），不重新签发或延长任何期限。ListSessions接受仅opaque page_token的续页，不要求浏览器另传snapshot_id；输出字段仍禁止作为输入。

### P04 当前登记 PeerAuthenticator（Core持有权威编排）

make_identity_peer_authenticator(IdentityAdminApplication&, SecurityClockPort&)只借用Core Admin应用和受信时钟。wrapper从真实AuthContext提取指纹/DER证据，取2秒本地与真实gRPC deadline较早者，调用Admin.authenticate_peer({leaf_fingerprint, deadline}, der_chain)。当前DB UTC、登记/凭据/主体、释放UoW后成熟crypto验证及验证后fresh recheck全部由Core负责；外层不直接读取repos、不复制业务规则。

该回调仅认证当前平台peer，不授予业务权限；必须与纯应用使用同一Identity权威工厂。2秒本地预算和真实gRPC取消/deadline在各阶段检查，I/O自身有界由受审端口实现保证，不声称抢占任意阻塞端口。库中不构造PG连接、信任根、bootstrap或ready；组合根仍须注入37f7a62等已审完整typed工厂、成熟crypto及受信时钟并完成真实PG链验收。无wire/API字段变化。
wrapper仅核对返回指纹、正verified_at、受信clock/已知偏差及与verified_at的2秒窗口，再检查取消/期限。不构造数据库/信任根/bootstrap或ready。借用应用与clock须活到服务停止/Wait，Core应用与安全应用由组合根绑定同一完整Identity工厂。wire/API不变。

### P04 IssueDelegation RPC（第七方法，可选依赖）

make_identity_rpc(IdentitySecurityApplication&, PeerAuthenticator, IdentityDelegationApplication* issuer=nullptr, InvocationDeadline={})仍注册一个IdentityService，借用issuer到停止/Wait；两参旧调用可编译，缺issuer时Issue返回RpcFailure/CAPABILITY_NOT_SUPPORTED。Exchange与其他未接方法仍明确未实现，不降级到reference。Peer factory当前签名为make_identity_peer_authenticator(IdentityAdminApplication&, SecurityClockPort&)。

Issue只接受当前Control peer，subject_proof按原始AccessSession token借用，不剥Bearer或接收现成委托替代。audience显式映射封闭六服务；scope/permissions非空、去重、已知枚举与合法exact资源，client_certificate_sha256必须恰好32字节并等于真实transport peer。wire声明的permission×scope product只转换一次为exact tuples，由Core issuer逐tuple复核当前来源与权限，外层不授予或拼接其他assignment。

按父定稿1b962d3，Issue/Exchange requires_idempotency=false：签发不消费配额或一次性材料、无业务意图/普通七天秘密收据，重复请求重新授权并可产生新proof ID。Login/Refresh/Revoke/Rotate仍要求原幂等键。返回只复制应用实际签出的proof及原audience/expiry/一跳信息，核对claim与请求tuple、真实证书及来源上下文一致，不补签名/期限/来源；crypto和权威查询均由Core持有。真实JWT生产签名/PG/HTTPS组合根闭环由父继续验收，本RPC fixture不据此标target implemented。

Issue RPC在解码/展开前同步成熟JWT的128 exact bindings上限：资源1..128、权限1..40且封闭去重，permission_count <=128/resource_count，以除法规避溢出。超限沿既有INVALID_ARGUMENT返回，不调用Core issuer的来源验证/签名；不先构建大型product再等crypto晚拒。Core自身的同一上限由Core能力维护，传输层不改其实现。

### Identity 自身 TLS 调用期限

当前factory为make_identity_rpc(SecurityApp&, PeerAuthenticator, Issuer*=nullptr, InvocationDeadline={})，其中InvocationDeadline是function<Outcome<TimePoint>()>，闭包由服务持有。生产composition必须注入实际选定自身TLS链有效期与当前可信clock/lifecycle gate；空回调仅为fixture/源兼容，不得作为生产默认安全策略，也不得把自身到期时间写入远端Control的CertificateBinding。

run保留body/gRPC/default2秒/max5秒及取消检查，可信回调失败沿原Failure进入完整RpcFailure，异常在调用应用前转Internal/NotAccepted。返回非正时间失败关闭为UpstreamUnavailable；过期期限为Timeout/NotAccepted。有效期限取已有预算与自身limit最早者，回调之后与认证之后再次检查取消/到期；传入每个Core用例的context.deadline包含此上限，由Core final_check阻止自身TLS到期后的提交。外层不授权、不查仓储、不实现TLS链/clock判断或伪造有效期。较晚limit不能延长请求期限。

### IssueDelegation platform capability and represented permissions

An authenticated Control service must currently hold an explicit
identity.delegation.issue binding at its own stable Principal resource
(ResourceKind::Principal, id=the actual registered platform subject). Certificate
validity/registration and default roles do not grant this capability; a grant at a
represented human Principal, Catalog or other service Principal cannot substitute.
This service capability is checked independently of every represented subject's
exact requested business permission/resource. It therefore supports dynamic Session
resources without turning the service capability into blanket business authority.
Offline enrollment must explicitly provide the own-Principal grant; no CLI or daemon
automatically creates it.

Issue reads the current Control registration, credential, Principal, authorization
watermark and exact grants before signing and again after signing. Source access
and business grants are also revalidated. Each short authority transaction uses the
existing current-time/final-check gate with at most one second authorization window
and the request/certificate/credential deadlines. Crypto/random/session-application
calls hold no outer UoW. Missing/wrong capability returns PermissionDenied; revoked,
expired or mismatched current peer returns Unauthenticated; changed authorization
watermark returns Conflict/ReadCurrent; dependency and deadline failures propagate.
A signed intermediate proof is discarded on any final check failure and never returned.

### P04 bootstrap当前材料定位端口

Identity内部by_bootstrap仅接可信key引用、WorkloadBootstrap用途和固定32字节tag，
不接raw证明或body身份。只定位当前credential版本，旧历史材料不能通过当前查询认证；
save原子维护唯一current locator，其他材料无该locator。空optional仅未匹配；错误shape
InvalidArgument，SQL/损坏/歧义为Failure，不LIMIT1任选身份。定位不是认证或业务授权，
Core继续检查当前主体、状态、期限、绑定和audience；crypto在UoW外。无wire/API字段变化。

### GetVerificationKeys RPC（借用Core公共验证材料用例）

factory保留原issuer与InvocationDeadline位置，末尾新增IdentityRpcOptions={.verification_keys=&app}第五参；app借用至server停止/Wait，缺省typed Unsupported。当前mTLS认证及自身TLS InvocationDeadline在查询前照常执行。GetKeys不读/验证context.delegation或JWT，不对unknown kid先做JWT预检，避免获取验证材料的自举循环；Core151f31e负责当前平台身份、显式IdentityKeysRead@可信Catalog与授权后future-version拒绝。

known_key_version显式0无效；正值原样交Core，none/same/old均返回完整current set，不做空delta或成功cache。只映射Core的public-only DTO：key_id、raw32 Ed25519公钥、正uint64键/集合版本与精确纳秒有限valid_until；空集合/零版本/不合法Catalog/零checked_at/过期或无限期限等畸形成功结果Internal。实际依赖/材料过期失败保持完整RpcFailure。外层不访问private/config路径，不加载材料、不重建keyring、不使用token选择远程key源。当前Core契约仅一个active key，生产PG及成熟材料端口闭环由组合根验收。

### P04 CheckAuthorization 当前实现边界

纯Core应用仅支持Runtime/Host/Scheduler/Model检查自身方法和签名audience，调用者需
显式IdentityAuthorizationCheck@自身Principal。外层从唯一method metadata提供受信
owner/permission/scope规则，不能从请求注入权限。Control管理员跨consumer查询尚不支持。
查询者自身缺能力为RpcFailure/PERMISSION_DENIED；bad proof/audience及来源或actor当前
状态失效为RpcFailure/UNAUTHENTICATED；业务proof范围或source当前grant不足为正常
AuthorizationDecision.allowed=false并带PERMISSION_DENIED。依赖故障、时钟故障和TIMEOUT
保留失败，不伪装业务拒绝。known版本为零/future拒绝INVALID_ARGUMENT，旧值仍查当前。
返回当前source授权版本及DB checked_at；专用验签不证明原业务TLS possession，owner必须
先用真实来客普通verify。详见doc/02-core的纯应用章节；RPC与生产接线仍需独立验收，
此变更不新增proto字段、稳定错误枚举、API或映射。

### Current original Control of browser sessions

Every browser session retains its original Control subject and credential ID. Access,
refresh and secret replay require the actual current Control to match both stable IDs;
certificate rotation under that same ID is allowed after current TLS/registration
verification. Another Control cannot take over or rebind old tokens. Missing binding
fails closed. Current head, active PlatformService/Control, own Principal binding,
registered SAN, credential/certificate validity and dependency failures are checked
again at session authority reads; no VerifiedPeer is fabricated from stored records.

For delegated source_session, the persisted original Control subject must match
claims.actor_chain.front(). That original Control is checked from typed current
registration independently of the actual direct Runtime's own authenticated TLS.
Relevant final deadlines include the original Control current credential/certificate
expiry and the existing one-second authorization checkpoint. Crypto runs outside UoW;
fresh reads after verification/replay reject intervening invalidation or dependency
failure. Source binding does not pin a certificate version or overwrite direct peer data.

CheckAuthorization的actor当前有效性包含每个Control/Runtime当前显式
identity.delegation.issue@其自身Principal；撤去、错Principal或role-only均为
RpcFailure/UNAUTHENTICATED，不作为source业务PERMISSION_DENIED decision。


### P04 ExchangeDelegation RPC 适配

`IdentityRpcOptions{.exchange=&app}` 可选借用 Core `IdentityExchangeApplication`，生命周期至
server 停止/Wait；原 issuer、InvocationDeadline 及 verification_keys 参数位置不变。
未注入时返回 typed CapabilityNotSupported。只接受当前认证的 Runtime 平台 mTLS，
子受众仅 model/scheduler；body 的32字节 client_certificate_sha256 必须等于实际 Runtime
叶指纹。parent_proof 原样借用，不剥离 Bearer，不在外层解析 JWT，不把 Runtime 指纹
当成父 proof 的上一跳 Control 指纹。父签验、两平台当前显式能力、来源会话/凭据/业务
权限及签后重查均由 Core 负责。

封闭 permission/resource 去重并在解码、笛卡尔积展开前限制最多128个 exact tuples；
父 proof 非空、可见 ASCII 且最多32768字节。无普通写入幂等键要求、无收据或库写入。
继承当前 mTLS、请求/transport/自身 TLS InvocationDeadline 的最小期限与完整 RpcFailure
映射。成功只投影 Core 返回的原始 proof、audience、expiry、hop2，校验当前 Runtime
指纹、actor 链和请求精确集合；异常成功结果失败关闭，不发送部分 proof。

真实 mTLS fixture 覆盖该适配与实际 Core 编排；测试的仓储、随机和签验仍为明确替身，
不代表生产 PostgreSQL、JWT 材料端口或组合根已接线可用。

P09f（2026-11-02，HEAD d508d67）：上述 P09 target 状态经真 PG16.4 fixture `p09_postgres_storage_records`、application `p09a_runtime_storage_application` 与真 TLS/PG `p06_bridge_acceptance` storage 帧矩阵复核，5/5 P09 相关 CTest 通过。未翻转：`NamespaceScope` message（仅两个已验证字段翻转，message 仍含 internal-only `installation_shared_id`）、`NamespaceScope.installation_shared_id`（插件自报被拒绝，仅内部/管理路径）、unary `Runtime*Storage*`（internal_dispatcher，不对外注册；插件使用 `HostRequest.storage_*`）、`operations.createSharedNamespace/getSharedNamespace`（Control REST 未接线）。证据/缺口详见 `reports/modules/architecture-p09-storage.md` P09f 节。注意：main@d508d67 上 `target_api_test.rb`（listAuditEntries/listLogs/beginDeleteResource/restoreResource 带 owner-RPC 却标 implemented）与 `evidence_scopes_test.py`（冻结集缺上述 4 项）已失败，属 P12e/P12c-7/P13c 登记漂移，P09f 未扩改。
## P06f adapter 事件映射与 fan-out 语义补充（2026-11-02）

OneBot adapter 的 `EventEnvelope.source.account_id` 始终是 Runtime account UUID；平台 `self_id`（OneBot bot id，可能是数字 QQ 号）不是账号资源 id，只能用于平台 bot 查找与来源校验。adapter mapper 以独立的 `platform_self_id`（部署 `ONEBOT_SELF_ID`，缺省 account id）校验入站帧，不得把平台 self id 与 account UUID 比较。事件进入 Runtime 后，fan-out 仍按已连接 plugin 的 manifest capability 订阅（`event.message.received` 等）生成 `delivery_targets`；未连接/未订阅插件不产生 target，重复事件按 event id/content 去重。P07d e2e 的 fake OneBot 消息帧必须使用 OneBot v11 整数 `message_id`，否则 NoneBot 退化为通用 Event、matcher 不执行。

P07g（2026-11-02，HEAD 95495c2）：Action target 已翻项经 `p07_action_rules`、`p07_postgres_action_records`（真 PG16.4）、`p06_bridge_acceptance` P07 段（真 TLS/PG/Runtime）、`p07_adapter_e2e`、SDK `test_actions.py` 28 passed、`p12_control_owner_gateway` 复核；5/5 P07 相关 CTest 通过。未翻转：`Get/UpdateActionPolicy`（应用实现，待 P07f-1 wire 证据，P07f-2 翻）、react/delete/mute/kick/approve payload targets（待 P07f-1/2）、plugin 权限过滤独立项、`ListActionMetadata/InspectActionMetadata`（未接线）；理由见 `reports/modules/architecture-p07-action.md` P07g 节。真实 QQ/NapCat、真实 reaction API、平台延迟/EXPIRED 故障注入和跨主机仍标未验收。
## P11f Model 证据核对（2026-11-02）

P11f 实测 p11 套件 9/9、p03 真 PG 2/2、SDK pytest 141 passed/23 skipped、`check_implementation_status.py` PASS；HEAD `95495c2`。ModelService 11 个 owner RPC 为 target implemented；ModelChat 消息/HostRequest.model_chat 由 P11e bridge fixture 证据支持。未翻/延期项：ModelChatDisposition/ModelCallDisposition 无线上消费暴露证据（仅 owner 内部/管理投影，不据此翻客户端能力）；streaming 明确不做（HTTP provider `stream=false`，无流式字段）；GetOperation 仅 model owner 分支，Runtime/Scheduler owner 未接线；插件→Runtime→Model→Provider 单链路需 P05b 委托语义确认。当前 OpenAPI Model 9 个 REST operation 仍 inline `not_implemented`，与 p11_control_model_rest e2e 不一致，属契约清理复核项，本任务不改契约。另 `p01_target_roundtrip` 因 ListAudit `scope_kinds`（P12c-6）镜像不一致失败，与 P11 无关，已上报 Lead。

## P00p 注释/契约状态复核（2026-11-03，只读）

P00p 只核对注释与本文 target 状态，不翻转任何契约。已实现但仍写“未实现”的注释已修正：
`host_bridge_server.hpp` 的 deployment helper 注释（P08b-3/3a/3b persistence 已接线；PG
`HostRegistrationPort` 已存在但组合根仍用 in-memory 注册 store）、`runtime_rpc.hpp` 的
`QueryActionResult`/P06 internal-dispatcher 注释、`sdk/python/README.md` 的 P06a/P08 文案。
下列仍按本文 target 表 / `proto/tests/target_spec.json` 保持 **not_implemented**，相关注释保留并
注明依据，不得因注释清理推断为已实现：

- `operations.listActionMetadata` 及 `RuntimeListActionMetadata*`：target 表未翻，Runtime/Control
  无生产 handler；P07g/P12f 记录未接线。
- `HostService` 7 个方法及 `host_operations`：P08 内部 HostBridge 面，target 仍 not_implemented；
  P13f 记录 host operation owner 待用户决策。
- `RuntimeBindAdapterResponse.bootstrap_credential` / `RuntimeCreateInstanceResponse.bootstrap_credential`
  字段级：target 状态未翻（P06 消费路径有实现/测试，字段级翻转仍缺单独证据）。
- Control WebSocket channels：AsyncAPI/P12a-2 延期，target channels 全 not_implemented；REST
  operation 快照是当前唯一 resync 源。

证据与缺口以对应模块报告为准；本注记不改变任何契约状态。

### P05b-2/B1 delegation issuance compatibility

`IdentityIssueDelegationRequest.instance_id` is a backward-compatible optional field. Empty keeps
the legacy administrator-subject issuance exactly. Non-empty is accepted only for audience
`runtime`, only from an authorized Control administrator session, and resolves the subject from
the unique plugin principal bound to `RESOURCE_INSTANCE/{instance_id}`; no caller-supplied subject
principal id exists. Permission/resource tuples are whitelisted and must also be covered by the
administrator's exact assignments plus `resource.manage` on the instance. Claims keep
`business_hops=1`, source credential/session of the administrator, subject of the instance, and
`expires_at-issued_at<=56s`. Exchange keeps its existing hops2 narrowing and owner verification.

## P05 approved catalog authorization (2026-11-04)

CreateAccount.catalog=5, ValidateManifest.catalog=3 and CreateInstallation.catalog=4 are required RESOURCE_CATALOG references to the offline registered accounts/installations family UUID. CreateInstance authorizes its exact parent installation. ListAccounts/ListInstallations/ListInstances/ListAdapters/ListHosts authorize the registered family catalog and independently each exact child. Their scope_kinds contains CATALOG plus their single child kind, without inherited grants. Missing or malformed catalog is INVALID_ARGUMENT, wrong family or missing grant is PERMISSION_DENIED. Absent deployment catalog configuration keeps these capabilities unavailable. Method implementation status remains separately evidence-gated. Lists retain fixed ten-minute immutable snapshots, current authorization per page, and conflict on scope/version/subject changes or unknown cursors.
