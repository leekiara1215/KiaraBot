# Plugin Host 插件宿主模块设计

本文描述 `kiarabot-plugin-host` 进程：单台主机上受管插件进程的唯一执行者、Host 本地进程事实的权威、Host 维护/排空与管理面（HOST-02/03/04）的所有者，以及 bootstrap 凭据的受限消费端。跨进程契约唯一来源是 `proto/kiarabot/v1/host.proto`；应用实现在 `core/src/application/target/host_service.cpp`、`core/src/application/target/host_maintenance.cpp`、`core/include/kiarabot/core/application/target/host_management.hpp`，传输与客户端在 `core/src/runtime/platform/host_bridge_client.cpp`、`core/src/runtime/platform/host_bridge_server.cpp`、`core/src/runtime/platform/host_management_rpc.cpp`、`core/src/runtime/platform/host_runtime_client.cpp`、`core/src/runtime/platform/host_identity_client.cpp`，本地存储实现在 `infrastructure/src/host_journal.cpp` 与 `infrastructure/src/host_maintenance_sqlite.cpp`，组合根为 `core/app/host_composition.cpp`。模块在整体架构中的位置见[架构总览](../architecture.md)（§3、§6、§8）。

## 1 模块定位与职责

Host 负责：

- 受管执行：从已认证 HostBridge 流接收 `ApplyDesiredProcess`/`StopExecution`/`DrainHost` 命令，持久化命令身份后再产生进程副作用，按部署白名单 spawn 插件进程。
- 进程事实：以执行 marker（pid + 启动标识）认领/核验子进程，向 Runtime 上报带本地单调 sequence 的执行观察；崩溃恢复和 SIGTERM 收尾保持 UNKNOWN 语义。
- 命令幂等：`command_id + payload hash` 相同重放返回原回执，不同内容 `CONFLICT`；同一 execution generation 永不重复 spawn。
- 维护/排空（HOST-02）：持久受理 Begin/Resume/Cancel operation，本地关闭接纳、与 Runtime admission gate 协调、冻结并逐项停止受管执行；`HostManagementService`（HOST-04）是 Control 的 owner 管理面。
- bootstrap 凭据消费：从部署提供的 `credential_directory/<instance_id>.bootstrap`（0600、运行用户所有）读取，只通过 launch profile 的白名单环境变量注入子进程。
- 本地观察：Host 自己的 operation/admission 变化 journal（600 秒窗口），供 Control WS/REST 恢复。

Host **不负责**：desired state/注册/配置应用版本的权威（归 Runtime）、任何未认证 unary `HostService` 监听、插件业务逻辑、调度与消息投递、中央业务库；也不提供 shell 或任意 PID/path 管理接口。

## 2 边界与依赖

- Runtime：Host 主动连 Runtime 内部 mTLS `grpc_listen` 上的 `HostBridge.Connect` 双向流；Runtime `hosts[]` 以证书 SHA-256 预注册 `host_id`，Host 配置的 `host_id` 必须一致。Runtime 侧 `host_connections`/`host_commands`/`host_observations`/`host_admission_gates` 是投影与 outbox/inbox，不是 Host 本地事实的来源。
- Identity：HostManagement 调用要求 Control 平台 peer + audience=`host` 的短期 delegation，并由 Host 以自身注册平台 peer 身份在线 `CheckAuthorization`；Runtime 的 admission gate 另经 `IdentityService.CheckHostGateAuthorization`。未配置 `identity_endpoint` 时管理面 fail-closed。
- Control：`HostManagementService` 由 Control owner gateway 经固定离线 host endpoint 调用（`control_api.host_endpoint` 或 `host_endpoints` 注册表），客户端不能自报 endpoint。
- 本地依赖：Host SQLite（journal + maintenance，WAL、单写入者）、POSIX supervisor、部署的 packages/profiles/environment 与 credential 目录；不访问四业务 PG。
- 与插件：Host 只管理进程生命周期与环境；插件经自身 TLS PluginBridge 注册到 Runtime，Host 不转发插件业务流量。
- 与其他服务：Adapter 不在 Host 内运行；Host 停止不代表 Adapter 停止。

## 3 进程/组件结构

```text
kiarabot-plugin-host（core/app/host_main.cpp → host_composition.cpp，run_service）
  ├─ HostService                 host_service.cpp：命令/执行/恢复/观察/停止
  │    ├─ SQLite HostJournal（command_journal/managed_executions/execution_observations）
  │    ├─ POSIX ProcessSupervisor（host_process.cpp，独立进程组，TERM→KILL）
  │    └─ HostBridgeClient（host_bridge_client.cpp，5s heartbeat + fence 校验）
  ├─ HostMaintenanceApplication + SQLite store（host_maintenance.cpp / host_maintenance_sqlite.cpp）
  │    ├─ RuntimeAdmissionGatePort → HostRuntimeClient（→ Runtime.SetHostAdmissionGate）
  │    └─ HostMaintenanceAuthorizationPort → HostIdentityClient（→ Identity.CheckAuthorization）
  ├─ HostManagementService RPC    host_management_rpc.cpp（owner mTLS listener）
  └─ 探针 /health/live、/health/ready（共享 run_service）
```

维护进度由共享 `run_service` 的 1 秒 dependency hook 推进：每 tick 最多推进一个持久阶段或执行一次有界进程停止；owner RPC 与后台推进共享同一 maintenance mutex。SIGTERM/SIGINT 的 `before_stop` 先执行 `HostService::stop()`（有界 TERM、observation flush、prune、关闭 journal、停止 Bridge），再释放 gRPC 监听。

## 4 数据模型与持久化

**Host 本地 SQLite（权威）**

- `command_journal`：`command_id` 主键、`payload_sha256`、kind/state、instance/generation、receipt、execution 快照、时间与 `retain_until`。
- `managed_executions`：`(instance_id, execution_generation)` 主键、host/installation、`desired_version`、`pid`、`process_marker`、state、outcome、exit code/signal、时间与 `command_id`；保留期 resolved 7 天、unresolved/未知 30 天。
- `execution_observations`：本地唯一单调 `sequence`、host/connection generation、command id、执行快照、`reported` 标记与保留时间；未上报项在重新注册/resync 后补发，重复上报幂等。
- 维护库：`host_maintenance_meta`（schema 版本、observation epoch/head/low_water）、`host_maintenance_state`（host、资源版本、admission、活动 operation、gate 版本）、`host_maintenance_operations`（按 actor+action+幂等键唯一）、`host_maintenance_cancellations`、`host_maintenance_audit`、`host_operation_changes`。operation 变化保留 600 秒、容量 65536 条/64MiB，低水位淘汰；普通 Host 重启保持 epoch，可信离线重建才显式轮换 epoch。
- 凭据文件：`credential_directory/<instance_id>.bootstrap` 为 0600 原子文件，只注入白名单 `credential_environment`；明文不落 SQLite、日志、argv、观察或 Runtime 数据库（Runtime 只存 SHA-256）。

**Runtime 侧（投影，非 Host 权威）**：`host_connections`、`host_commands` outbox、`host_observations` inbox、`host_admission_gates`（Runtime schema v25，见 `infrastructure/postgres/schema/v1.hpp` 与[基础设施模块](infrastructure.md)）。

## 5 关键流程/状态机

**启动与恢复**：严格校验 `host` 配置对象（仅 plugin-host 允许；`host_id`、绝对路径 `sqlite_path`/`credential_directory`、`runtime_endpoint`、非空 packages/profiles、environment 上限；其他服务出现 `host` 对象 exit 2）→ 打开 journal → 恢复：marker 仍存活则认领 Running 并补观察；进程已消失但无本进程 `waitpid` 确认的退出事实则保持 Unknown；同一 generation 不重复 spawn → 启动探针/gRPC 监听 → HostBridgeClient 后台有界退避注册（200ms→5s）。readiness = `journal_open_ && bridge.registered()`；Runtime 不可达时进程仍存活、ready=false，不伪造“已注册/已停止”。

**注册与心跳**：首帧必须为 registration；Runtime 由真实 mTLS 叶证书 SHA-256 匹配部署 allowlist 得到 `host_id`，回 `registered{host,fence}`；之后每帧必须携带当前 fence，旧 connection generation 被 `CONFLICT` 拒绝，新连接替换旧连接。heartbeat 5 秒，Runtime 15 秒无心跳只影响在线投影，不改写 process outcome。

**Apply/Stop**：先持久化命令（同 id+hash 幂等、异 hash `CONFLICT`），在进程副作用前检查本地 admission gate；解析包摘要（必须命中部署 allowlist）、launch profile、参数名与环境变量白名单、凭据文件；spawn 到新进程组、stdio detached。拒绝命令以 `persisted=false` 回执表达，不产生进程/观察。停止为 TERM → grace（≤10s，默认 10s）→ KILL；只有 supervisor 对本组合根 spawn 的原 child 的真实 waitpid 退出事实才可记为 `Exited+Known*`，marker/pid 暂时可见、alive=false、proc 读取失败、EPERM/ESRCH 都不能单独证明停止；无法确认保持 `Unknown`。已由 waitpid 确认退出的重复停止是非 forced 成功，不再发信号。

**优雅 drain**：SIGTERM/SIGINT 时按固定顺序：对每个未终态执行有界 TERM/KILL → flush 最终 observation → 有界 prune → 关闭 journal → 停止 Bridge；任何未确认终止保持 Unknown，drain 不把 UNKNOWN 改写为 stopped/succeeded。

**维护 Begin**（`HostMaintenanceApplication::advance`）：`Accepted` 持久受理后，首个推进步骤原子关闭本地 admission 并进入 `Closing`；调用 Runtime gate 关闭派发（`SetHostAdmissionGate` 携带 operation/maintenance_version/expected_gate_version/admission=false；Runtime 校验真实 Host 叶 allowlist、当前 HostBridge fence，并经 Identity `CheckHostGateAuthorization` 在线核定）→ `GateClosed` 后冻结 ≤256 个受管执行，进入 `Draining`，每 tick 最多停止一个目标（按 journal 的 instance/generation/marker 核验）→ 全部 KnownStopped 才 `Finished/Succeeded`。任一执行未知、依赖故障或 gate 进度写失败都置 `Blocked`（稳定 reason + next_attempt），绝不伪成功。**Cancel** 只允许 `Accepted` 且尚未改变接纳/未请求 gate；之后 `CONFLICT`。**Resume**：先确认无活动维护冲突与未决执行，持久进入不可取消的 `Opening`，再确认 Runtime gate 打开，最后本地原子开放接纳并 `Succeeded`；Restore 不复活旧任务、凭据、租约或被清空的未知状态。

**HostManagement RPC**：每次调用认证 Control 平台 peer + 新鲜 delegation（绑定实际叶证书、audience=`host`），应用层按当前存储的 host 资源核对 `host.read`/`host.execute`；wire `host_id` 与本 owner 存储不一致返回 `NOT_FOUND`（不泄露跨 host 存在性）。Control 侧 `202` 与 owner 持久受理都只表示 durably accepted，`receipt persisted=true` 也不是完成。

## 6 接口与契约

- `HostBridge.Connect(stream HostToRuntime) returns (stream RuntimeToHost)`：`HostToRuntime` 携带 registration/inventory/receipt/observation/heartbeat；`RuntimeToHost` 携带 apply/stop/drain/registered/failure/heartbeat_ack。首帧注册后所有帧必须带当前 `Fence`；新连接替换旧 connection generation。HostBridge 只注册在 Runtime 内部 mTLS `grpc_listen`，不占用外部 Bridge 监听。
- `HostService`（`proto/kiarabot/v1/host.proto`）的 RegisterHost/ReportInventory/ApplyDesiredProcess/StopExecution/DrainHost/GetExecution/WatchExecutions 是**方法目录**：Host 不注册 unary HostService 监听，命令只从 HostBridge 流入。
- `HostManagementService`（owner mTLS，仅 Control 调用）：`GetMaintenance`（2s/5s，`host.read`）、`BeginMaintenance`（5s/30s，`host.execute`，幂等，`grace_ms` 0→10000、>10000 `INVALID_ARGUMENT`）、`ResumeAdmission`（5s/30s，幂等）、`GetOperation`（2s/5s）、`CancelOperation`（5s/30s，幂等）、`ListOperationChanges`（500ms/500ms）；全部要求 `RESOURCE_HOST` scope 与在线授权。
- Runtime/Identity gate 契约：`RuntimeService.SetHostAdmissionGate` 只接受当前注册 Host 的 mTLS/fence/维护意图单调版本；`IdentityService.CheckHostGateAuthorization` 由 Runtime 调用，携带实际 Host 叶证书 SHA-256 与单 Host scope，返回是否允许及 credential/principal 版本；允许不缓存。
- Host 配置：`host_id`、`sqlite_path`、`runtime_endpoint`、`credential_directory`、`packages[]`（plugin_type_id/root/sha256/launch_profile/allowed_argument_names）、`profiles[]`（executable/fixed_arguments/allowed_environment_names/credential_environment）、`environment{}`；可选顶层 `identity_endpoint`。严格 schema，非法材料启动失败。
- Control 侧对 Host 的 REST 入口（`/hosts/{hostId}/maintenance`、`/hosts/{hostId}/resume`、`/operations/host/{operationId}/cancel`）属于 Control owner gateway，不属于 Host 进程的对外接口。

## 7 权限与安全

- 传入命令只认已认证流：HostBridge 证书来自部署签发，Runtime 用真实叶证书指纹匹配 allowlist；Host 不采信任何自报 host_id/owner/fence。
- 帧 fence 强制执行：旧 connection generation 的 inventory/receipt/observation 均被拒绝；同一 host 新连接替换旧连接。
- 管理面：Control 平台 peer + delegation 叶绑定 + 每调用 Identity 在线核验；不持久化 Control bearer 供后台使用；跨 host 请求返回 `NOT_FOUND`，存储资源身份决定可见性。
- 进程边界：包摘要必须命中部署 allowlist 且与 launch profile 匹配、参数名/env 名受白名单约束、可执行文件来自部署 launch profile；不开放 shell、任意命令、任意 PID 或任意文件路径；停止只接受 journal 已登记的 instance/generation/marker。
- secret：bootstrap 明文只在 0600 文件与子进程环境变量中出现，不进日志/argv/观察/数据库；Host 维护变化日志只保存 operation 安全投影，不含 PID、marker、配置、凭据或模型/消息正文。
- Runtime gate 不把“allow”变成进程停止证明：gate 只是禁止新派发的意图副本，执行停止仍只以 Host waitpid 事实为准。

## 8 错误/失败/恢复语义

- 启动配置非法（缺 host 材料、路径非绝对、包/环境非法、TLS/凭据缺失）使进程启动失败；Runtime 不可达则离线存活、readiness=false，恢复后重新注册并补发观察。
- 命令拒绝：`persisted=false` 回执；Host 本地不产生 spawn/journal 伪条目；Runtime 保持 Pending/Unknown，不伪 stopped。
- 停止未确认：`Unknown`/`Blocked`；SIGKILL 后重启按 marker 认领仍存活子进程，不重复 spawn；已确认退出后的重复停止幂等。
- Bridge 断开：`send_observation`/`send_receipt` 返回可重试 `UNAVAILABLE`，观察已在本地下次注册/resync 补发；不发“假回执”。
- Runtime 侧 outbox：命令 30s 无回执转 Unknown，不推断进程停止；receipt 按 `(command_id, connection_generation)` 条件更新，旧 generation 被拒；observation 按 `(host, connection_generation, sequence)` 去重。
- 维护：gate/identity/执行任一依赖失败、提交结果未知、执行集合超 256 或身份不一致 → `BLOCKED`（可恢复，下轮重试），operation 可沿原身份恢复；Cancel 在 gate 之后 `CONFLICT`；Resume 有未决执行时 `Conflict`/`BLOCKED`，不把未知清零。
- SQLite：WAL + 单写入者 + 2 秒 busy；事务先取写锁再分配 sequence，回滚不产生变化通知；命令身份与 operation/audit/changes 同事务提交，崩溃恢复不猜测完成。

## 9 可观测性与运维

- 健康：共享探针 `/health/live` 与 `/health/ready`；ready 要求 journal 已打开且 HostBridge 已注册，依赖组件分别投影 business implementation/database schema/runtime 状态；未注册时 live 仍 200、ready 503。
- 日志：固定键值字段（operation/outcome/error_code 等），不写插件正文、secret、prompt；Host 维护 operation/audit/changes 以本地 journal 为准，可经 `HostManagementService.GetOperation` 与 `ListOperationChanges` 读取。
- 运行：Host 每主机一个 `host_id`，可管理多个受管实例；SQLite 文件、credential 目录与部署 packages/profiles 是本地卷，需随 Host/Adapter 一起备份；普通重启保持 maintenance version 与 observation epoch。
- 凭据：部署 helper `scripts/deploy/target_host_credential.py` 生成/校验 0600 bootstrap 文件（幂等、`--replace` 显式轮换、`prune` 清理、`verify` 校验），受控 bootstrap 交付由 Control 固定 recipient 文件 sink 写入 0600 文件，Host 只读消费；明文丢失不能从摘要恢复，只能显式受权重发。
- 维护 runbook：Begin 前先确认无未知执行；grace 上限 10000ms；Begin/Resume 的响应是“已持久受理”，最终结果以 GetOperation/operation changes 为准；UNKNOWN/Blocked 不应手工改成成功。

## 10 测试与验收证据

- 组件单测（C++）：`p08_host_service`、`p08_host_bridge`、`p08_host_bridge_server`、`p08_host_process`、`p08_host_journal`、`p08_host_maintenance`（真实 SQLite/触发器回滚/fork 丢响应/symlink 边界）、`p08_host_management_rpc`（跨 host `NOT_FOUND` 等）、`p08_host_runtime_client`、`deploy_host_credential`（`scripts/deploy/tests/host_credential_test.py`）。
- 进程级 e2e：`p08_host_bridge_e2e`（真 PG16.4 + 真 Runtime + fake Host）、`p08_managed_host_e2e`（真 Host + 真受管 SDK 插件 + 真 PG）、`p13_host_sdk_drain`（Host/SDK 优雅 drain 与 SIGKILL 恢复）、`p12_host_control_rest`（真 Control + fake HostManagement）、`p12_host_control_chain_e2e`（真 Control→Identity 委托→真 HostManagement→fake Runtime gate）、`p08_host_management_e2e`（真 Host 进程 + fake Runtime/Identity）。
- 全真实链：`p12_host_real_chain_e2e`（真 owned PG + 离线 bootstrap + 真 Identity/Runtime/plugin-host/Control；Begin→本地关接纳→HostBridge→Runtime gate→Identity 在线核定→SQLite 持久；含 Runtime 故障 BLOCKED、Host 重启、host-2 无 grant 拒绝）、`p12_host_dual_host_chain_e2e`（双 Host + 单 Control 多 endpoint registry 的路由隔离/故障隔离）、`p13h3_joint_delete_production_e2e`（真实 Host UNKNOWN/StopExecution receipt 驱动 BeginDelete 收敛或 BLOCKED）、`p12_bootstrap_delivery_e2e`（Control→Runtime→固定文件 sink→消费端的受控交付，含 ACK 丢失）。
- 注册位置：`cmake/platform.cmake`、`cmake/host-management.cmake`、`cmake/host-maintenance.cmake`、`infrastructure/postgres/CMakeLists.txt`（`p08_host_bridge_e2e`/`p13h3_joint_delete_production_e2e`/`p12_host_real_chain_e2e`/`p12_host_dual_host_chain_e2e`）。

## 11 已知限制与后续

- 真实 QQ 平台与真实受管插件业务场景不在 Host 验证范围；10s grace 上限只按当前 5s 生效路径验证。
- proto 冻结无 Restart RPC：Host 重启走 P13b drain/恢复，实例 restart 由 stop/start desired-state 组合；launch_profile/arguments/environment 仍由部署配置映射，Runtime 不解析 manifest。
- 维护 gate 的真实 Identity/Runtime 组合已由 `p12_host_real_chain_e2e` 覆盖，但生产 CA/证书轮换、多主机跨主机接管、满盘与真实规模仍属部署环境项。
- 受管 bootstrap 的自动预置仍是部署 helper/受控交付文件契约，不是 Runtime 直接写 Host 文件；普通一次性 `bootstrap_credential` 响应丢失后只能按原幂等请求恢复或显式 reissue。
- 观察窗口 600 秒；更早变化只能从 owner journal 或 HostManagement/GetOperation 快照恢复，旧 cursor 会要求 resync。
- Host 是单机进程事实权威，不是第二套中央业务库；多主机迁移必须在确认旧执行停止或资源隔离后才激活新代次，网络超时不能当作强制接管依据。
