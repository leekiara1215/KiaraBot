# 统一协议层设计

T03a 新增 CoreToPlugin.error（oneof 字段 6），承载注册之后的终止错误，不复用旧编号。注册拒绝仍使用 RegisterPluginResponse.error；注册前身份错误仍为 gRPC UNAUTHENTICATED/PERMISSION_DENIED。注册后的非法 ACK、权限/状态拒绝、依赖故障使用终止 Error 帧，随后结束流；断线/取消可能使该帧不可达，此时客户端保持未确认结果未知并重新注册。完整注册、订阅和 ACK 约束见 doc/03-plugin-sdk.md 的 T03a 子集。

## 责任

统一协议层负责定义所有语言和进程都能理解的事件、动作、错误、能力和版本信息。

它是 Python 适配器、C++ Core、C++ 插件和 TypeScript 面板之间唯一的共享语言。

## 契约实现状态（v1）

`proto/tests/contract_status.json` 是"契约声明 → 实现状态"的机器可读登记表，`python3 proto/tests/check_implementation_status.py` 会校验：每个 Proto 字段/oneof 载荷、枚举值、AsyncAPI 频道/事件、OpenAPI 操作都必须登记为 `implemented`、`not_implemented` 或 `deprecated`；未实现项必须在 `.proto` 内有 `CONTRACT_STATUS` 注释、在 `api/*.yaml` 的 `x-contract-status` 中列出，并在下表出现。**未列为本表可用的契约项不得在文档、面板或客户端中被当作已可用能力。**

<!-- contract-status:start -->
| 契约项 | 状态 | 说明 |
| --- | --- | --- |
| `EventEnvelope.message_sent`、`EventEnvelope.group_member_changed`、`EventEnvelope.adapter_status_changed`、`EventEnvelope.plugin_status_changed` | not_implemented | 唯一适配器只产生 `MessageReceived` 与 `FriendRequest`；Core 对其余事件只有接收/订阅映射。插件订阅这些事件收不到数据。 |
| `ActionEnvelope.schedule_task`、`ScheduleTask.task_id`、`ScheduleTask.schedule`、`ScheduleTask.payload` | deprecated | 旧 Action 调度与 `host.tasks.v1` 重复且语义不等价（见 `doc/13`），v1 继续以 `CAPABILITY_NOT_SUPPORTED` 拒绝；调度只走 `host.tasks.v1`。 |
| `SendMessage.reply`、`SendMessage.reply_to_message_id` | not_implemented | Core 与适配器都显式拒绝；v1 的 Action 面包含 `send_message`、`react_message`、`delete_message`、`mute_member`、`kick_member` 与 `approve_request`。 |
| `MessageSegment.Type.AUDIO`、`MessageSegment.Type.VIDEO`、`MessageSegment.Type.FILE`、`MessageSegment.Type.LOCATION`、`ConversationRef.Type.CHANNEL`、`PluginState.PLUGIN_STATE_STARTING`、`PluginState.PLUGIN_STATE_STOPPING` | not_implemented | 适配器把非 text/image/at/reply 段降级为 `UNKNOWN`；CHANNEL 会话被拒绝；运行时状态机不产生 STARTING/STOPPING。 |
| `Error.details`、`Account.display_name` | not_implemented | 无生产/消费点；REST `Error.details` 保持空对象。 |
| `PageRequest.page_size`、`PageRequest.page_token`、`PageInfo.next_page_token`、`PageInfo.page_size`、`Health.state`、`Health.component`、`Health.version`、`Health.checked_at`、`Health.dependencies` | not_implemented | REST 使用 OpenAPI 自己的分页/健康 schema，v1 无代码引用这些 Protobuf 消息。 |
| `PluginManifest.dependencies`、`PluginManifest.config_schema` | not_implemented | 非空值被 `CAPABILITY_NOT_SUPPORTED` 拒绝；`config_schema` 只接受空串或 `{}`。 |
| `CoreToAdapter.shutdown`、`CoreToPlugin.shutdown`、`ShutdownNotice.reason`、`ShutdownNotice.drain_timeout_ms`、`semantic.transport.graceful_shutdown` | not_implemented | 帧已定义但 v1 Core 不发送；关闭流仍按“结果未知”处理，见“优雅停机”。 |
| `semantic.stream.plugin_updated_health_state` | not_implemented | `plugin.updated` 只推送 `state`，不推送 `health_state`。 |
| `events.health.updated` | not_implemented | health 投影固定 `HEALTHY` 单组件，事件永不产生；`/ws/health` 只会停在 SESSION_READY。 |
| `operations.getActionByIdempotencyKey`、`operations.getActionResult`、`operations.listGroups` | not_implemented | REST Action 查询没有写入方（无 `control/<user>` owner），恒 404；`/groups` 无 Groups 分支，恒空页。 |
| `semantic.action.deadline_max_300s`、`semantic.action.target_source_id_charset` | not_implemented | deadline 只校验正时间戳，无 300 秒上限；Action 的 `target_source` 用比入口更窄的字符集。 |
| `semantic.adapter.backpressure_retryable_rate_limited` | not_implemented | 契约要求队列满返回可重试 `RATE_LIMITED`，实现返回 `retryable=false` 且不关流。 |
| `semantic.host.unknown_operation_error` | not_implemented | 契约要求未知 Host operation 返回 `CAPABILITY_NOT_SUPPORTED`，实现先返回 `INVALID_ARGUMENT`。 |
| `semantic.rest.action_query_terminal_retention`、`semantic.rest.action_query_deadline_expired` | not_implemented | REST 查询未实现 168 小时终态保留与到期 `EXPIRED` 迁移；语义已冻结，待 control-api 接线。 |
| `semantic.grpc.business_error_mapping` | deprecated | 旧文档的 `CONFLICT→ALREADY_EXISTS/ABORTED` 等业务错误映射已废止；业务错误以 in-band `Error` 帧为准，见“错误模型”。 |
| `semantic.rest.admin_role_wildcard` | deprecated | 旧 `admin: ['*']` 通配授权已废止；角色改为显式权限列表，任何默认角色都不含 `message.read`。 |
| `AdapterStatusChanged` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `GroupMemberChanged` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `MessageSent` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `PluginStatusChanged` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AdapterStatusChanged.adapter_id` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AdapterStatusChanged.reason` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
| `AdapterStatusChanged.state` | not_implemented | P01 目标声明；无生产实现，见模块报告。 |
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

上表是事件词表，不是实现承诺：v1 端到端闭环 `MessageReceived`、`TimerTriggered` 与 `FriendRequest`；其余四种见“契约实现状态”中的 `not_implemented` 行。客户端/插件不得订阅尚未实现的事件并假定能收到数据；Core 保留订阅映射只为后续接线。

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

v1 Core 关闭 `AdapterBridge` / `PluginBridge` 时直接取消或结束流，不发送 `ShutdownNotice`（`CoreToAdapter.shutdown=5`、`CoreToPlugin.shutdown=8` 已定义，状态为 not_implemented）。因此：

- 客户端收到流关闭（无论 gRPC 状态）都视为**未知结果**，不得推断事件未入库或 Action 未执行；
- 重连后必须重新注册，并按 `event_id` 幂等重投未确认事件、按认证调用方 + 幂等键查询未确认 Action；
- 将来 Core 发送 `ShutdownNotice` 时，`reason` 给出原因，`drain_timeout_ms` 给出在途处理上限：客户端应停止领取新工作、在上限内完成并确认在途请求，然后正常关闭。未收到该帧时的行为与 v1 完全相同；旧客户端必须忽略未知 oneof 成员。

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

启用持久化接收的 AdapterBridge 首帧必须是 RegisterAdapterRequest。协议版本固定 kiarabot.v1；账号至少一个，必须 enabled 且在宿主配置的允许列表中；自报账号及 Capability 不授予权限。注册成功返回 accepted、随机 assigned_instance_id、required_protocol_version 和 30 秒心跳间隔；注册失败返回 accepted=false 和稳定 Error，然后正常结束流。新注册替换同身份旧实例，旧连接不能继续入库。事件和心跳必须绑定当前实例。身份冒用仍以 gRPC PERMISSION_DENIED 终止。

CoreToAdapter.event_ack 使用新编号 4。AdapterEventAck 包含 event_id、trace_id、status、error、retry_after_ms：PERSISTED 表示 inbox/会话水位/审计和幂等记录已原子提交，DUPLICATE 表示同内容已提交，REJECTED 表示本次未接收。只有前两者是确认；不是插件 ACK 或 Action 完成。

内容或顺序冲突使用 CONFLICT，错误 schema/缺失字段使用 INVALID_ARGUMENT，无账号授权或伪造 Core 内部事件使用 PERMISSION_DENIED，队列满 RATE_LIMITED，存储不可用 UPSTREAM_UNAVAILABLE，损坏/内部失败 INTERNAL。RATE_LIMITED、存储暂时故障及版本提交冲突可重试，retry_after_ms=1000；内容冲突不可重试。传输超时/取消依旧使用 gRPC 状态，确认丢失不能推断未入库。未装配 ActionRouter 时 ActionResult 明确拒绝 CAPABILITY_NOT_SUPPORTED / gRPC INTERNAL；T04b 装配后按已认证适配器归属验证并持久化结果。未配置持久化入口时保留 T01b 拒绝语义。

## 配置下发闭环

配置通过 plugin.config.v1 / PluginConfig / PluginConfigAck 下发并按连接确认。REST GET 的 delivery 区分持久化与运行实例应用，PUT/幂等重放只确认持久化。状态、超时、重连、错误及事件门控的完整规则见 [PluginSDK 配置下发闭环](03-plugin-sdk.md#配置下发闭环t08b-后续)。协议唯一来源为 proto/kiarabot/v1/plugins.proto，REST 为 api/openapi.yaml；无新增错误码或 HTTP/gRPC 映射。

## P01 目标公共类型（尚未实现）

`platform.proto` 定义资源类型、稳定实例 Fence、RequestContext、Operation/OperationRef、FailureContext、HandoffContext、StreamCursor 与 DeliveryAttempt。所有新增项均为 not_implemented。内部层必须映射为独立值类型，不能依赖生成消息。目标 ID 使用小写 UUIDv4，原 event/action/task ID 沿来源规则；版本、执行代次、连接代次必须正值，presence 缺失或 0 均 INVALID_ARGUMENT。正文自报身份不可信；认证绑定由传输验证。

操作采用 OPERATION_ACCEPTED/RUNNING/BLOCKED/SUCCEEDED/FAILED/CANCELLED/EXPIRED；BLOCKED 非终态且带 reason/next_attempt_at；stalled 仅观测标记。执行结果采用 EXECUTION_NOT_STARTED/KNOWN_SUCCEEDED/KNOWN_FAILED/UNKNOWN，与操作状态独立。目标 gRPC 映射以 target_spec.json 为准：CONFLICT→ABORTED，CAPABILITY_NOT_SUPPORTED→UNIMPLEMENTED；流错误帧与 unary status details 都保留稳定码及关联 ID。Unary RpcFailure 包含受理事实、恢复建议和操作引用；无法收到响应仍按原意图查询。
