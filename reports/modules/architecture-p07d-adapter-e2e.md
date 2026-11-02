# P07d 真实 NoneBot Adapter + fake OneBot WS + 真实 Runtime 全链路 e2e

日期：2026-11-02；worktree `.worktrees/p07d`，分支 `codex/p07d`，基于 main@5565f33。

## 目标与范围
验证 P07c adapter Action 与 P07a/P07b Runtime Action 派发/结果/回执在真实进程链路可用：真实 Runtime（真 PG16.4、真 mTLS）↔ 真实 NoneBot adapter 子进程 ↔ loopback fake OneBot v11 WS；插件通过真实 SDK PluginBridge 提交 Action。不使用真实 QQ/NapCat。

## 实现
1. `tests/architecture/p07_adapter_e2e.py`：启动真实 Runtime 与 PG（复用 p06 e2e 的 seed/PKI 约定），fake OneBot WS 实现事件注入和 Action API（send/delete/mute/kick/approve）成功/失败响应；真实 adapter 子进程用 `ONEBOT_WS_URL` 连接 fake 平台、用真实 `BRIDGE_ENDPOINT`/bootstrap 连接 Runtime。
2. 修复 e2e 暴露缺陷：adapter 原用 `config.account_id` 调 `nonebot.get_bot()`，而 Runtime 账号资源 id 是 UUID、OneBot bot 键是平台数字 self_id，真实平台必失败。`__main__.py` 新增可选 `ONEBOT_SELF_ID`（缺省回退 ACCOUNT_ID）仅用于 bot 查找，bridge/事件映射仍用 Runtime 账号 UUID。
3. CTest `p07_adapter_e2e`（labels `p07;integration;fault`，需要 adapters/nonebot/.venv 的 nonebot2[websockets]）运行真实进程矩阵。

## 覆盖矩阵
- 成功：plugin Action → Runtime 派发 → 真实 adapter → fake OneBot `send_group_msg` → result/evidence → receipt；Action state=SUCCEEDED、execution_outcome=KNOWN_SUCCEEDED、platform_message_id 记录。
- 平台失败：fake OneBot retcode 1200 → adapter 稳定码 RATE_LIMITED → Action state=FAILED、KNOWN_FAILED，绝不伪成功。
- 重复平台帧：Runtime events 数量不增、Action 数量不增（adapter outbox 可重复入队但 Runtime event key 去重）。
- dispatch 后取消：adapter 已收到 Action 后 plugin cancel 返回 CONFLICT，Action 终态不被改写。
- 平台 WS 断线重连：连接重建后继续提交 Action 并成功，平台调用次数不重复。
- 泄漏检查：adapter 进程输出不含 bootstrap 凭据/平台 token。

## 未覆盖 / 真实 QQ 缺口
- 真实 QQ/NapCat 未运行：真实平台账号登录、真实审批/撤回/禁言/踢出凭证、NapCat 扩展 reaction API、真实网络与封禁风险仍属 M2/T07b。
- 平台延迟/EXPIRED 迟到 late-evidence、action UNKNOWN 后重连补报窗口未在本 e2e 矩阵中覆盖（P07c adapter journal 单测覆盖；Runtime P07a 单测覆盖 EXPIRED/EVIDENCE_ONLY），需要后续真实延迟故障注入。
- adapter 重复帧的本地 outbox 不去重（Runtime event 去重兜底），未在 e2e 中断言 adapter 内部行数语义。
