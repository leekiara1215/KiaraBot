# P06g：真实 adapter + Runtime + 受管 SDK plugin 全总线 e2e

状态：已实现单链 e2e；真实 QQ/NapCat 与单链 UNKNOWN/EXPIRED 迟到证据仍待。

## 证据

`tests/architecture/p07_adapter_e2e.py`（p06/p07 标签）：
- 真 PG16.4 一次性集群、真 mTLS Runtime、真 `kiarabot-adapter` 子进程 + fake OneBot WS、
  独立 `python -m kiarabot_sdk` plugin 子进程（真实 PluginBridge）。
- 平台消息 → adapter 事件/outbox → Runtime 事件 inbox → 订阅 delivery target/lease/ACK →
  plugin 收到事件；plugin 提交 send_message + policy mute（真 PG `action_policies`）→ Runtime
  派发 adapter → fake OneBot `send_group_msg`/`set_group_ban` → result/evidence/receipt →
  终态 SUCCEEDED。
- 重复平台帧不重复建事件/Action；adapter WS 断开重连后 Action 不重复执行；非订阅/离线插件
  无 delivery target；post-dispatch cancel CONFLICT；adapter/plugin 日志 secret 检查。

## 未完成

- 真实 QQ/NapCat 平台（M2 环境）。
- 单链 UNKNOWN/EXPIRED 迟到证据：p06_bridge_acceptance 手工 wire fixture 已覆盖
  UNKNOWN/EXPIRED/证据 receipt，但未合并到真实 adapter+SDK plugin 链。
- format16 本机不可用（无 clang-format16）。
