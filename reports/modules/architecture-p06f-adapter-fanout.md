# P06f adapter→Runtime 事件 fan-out 修复报告

日期：2026-11-02；worktree `.worktrees/p06f`，分支 `codex/p06f`，基于 main@3ac3761。

## 现象与复现

P07d 真实 e2e（真 PG16.4/TLS + 真实 NoneBot adapter + fake OneBot）中 adapter outbox 有 ACK、Runtime `events` 有行，但 `delivery_targets` 为空、已注册 SDK plugin 未收到事件。P07d 只断言了 Action 链路，未断言事件 fan-out。

在 p07_adapter_e2e 增加诊断后复现：Runtime 仅收到 `payload_kind=4`（AdapterStatusChanged，连接状态事件），没有 `payload_kind=0`（MessageReceived）；`delivery_targets=0`，plugin 的 on_event 未触发。adapter 进程日志只有被脱敏的 NoneBot warning，outbox 中只有状态事件。

## 根因（两处，均在 adapter 测试/映射链路，不是 Runtime fan-out 或 proto 契约）

1. **fake OneBot 消息帧的 `message_id` 类型不符合 NoneBot v11 模型**：P07d 的 `message_event()` 使用字符串 `msg-p07d`。NoneBot OneBot v11 的 `MessageEvent.message_id` 是整数；pydantic 校验失败后 `json_to_event` 退化为通用 `Event`，`nonebot.on_message` matcher（要求 `MessageEvent`）不会执行，因此消息从未进入 outbox。只有 adapter 自己的 AdapterStatusChanged 状态事件到达 Runtime，自然没有匹配 `event.message.received` 订阅。
2. **adapter mapper 将平台 OneBot `self_id` 与 Runtime account UUID 比较**：真实 OneBot 的 `self_id` 是平台 bot id（如 `10001`），Runtime account id 是 UUID；`_map_message/_map_request` 直接 `raw['self_id'] != account_id` 会拒绝每个真实平台消息（PERMISSION_DENIED）。P07d 之前只加了 `ONEBOT_SELF_ID` 供 NoneBot bot 查找，未接入 mapper 校验。

Runtime 的 fan-out 实现（已连接 plugin profile 的 manifest capabilities → `event_subscription(payload)` → `delivery_targets`）在事件真正到达后工作正常；问题在于事件根本没有以 MessageReceived 入队/入 Runtime。

## 修复

- `adapters/nonebot/src/kiarabot_adapter_nonebot/mapper.py`：`map_message/map_request` 新增可选 `platform_self_id`；仅用于校验 OneBot `self_id`，默认回退 `account_id`（保持既有 API/测试兼容）；envelope `source.account_id` 和 conversation account 仍为 Runtime UUID。
- `adapters/nonebot/src/kiarabot_adapter_nonebot/__main__.py`：matcher/request handler 将已配置的 `platform_self_id`（`ONEBOT_SELF_ID`，缺省 account UUID）传入 mapper。
- `tests/architecture/p07_adapter_e2e.py`：fake OneBot 消息 `message_id` 改为整数；在真实 ACK 后断言 MessageReceived 事件、订阅 plugin 的 delivery_target、SDK plugin on_event 与终态 ACK（`delivery_targets.state=3`）；增加 subscription matrix：无订阅能力不产生 target、插件离线不伪造 target、重新订阅后新事件 fan-out；重复帧断言使用同一原始帧以真正验证去重。
- `infrastructure/postgres/CMakeLists.txt`：`p07_adapter_e2e` 增加 `p06` label（真 TLS/PG + 真 adapter/SDK 组件测试）。

## 验证（本机实际运行）

- 修改后的 `tests/architecture/p07_adapter_e2e.py`（main build 的真实 Runtime + PG16.4 + TLS + 真实 NoneBot adapter/fake OneBot + 真实 SDK plugin）**PASS**，覆盖：允许订阅 fan-out + lease/ACK 终态、重复帧去重、无订阅不投递、离线插件不伪造 target、重连后新事件投递。
- adapter 单元/进程测试：`pytest adapters/nonebot/tests` **110 passed**。
- main build 回归：`ctest -R 'p06|p07|p05|p08|p03_postgres' -j1` **20/20 passed**（C++ 无改动）。
- C++ 源码未修改，因此未重跑全量 C++ 构建；格式检查无 C++ diff。

## 未完成/说明

- P07d 原有 Action/平台响应矩阵未改变；事件 fan-out 现在在 P07d e2e 中真实断言。
- 离线插件的语义是“离线时事件不产生 target；重连后只投递重连后新收到的事件”，不是离线补投；与 P06 fan-out 的当前快照语义一致，已在测试中覆盖。
- 无 proto/契约修改，根因是 adapter 输入校验与 e2e 帧类型，不是契约缺口。
