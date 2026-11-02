# P08 Host 受管执行与 HostBridge（P08a–P08b-3 进展）

状态（2026-11-02）：**P08 未退出**。传输层（P08b-2b）、Runtime 命令/观察持久化（P08b-3）、
HostBridge 下发/receipt/observation PG 接线（P08b-3a）、真 PG16.4+真 Runtime+fake Host 的
e2e（P08b-3b）与 P08d 部署 helper/Delete 快照已完成；真实 `kiarabot-plugin-host`+受管 SDK 插件
进程联测仍未完成。

## 已实现

- P08a Host 本地：SQLite command_journal/managed_executions/execution_observations、进程
  supervisor marker、HostBridge client 离线重试、受管进程启动/停止/恢复。
- P08b-1 应用规则：注册/心跳/15s 离线投影/fence；P08b-2 的 PG `host_registration()` 与
  `host_connections` 表已合入。
- P08b-2b HostBridge 服务端：owner mTLS `grpc_listen` 上的同步 bidi；首帧 registration +
  `extract_peer_certificate` + 部署 `hosts[]` allowlist；fence/旧代次拒绝；heartbeat_ack；
  inventory/receipt/observation 帧校验与 P08b-3 回调挂点；in-process conformance 测试。
- P08b-3 本次：Runtime UoW `host_commands()` 端口与 PG 实现；`host_commands` outbox +
  `host_observations` inbox 表（Runtime schema v14，实测 descriptor
  `7620002f564f2db02e83bd77d89e78fe` / relation_count 152）；P05 `CreateInstance`（带 host）与
  `SetDesiredState` 在同一 UoW 内写入确定性 command_id（`hc-<payload sha256>`）的命令快照；
  受管实例也生成一次性 bootstrap（仅返回部署持有者，PG 只存 SHA-256）；PG 集成测试覆盖
  command 幂等/冲突/待发/回执/observation 去重与重启持久。
- P08b-3a：HostBridge server 注册/心跳后按当前 generation 读 pending 并下发 apply/stop/drain；
  receipt 条件更新（旧 generation 拒绝，重复 receipt 幂等）；observation 按
  (host,generation,cursor.sequence) 去重落 PG；重连重放未确认 command_id；30s 超时 Unknown，
  7d/30d 保留清理；fake Host server 测试覆盖下发/回执/观察/重放/旧 fence 拒绝。
- 未确认/失败命令不伪造 stopped：outbox 行保持 Pending/Unknown，receipt 只记录
  `receipt_persisted/outcome`，Runtime 不自行写 process stopped。

- 2026-11-02 合并回归：P13c 合入时 runtime_composition 的 HostBridgePersistence 回调接线被冲突解决误删，HostBridge 只注册不派发，main 的 `p08_host_bridge_e2e` 在 apply 阶段超时；已恢复 pending/dispatched/receipt/observation 四路回调（保留 P13c HostLifecycleParticipant），e2e 等待增加阶段标签，干净增量构建后通过。

## 未完成 / 风险

- 真实 Host 二进制 + 真 PG 的 HostBridge 命令端到端（含 P08a client journal/resync）未跑；
  当前为 fake Host 帧测试 + 真 PG 仓储测试。
- 受管 bootstrap 仅生成并随 CreateInstance 响应返回给部署方，尚未通过 HostBridge/部署目录
  自动预置到 Host `credential_directory`；P08a Host 侧启动消费已有文件约定，操作步骤需文档化。
- package_sha256 使用安装 manifest 摘要（Core 不解析 manifest）；launch_profile 留空，需部署
  host 映射补齐；restart/Delete 参与者的命令快照尚未接线。
- 受管 bootstrap 尚未自动预置到 Host credential_directory；restart/Delete 专门命令未接；
  P08 退出判定：**未通过（3b 待完成）**。


## P08d：部署 helper + Delete 命令快照（2026-11-02）

已实现：

- `scripts/deploy/target_host_credential.py`：从 CreateInstance/BindAdapter 的一次性
  `bootstrap_credential` 响应生成 Host `credential_directory/<instance_id>.bootstrap`；原子写、
  严格 0600/regular/非符号链接，支持相同凭据幂等、`--replace` 显式轮换、`prune` 清理非活跃
  实例与遗留临时文件、`verify` 启动前权限/owner 检查；明文只从 stdin/0600 response 文件读取，
  不进入 stdout/stderr/argv/日志。
- 单元测试 `scripts/deploy/tests/host_credential_test.py`（6 用例：存储/幂等/轮换拒绝与替换/
  bind adapter 响应/清理/不安全输入与符号链接/verify），CTest `deploy_host_credential`。
- P13c Delete Host 参与者的 StopExecution 快照补齐 `installation_id`、`package_sha256`、
  `desired_version`、`desired_state=Stopped`（launch_profile/arguments/env 保持部署映射事实，
  StopExecution 帧不需要）；现有测试扩展覆盖快照字段与 Runtime 重启后同 command_id 重放
  幂等、无 receipt 仍 BLOCKED（UNKNOWN 不伪 stopped）。

未完成/边界：

- P08e：`p08_managed_host_e2e` 已用真 PG16.4 + 真 Runtime + 真 `kiarabot-plugin-host` + 真受管
  SDK 插件子进程跑通注册/apply/spawn/PluginBridge 注册/Stop/Drain/Host 重启；修复 Stop/Drain
  handoff payload 未写导致 Host 拒绝命令的缺陷。
- 未新增 Restart RPC（proto 冻结）；Host 重启走 P13b drain/恢复，实例 restart 由
  stop/start desired-state 组合，host_command 快照复用既有 Apply/Stop 命令。
- Runtime 不解析 manifest，launch_profile/arguments/environment 仍由部署 host 配置映射；
  这些字段在 Stop 快照中为空、在 Apply 快照中来自当前 Core 可用的安装事实。
- P08 退出判定：**未通过**（真实 Host+SDK 进程联测与端到端凭据注入仍待 P08b-3b/后续）。
