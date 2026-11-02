# P13b Host/SDK/Adapter 优雅 drain 与崩溃恢复（实现记录）

日期：2026-11-02；worktree `.worktrees/p13b`，分支 `codex/p13b`，基于 main@d4da6f9。
范围：Host 有界停止顺序、SDK ShutdownNotice 收尾、Host/SDK 崩溃/重启恢复；Adapter 复用既有 shutdown 语义并纳入验证。

## 实现
1. Host：`HostService::stop()` 增加最终 observation flush 后的有界 journal prune，再关闭 SQLite；组织根 `before_stop` 在 gRPC 监听释放前显式执行 stop（此前仅析构时隐式）。活动 execution 有界 TERM→KILL，UNKNOWN 不改为 stopped/succeeded；同代次不重复 spawn。
2. SDK：修复 ShutdownNotice 立即 break reader 导致在途事件等待回执只能超时的问题。drain 期间继续读取 receipt/host/action 响应直到队列排空或 drain deadline，然后结束；on_stop 一次、退出码 0、不重连。
3. 测试：Host service 单测覆盖停止顺序（TERM 全部活动、flush 在 prune/close 之前、journal 不无 prune 关闭、UNKNOWN 不伪成功）；SDK 单测覆盖在途 handler ACK 完成后 on_stop、无重连；真实 P08a Host + 真实 managed SDK 进程覆盖 SIGTERM 优雅退出+SQLite integrity，以及 SIGKILL 后重启恢复（同一 execution pid、不重复 spawn、崩溃窗口不写成功）。
4. Adapter：沿用已有 ShutdownNotice/drain/outbox 保留/退避重连/on_shutdown 顺序；回归 108 个 adapter 单测全通过。

## 验证
- 全量 full-locked 构建（Host/SDK 相关目标 + CTest 注册）。
- `ctest -R 'p13|p08|p06|p05'`：p13_host_sdk_drain（真实 Host+SDK）、p13_runtime_lifecycle、p08_host_service、p08_host_bridge 等通过。
- SDK pytest 136+ passed（本地需 generated proto；单独运行通过），Adapter pytest 108 passed。
- 行为约束：Host 崩溃后 SDK 子进程可能仍存活，重启恢复时按 marker/pid 识别并在优雅停止时真实终止；不因 Host 重启自动改判成功。

## 未完成 / 交接
- P08b-3a Host 命令下发并行开发：其 `DrainHost/StopExecution` 命令复用本任务的 Host drain/停止语义；P13a BeginDelete 的 Host 参与者在 P08b-3a 接线后把 `BLOCKED` 转真实停止/观察。
- Adapter 的同机/异机真进程故障矩阵（真实 NapCat/QQ）未执行；Adapter 侧使用既有 fake runtime 回归。
- Host 常驻 periodic maintenance（非仅 shutdown prune）留 P14 部署/运维；P08d 负责该 pytest 的正式 CTest 注册；本任务已注册 p13_host_sdk_drain，可被 P08d 调整。
- 未跑真实平台/跨主机网络分区；P13c/P14 负责跨服务 shutdown 编排与故障演练。
