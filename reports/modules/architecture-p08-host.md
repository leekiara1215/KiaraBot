# P08 Host 受管执行与 HostBridge（P08a–P08b-3 进展）

状态（2026-11-02）：**P08 未退出**。传输层（P08b-2b）、Runtime 命令/观察持久化（P08b-3）、
HostBridge 下发/receipt/observation PG 接线（P08b-3a）与真 PG16.4+真 Runtime+fake Host 的
e2e（P08b-3b）已完成；真实 `kiarabot-plugin-host`+受管 SDK 插件进程联测、以及受管 bootstrap
自动写入 Host credential_directory 仍未完成。

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
