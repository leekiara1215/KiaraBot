# Model 模型网关模块设计

本文描述 `kiarabot-model` 进程：模型配置与加密 Provider secret 的所有者、模型调用事实（两次事务、UNKNOWN、计费保守事实）的权威、HTTP Provider 执行方与 ChatStream 服务端流的提供者。跨进程契约唯一来源是 `proto/kiarabot/v1/model.proto` 与 `proto/kiarabot/v1/model_stream.proto`；应用/领域实现在 `core/src/application/target/model_gateway_application.cpp`、`core/src/application/target/model_gateway_application_stream.cpp` 与 `core/include/kiarabot/core/domain/target/model_gateway.hpp`、`core/include/kiarabot/core/domain/target/model_stream.hpp`；Provider 在 `infrastructure/src/model_http_provider.cpp`、`infrastructure/src/model_http_stream_provider.cpp` 与 `infrastructure/src/model_sse_parser.cpp`。模块在整体架构中的位置见[架构总览](../architecture.md)（§3、§5、§9）。

## 1 模块定位与职责

Model 是模型调用的**领域所有者**，负责：

- `ModelConfiguration`/`ModelMetadata` 管理：provider kind、endpoint、provider model、超时、并发上限、生命周期与版本；metadata 与可编辑配置读取分离。
- Provider secret 的密封存储与轮换（XChaCha20-Poly1305，根密钥部署注入）；线上只暴露 `secret_configured`/`secret_version`。
- 非流式 `Chat` 的两次事务、`logical_call_id` 防重与 replay 处置、UNKNOWN 的处置与保留。
- `ChatStream` 服务端流：admission、增量文本投递、终帧事实、取消保护与撤销权限轮询。
- 调用记录（usage/`may_have_billed`/provider request id/latency/disposition）与管理 operation 的持久化。
- 维护扫描：恢复超期 `Started` 为 UNKNOWN、处置超窗 UNKNOWN、清理过保留期终态。

Model **不负责**：插件身份与会话（归 Runtime/Identity）、插件准入与 feature 协商（归 Runtime `HostRequest` ingress）、外部计费对账、工具/多模态/非文本结果、prompt/正文归档、跨服务事务。流式文本、sequence 与 partial 都是**临时投递事实**，不持久化、不重放；durable 事实只有调用记录与最终的 `Started/Succeeded/Failed/Unknown` 状态。

## 2 边界与依赖

- 上游调用者：Runtime（`HostRequest.model_chat`/`model_stream_start` 经 PluginBridge 与 `RuntimeModelClient`）与 Control（管理面 REST 经 owner gateway）。Model 不接受插件直连。
- 身份依赖：`ModelIdentityClient`（`core/src/runtime/platform/model_identity_client.cpp`）用实际入站 mTLS 叶证书绑定 delegation proof，再对每个操作调用 Identity `CheckAuthorization`；未配置 `identity_endpoint` 时 authorization 端口 fail-closed，RPC 一律拒绝。
- 存储依赖：Model 私有 PostgreSQL 逻辑库，经 typed UoW 访问 `model_configs`、`encrypted_provider_secrets`、`model_intents` 以及共享 receipts/audit/operations；schema 版本以 `infrastructure/postgres/schema/v1.hpp` 为准（当前 v11），细节见[基础设施模块](infrastructure.md)。
- 网络依赖：libcurl 实现的已批准 HTTP Provider；流式只使用标准 SSE 拆分；不引入厂商 SDK。
- 部署依赖：`model_security.root_key_file`（32 字节 hex、0600、可信文件）与 `key_version` 必填，根密钥不入库、不与密文同表；TLS 材料复用服务配置。
- 不与其他服务共享表或进程内对象；跨服务只用 `proto/` typed 引用。

## 3 进程/组件结构

```text
kiarabot-model（core/app/model_main.cpp → core/app/model_composition.cpp）
  ├─ ModelService unary 适配器  core/src/runtime/platform/model_rpc.cpp
  ├─ ModelService.ChatStream    core/src/runtime/platform/model_stream_rpc.cpp
  │    └─ 编解码/严格解码器       core/src/runtime/platform/model_stream_codec.cpp
  ├─ 应用层 ModelGatewayApplication（含 streaming 扩展）
  ├─ 身份适配器 ModelIdentityClient（mTLS + delegation 结构校验 + 在线授权）
  ├─ Provider: model_http_provider / model_http_stream_provider + model_sse_parser
  └─ 私有 PG 库 + typed ModelUnitOfWork（模型/密钥/调用仓储）
```

`make_model_gateway_application` 装配的依赖是纯端口：`ModelProviderPort`、`ModelSecretCryptoPort`、`ModelDigestPort`、`ModelIdPort`、`ModelAuthorizationPort`、`ModelUnitOfWorkFactory`；应用层不依赖 protobuf、HTTP、SQL 或日志。流式与非流式共用同一 admission/settle 代码路径（`admit_chat`/`settle_chat`），只多一条 `ChatStream` 投递层。

## 4 数据模型与持久化

- `model_configs`：`model_id`（PK）、唯一 `alias`、`provider_kind`（仅 `openai_chat_completions`）、`endpoint`、`provider_model`、`timeout_ms`、`max_concurrency`、`lifecycle_state`、配置版本与时间；版本即 CAS 水位。
- `encrypted_provider_secrets`：`(model_id, secret_version)` 主键，保存 `root_key_version`、AEAD 算法、24 字节 nonce 与密文；AAD 绑定 `model_id|secret_version|root_key_version|aead`。轮换只影响后续调用，在途调用固定原版本，旧版本保留到在途结束。
- `model_intents`（调用记录）：`owner+logical_call_id`、`model_id`、config/secret 版本、服务端 `request_hash`、`state`（Started/Succeeded/Failed/Unknown）、`execution_outcome`、`usage_known/input_tokens/output_tokens`、`may_have_billed`、provider request id、latency、error、`disposition`、resolved 信息与 `retain_until`。
- 保留：终态按 7 天保留期清理（`kModelCallRetention`）；UNKNOWN 未决不清理，30 天后由维护扫描规则性置 `ABANDONED`（`kModelUnknownResolutionWindow`）并作为终态继续受保留期约束；超过 35 秒仍为 `Started` 的行同样由维护扫描恢复为 UNKNOWN。
- 流式文本不落库：`ModelStreamEvent` 的 delta/sequence/partial 只在本次 attempt 内有效；终帧快照由 `ModelStreamCallSnapshot` 投影既有调用事实，不新增持久状态。
- 上限常量集中在 `model_gateway.hpp`/`model_stream.hpp`：消息 ≤128 条、请求/结果 ≤64KiB、Provider 响应 ≤1MiB、secret ≤4096 字节、超时 1000–30000ms、并发 1–64、delta ≤4KiB、累计文本 ≤64KiB、SSE 单行/事件 ≤64KiB、总 SSE ≤1MiB。

## 5 关键流程/状态机

**非流式 Chat（两次事务）**

1. 在线授权 `model.chat`（精确 `RESOURCE_MODEL` scope）后进入 txn1：按 `owner+logical_call_id` 查已有调用；同 id 异 `request_hash` 返回 CONFLICT，同 hash 返回 replay 处置（不调 Provider）；校验模型 Active、当前 secret 存在、per-model 并发 admission。
2. txn1 写 `Started` 意图并提交：记录 config/secret 版本、`request_hash`、`version=1`、`created_at`，并**保守**写 `may_have_billed=true`。提交结果未知时按 UNKNOWN 返回，且不得据此重跑 Provider。
3. Provider 调用发生在所有 UoW 之外；secret 只在内存中解封并使用，调用后立即擦除。
4. txn2 用新 UoW 以 `version` CAS 将 `Started` 更新为终态：已知成功写 `Succeeded`、明确 4xx/429 写 `Failed`；5xx/TLS/超时/畸形/超大响应写 `Unknown + execution_outcome=UNKNOWN + may_have_billed=true`，绝不写成“未调用/未计费”。commit=Unknown 不回滚 txn1。
5. 维护扫描把超过 35 秒仍为 `Started` 的行恢复为 UNKNOWN（error=TIMEOUT）；UNKNOWN 经 `ResolveModelCall`（`NotExecuted/ExecutedNotBilled/ExecutedBilled/Abandoned`，expected_version CAS + 幂等键）转 `Failed` 终态并保留处置与计费事实；30 天未处置按 `ABANDONED` 规则收尾。

**ChatStream 状态机**

- 事件序严格为 `started(sequence=0) → delta(1..N) → terminal(N+1)`；replay 为 `started(REPLAY_*)+terminal(REPLAY)`、零 delta、不调 Provider。
- 授权在 admission、首输出前、最长每 1 秒、终帧前复查；检查期间停止输出；执行期限同时被请求 deadline、模型配置超时与 delegation 到期裁剪。
- 终帧 `COMPLETED` 必须同时满足：Provider finish 为 STOP、收到 `[DONE]`、txn2 已确认提交；仅 HTTP EOF、grpc OK 或取消 receipt 都不足以完成。`TRUNCATED`（length/content_filter）保留已发文本并 `partial=true`；失败/取消区分稳定码，但都可能 `may_have_billed=true`。
- 背压：Provider 队列 32 帧/64KiB、入队预算 250ms、单帧写预算 1 秒；每连接最多 4 个流、每进程 64 个。Provider worker 由 `std::jthread` 持有并 join，取消通过 `stop_token` 传播，无分离 watchdog。

## 6 接口与契约

`ModelService`（`proto/kiarabot/v1/model.proto`）按精确 `RESOURCE_MODEL` scope 暴露；全部 mTLS + 在线授权：

| RPC | 权限 | 默认/最大期限 | 幂等 | 说明 |
| --- | --- | --- | --- | --- |
| `Chat` | `model.chat` | 30s/30s | 否 | 非流式调用；重复 logical id 只 replay 元数据 |
| `ChatStream` | `model.chat` | 30s/30s | 否 | 服务端流；首帧前结构化 `RpcFailure` trailer |
| `GetCallStatus` | `model.call.inspect` | 2s/5s | 否 | 查询 durable 调用事实 |
| `GetModelMetadata`/`ListModelMetadata` | `model.read` | 2s/5s | 否 | 脱敏 metadata；list 必须是显式 scope 集 |
| `UpdateModel`/`DisableModel`/`EnableModel` | `model.manage` | 5s/30s | 是 | 版本 CAS；管理 operation |
| `RotateProviderSecret` | `model.manage` | 5s/30s | 是 | 只写不读；返回新 secret 版本 |
| `GetModelConfiguration` | `model.manage` | 2s/5s | 否 | 只返回 `secret_configured`/`secret_version`，永不回显密钥 |
| `ResolveModelCall` | `model.manage` | 5s/30s | 是 | UNKNOWN 终态处置，expected_version CAS |
| `GetOperation` | `resource.read` | 2s/5s | 否 | 仅 model owner |
| `ListOperationChanges` | `resource.read` | 500ms/500ms | 否 | owner 观察日志，600 秒窗口，cursor gap 返回 CONFLICT |

流式契约（`model_stream.proto`）：`ModelStreamDisposition`（FRESH/REPLAY_IN_PROGRESS/REPLAY_TERMINAL）、`ModelStreamTermination`（COMPLETED/TRUNCATED/FAILED/CANCELLED/REPLAY）、`ModelStreamFinishReason`（STOP/LENGTH/CONTENT_FILTER）；`ModelStreamTerminal` 携带 `partial`、已发字节/帧数、usage、`may_have_billed` 与调用快照。取消经 PluginBridge `HostRequest.model_stream_cancel`，receipt 仅表示本地取消处理（REQUESTED/ALREADY_TERMINAL/NOT_ACTIVE），不替代 terminal，也不证明未执行/未计费。插件侧需协商 `host.model.v1`（非流式）与 `host.model.stream.v1`（流式），并持有 `model.chat`。

运行配置：Model 服务必须配置 `database` 与 `model_security`；可选 `identity_endpoint`。配置键严格校验，非法配置以 exit 2 拒绝启动。

## 7 权限与安全

- 身份只来自真实 mTLS 通道：Model 校验 delegation 结构、ticket 绑定叶证书、Runtime 二跳来源与 owner；`ModelCaller.owner` 不由请求字段提供。签名、audience、撤销与当前权限仍由 Identity 在线判定，结构校验本身从不授权。
- 管理面与调用面权限分离：每个 RPC 只校验自身 `required_permission`，管理面权限不等于调用面授权；`Chat`/`ChatStream` 按精确模型资源授权，运行期每 1 秒复查，撤权/过期立即停止输出。
- secret 只在内存中出现：根密钥部署注入、不入库；Provider 请求头携带 secret；日志、审计、错误、outbox、流事件与结果均不含 secret 或 prompt 正文。
- 流式终帧只由真实 Provider 事实 + txn2 生成，防止把网络截断伪装成 COMPLETED；`may_have_billed` 只在明确已知未计费时置 false。
- 管理写命令使用幂等键与命令指纹；重放前重新鉴权，同键异内容 CONFLICT；管理 operation 有独立审计。

## 8 错误/失败/恢复语义

- Provider 错误映射（非流式与流式失败一致）：400/422→INVALID_ARGUMENT；401/403→UPSTREAM_UNAVAILABLE；404/405/410→CAPABILITY_NOT_SUPPORTED；429→RATE_LIMITED（有界 Retry-After）；其余 4xx→UPSTREAM_UNAVAILABLE；5xx、TLS、连接、超时、畸形/超大响应→UNKNOWN 且 `may_have_billed=true`。上游自由文本不透传。
- 已 dispatch 的未知结果永不自动重试，只能以同一 `logical_call_id` 查询/处置；预检/预取消等未 dispatch 失败可返回已知未计费失败。
- txn1/txn2 的 commit=Unknown 都不作“未发生”推断：txn1 返回 UNKNOWN，txn2 保持原 `Started` 由维护恢复；崩溃恢复不伪造完成。
- 流式 attempt 的失败相位：started 之前用 gRPC trailer 的 `RpcFailure`；started 之后用 `terminal + grpc OK`；硬传输关闭且没有 terminal 表示“交付未知”，此时不得报告完成，已收文本保留为 partial。
- 背压/慢消费者导致重试超预算即取消 attempt，不阻塞 HTTP 写；取消/断线后 Provider 已产生的费用不可撤回。
- 维护阶段失败只影响扫描批次，不改变已持久调用；清理失败不撤销已提交事实。

## 9 可观测性与运维

- 健康：共享进程探针 `/health/live`（不访问存储）与 `/health/ready`；Model ready 要求 Identity 通道可用（组合根再与数据库观测相 AND），依赖未连通时 RPC fail-closed。
- 结构化日志只记录稳定字段（operation/outcome/error_code/request_id/trace_id/resource_id），不记录 prompt、completion、secret 或 Provider 原始错误文本；审计写 `ModelService.*` 事件，正文列留空。
- 维护循环由共享 `run_service` 的 1 秒 dependency hook 驱动：恢复 Started、处置 UNKNOWN、清理终态，批量 32/64；busy/conflict 不退出进程，其他数据库故障按服务生命周期处理。
- 生产运维：根密钥文件与 `key_version` 必须随部署安全轮换；密钥轮换用 `RotateProviderSecret`，不得直接改库；UNKNOWN 只能通过查询/`ResolveModelCall` 处置，不能手工把记录改成成功。
- 观察：`ListOperationChanges` 提供 600 秒有界 operation 变化；调用事实按 `GetCallStatus`/数据库只读核对。

## 10 测试与验收证据

- 纯单元/边界：`p11_model_gateway`（领域规则）、`p11_model_gateway_application`（两次事务/UNKNOWN/AEAD/错误映射）、`p11_model_secret_crypto`、`p11_model_http_provider`、`p11_model_identity_client`、`p11_model_rpc`、`p11_runtime_model_client`、`p11_model_bridge`，以及流式切片 `p11_streaming_model_codec`、`p11_streaming_model_rpc`、`p11_streaming_model_application`、`p11_streaming_sse_parser`、`p11_streaming_http_provider`、`p11_streaming_upstream_*`。
- 真实进程链：`p05b_model_chain_e2e`（受管 Host→Runtime→Model→本地 HTTP Provider 正/负矩阵）、`p05b_model_chain_fail_closed_e2e`、`p11h_managed_model_e2e`/`p11h_model_chain_fail_closed`/`p11h_provider_fixture`、`p11_streaming_model_rpc_e2e`（真实 PG16.4/TLS/Identity/Model + 本地 SSE）、`p11_streaming_managed_model_e2e`（受管插件→PluginBridge→Runtime→Model→SSE 的 18 项流式矩阵）。
- Control 管理面：`p11_control_model_rest`（真实 Control + Identity/Model fixture：metadata/list/update/disable/enable/secret 轮换不回显/UNKNOWN resolve/operation）。
- SDK 侧：`sdk/python/tests/test_model.py`、`test_model_sdk.py`、`test_model_stream.py`（流式关联、序号、背压、取消与本地 unknown delivery）。
- 验收记录见 `proto/tests/contract_status.json` 的 target evidence 与 `docs/architecture.md` §9；本机流式受管整链连续 5 次隔离通过（约 124–132s），`RUN_SERIAL` 防止重 e2e 并行干扰时序断言。

## 11 已知限制与后续

- 真实外部付费 Provider 与计费/配额核对未验收；本地 HTTP/SSE fixture 只证明协议与失败语义，不等于外部账单一致。
- 仅支持 `openai_chat_completions` 纯文本；工具调用、函数、多模态、非文本 finish 明确不支持，不会降级为另一次调用。
- UNKNOWN 依赖人工/规则处置与 30 天窗口，没有自动重放或对账；崩溃时已发生的 Provider 费用不可撤回。
- 流式慢写真实网络路径以取消传播/worker join 回归覆盖，未在真实慢消费者链路上量化写预算；每连接/进程流上限仍是本地进程级。
- 六服务真实部署、跨主机、真实 QQ 与环境级故障演练属部署阶段；多副本/HA 不在范围。
- 已登记契约缺口：历史 inline `CONTRACT_STATUS` 注释与 target registry 状态存在文本差异，以 `proto/tests/target_spec.json` 与 `contract_status.json` 为准。
