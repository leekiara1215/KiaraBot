# P08 Host 受管执行与 HostBridge（P08a–P08b-3 进展）

状态（2026-11-02）：**P08 未退出**。已完成传输层（P08b-2b）与 Runtime 命令/观察持久化底座
（P08b-3 部分），真实 Host 命令下发、receipt/observation 的 HostBridge 持久化接线与受管
bootstrap 经部署目录交付仍待完成。

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
- 未确认/失败命令不伪造 stopped：outbox 行保持 Pending/Unknown，receipt 只记录
  `receipt_persisted/outcome`，Runtime 不自行写 process stopped。

## 未完成 / 风险

- HostBridge server 尚未从 outbox 取命令下发 apply/stop/drain，也未把 receipt/observation
  帧写入 PG（当前只有校验与 transport-only 回调挂点）；重连重放、resync 去重端到端未接。
- 受管 bootstrap 仅生成并随 CreateInstance 响应返回给部署方，尚未通过 HostBridge/部署目录
  自动预置到 Host `credential_directory`；P08a Host 侧启动消费已有文件约定，操作步骤需文档化。
- package_sha256 使用安装 manifest 摘要（Core 不解析 manifest）；launch_profile 留空，需部署
  host 映射补齐；restart/Delete 参与者的命令快照尚未接线。
- 无真实 Host 二进制 + 真 PG 的 P08 端到端验收；P08 退出判定：**未通过**。
