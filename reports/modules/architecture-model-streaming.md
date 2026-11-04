# STR09 model streaming 受管插件整链验收设计（2026-11-04）

状态：**fixture 骨架与 GATE 设计已落盘；整链尚未执行**（依赖 STR07 Runtime/PluginBridge
streaming 生产面与 STR06 Model RPC）。本文只描述验收夹具、gate 和已实测的离线切片，不把骨架
或 SDK 单测当作整链证据，也不翻转 `host.model.stream.v1` / registry 状态。

- 驱动：`tests/architecture/p11_streaming_managed_model_e2e.py`（新，未注册 CTest 前不运行整链模式）
- 依赖切片：STR08 SDK `ModelStream` 已完成并通过 48 项 SDK 测试；STR06 Model ChatStream RPC 由
  streaming worker 验收；STR07 Runtime Model 客户端/异步 PluginBridge streaming 正在实现。
- 运行限制：无真实 QQ、无付费 Provider、无外部 DSN；只使用 owned PG16.4/TLS 夹具与本地 HTTP
  SSE fixture。

## 1. 链路与所有权

```text
managed kiarabot-plugin-host
  └─ 受管 SDK 子进程（真实 kiarabot_sdk.ModelStream）
       └─ TLS PluginBridge（HostRequest.model_stream_start/cancel + CoreToPlugin.model_stream_event）
            └─ kiarabot-runtime（STR07：RuntimeModelClient + Bridge 有序流）
                 └─ mTLS ModelService.ChatStream（STR06）
                      └─ kiarabot-model（PG16.4 持久 intent/txn2）
                           └─ 本地 HTTP OpenAI-compatible SSE fixture（本文件内嵌）
```

所有进程、PG 库、PKI、bootstrap/enrollment、sealed 56s 父委派与受管插件启动都复用合并后的
P05b-4/P11h owned fixture 机制（`identity_bootstrap_test`、`runtime_identity_e2e_test`、
`p14_native_single_host`、`p05b_model_chain_e2e` 的只读 helper），不引入 fake Host/Bridge。

## 2. 因果 gate：首 delta 早于 Provider 完成

不能只比较最终拼接文本。Provider fixture 的实现：

1. 收到真实 HTTP POST 后记录一条 `request` 状态行（含 `auth_ok`/`stream`/`mode`），回 200
   `text/event-stream`。
2. 立即写 role chunk 和首段 content（`P11 stream first`），然后阻塞在
   `threading.Event gate` 上（45s 上限），**不写后续 chunk、finish 或 `[DONE]`**。
3. 测试侧在 SDK 插件 journal 里看到 `delta #1` 后，断言 provider 状态文件仍无
   `kind=completed`，然后释放 gate。
4. 释放后 fixture 写剩余文本、finish chunk（可选 usage chunk）和 `[DONE]`，并追加
   `kind=completed` 状态行。

因此“SDK 在 Provider 完成前收到第一个 delta”是因果可证事实，而不是时间近似。Provider POST
计数用状态文件逐行统计；同一 logical id 的 replay/重试断言 POST 不增加。

## 3. 受管 SDK 插件（真实 `kiarabot_sdk`）

managed plugin-host 按 P08 流程启动真实 Python SDK 子进程（bootstrap credential 0600 文件注入、
TLS server-verified、heartbeat/refresh/ACK 全部真实）。插件 `on_start` 后轮询测试命令文件，
每条命令一个 asyncio task，向结果 journal 追加 JSON 行（begin/started/delta/cancel_receipt/
terminal/error/chat_result）。命令种类：

| kind | 行为 |
| --- | --- |
| `stream` | `async with context.model_stream(...)`，逐条记录 delta 与 terminal/error |
| `cancel` | 首 delta 后调用 `stream.cancel()`，记录 receipt，继续消费 terminal |
| `no_read` | 进入 stream 后故意不读（慢消费者），用于 SDK 本地队列溢出路径 |
| `chat` | 旧非流式 `model_chat` 回归（同一链、同一 Provider） |

插件结果通过独立 journal 观察，driver 不注入任何 SDK 内部状态。

## 4. 矩阵与状态

| # | 场景 | 期望 gate | 当前状态 |
| --- | --- | --- | --- |
| 1 | success | 首 delta 早于 Provider 完成；terminal COMPLETED；usage 权威；PG intent state/usage 落库；POST=1 | 已实现（待 STR07 执行） |
| 2 | empty | COMPLETED、零 delta、partial=false、POST=1 | 已实现 |
| 3 | usage missing | COMPLETED、usage.known=false 且计数 0（0 不是免费）、may_have_billed=true | 已实现 |
| 4 | length 截断 | TRUNCATED、partial=true、文本保留、POST=1 | 已实现 |
| 5 | 半流失败（半关连接） | 无 terminal → SDK unknown delivery；保留本地 partial；不伪造 COMPLETED；POST=1 | 已实现 |
| 6 | provider 429/500/400 | 稳定码（7/9/1），不因错误 body 变成 COMPLETED；POST=1 | 已实现 |
| 7 | 显式 cancel | typed receipt（1/2/3）；terminal∈{CANCELLED,FAILED} 且非 COMPLETED；POST=1 | 已实现 |
| 8 | deadline（Provider 被 gate 卡住 + 短 HostRequest deadline） | terminal∈{CANCELLED,FAILED}，非 COMPLETED；POST=1 | 已实现 |
| 9 | 慢消费者/队满 | SDK 本地 unknown delivery；一次 best-effort cancel；之后 unary 回归仍成功（reader 未被卡死）；POST 有界 | 已实现 |
| 10 | unary 回归 | 同链 `model_chat` 成功、stream=false、usage 正确 | 已实现 |
| 11 | replay（原 logical id） | started(REPLAY_*)+terminal(REPLAY)、零 delta、无正文重放、Provider POST 不增加 | 已实现 |
| 12 | 同 logical id 并发 | 一方 FRESH、另一方 replay；replay 零 delta；Provider POST=1 | 已实现（待 STR07 执行） |
| 13 | provider 401/404 | 401→UPSTREAM_UNAVAILABLE(9)、404→CAPABILITY_NOT_SUPPORTED(6)；POST=1 | 已实现（待 STR07 执行） |
| 14 | Bridge 断连重连 | 在途流 unknown delivery；Runtime 重启后插件重连、新命令可用；不自动重建旧流 | 已实现（待 STR07 执行） |
| 15 | Model 中途重启 | 在途流 unknown delivery；Model 重启后新命令可用；不伪造 COMPLETED | 已实现（待 STR07 执行） |
| 16 | 撤权 / Identity 不可达 | Identity 不可达 in-flight 保守收尾；删除 sealed parent 后新调用 fail-closed 且 Provider POST 不增；恢复后新命令可用 | 已实现（in-flight 凭据撤销缺短 TTL 夹具，待补） |
| 17 | txn2 提交失败/未知 | 真实 PG trigger 注入 txn2 UPDATE 失败：不伪造 COMPLETED、partial/may_have_billed 保守、PG state 不为 Succeeded、Provider POST=1 | 已实现（待 STR07 执行） |
| 18 | 跨实例隔离 | 第二注册实例（fixture SQL 等价 seed + 真实 TLS PluginBridge wire）收不到第一实例 stream 事件；未授权 cancel 返回 NOT_ACTIVE(3)；第一实例流仍 COMPLETED | 已实现（待 STR07 执行） |

矩阵 18 项均有 driver 实现，当前唯一未执行原因是 STR07 生产面未就绪；driver 打印
`STR09 pending gates: none`，失败注入方式：txn2 用 owned PG 上的 BEFORE UPDATE trigger（只作用于
目标 logical_call_id，测试后 DROP，不改产品代码）；Bridge/Model/Identity 中断通过停止真实 owned
进程、断言不进 COMPLETED、再重启恢复验证；in-flight 撤销通过把 instance principal 置 Disabled
（Identity 在线检查拒绝），恢复后重新启用；跨实例隔离通过 fixture SQL（与 owned seed 语义一致）
注册第二实例，再用真实 TLS PluginBridge raw wire 注册、对其发未授权 cancel 并观察无事件泄漏。

## 5. 已实测的离线切片

`--self-check` 不启动任何产品进程，只真实验证内嵌 SSE Provider fixture 的 HTTP 行为与 gate：

```bash
sdk/python/.venv/bin/python tests/architecture/p11_streaming_managed_model_e2e.py --self-check
# P11 streaming provider fixture self-check passed, exit 0
# log: build/logs/capability-expansion/sdk-str09-provider-selfcheck-second.log
```

覆盖：200 SSE header、首段 chunk 后 gate 阻塞（completed 未置位）、释放后 tail+`[DONE]`、
request/completed 计数、unary 回归、429/500/400/401/404/invalid_json 状态。首轮 self-check 因使用
`HTTPResponse.read(200)` 在 gate 前阻塞而失败（日志
`build/logs/capability-expansion/sdk-str09-provider-selfcheck-first.log` 保留）；修复为增量
`read1` 后通过，未放宽任何断言。

## 6. CTest 注册需求（交 lead）

拟新增测试名（`p11_streaming_*` 族）；注册片段已交 lead（见消息/本文件第 6 节）：

1. `p11_streaming_provider_fixture`：`"${KIARABOT_TARGET_PYTHON}"
   tests/architecture/p11_streaming_managed_model_e2e.py --self-check`，LABELS
   `p11;streaming;unit;provider`，TIMEOUT 60。该测试今日即可运行。
2. `p11_streaming_managed_model_e2e`：owned fixture 包装，参数顺序与 P11h 完全相同
   （identity-bootstrap、PG_BIN、identity、runtime、model、runtime-bridge-seed、
   p05b-model-seed、plugin-host），LABELS `p11;streaming;integration;security;fault`，
   TIMEOUT 600，WORKING_DIRECTORY `${CMAKE_SOURCE_DIR}`，ENVIRONMENT
   `PATH=${RUNTIME_E2E_OPENSSL_BIN_DIR}:${RUNTIME_E2E_PYTHON_BIN_DIR}:$ENV{PATH};PYTHONPATH=${CMAKE_SOURCE_DIR}/sdk/python/build/generated`。

环境/前置要求：`sdk/python/.venv` 存在；`sdk/python/build/generated` 为当前 proto 生成；
真实编译产物 `kiarabot-identity-bootstrap`、`kiarabot-identity`、`kiarabot-runtime`、
`kiarabot-model`、`kiarabot_postgres_runtime_bridge_seed`、`kiarabot_p05b_model_seed`、
`kiarabot-plugin-host` 均存在（与 P11h 相同）；无需新增 C++ 目标。CTest 注册行由 lead 统一写入
`cmake/platform.cmake`；在 streaming worker 宣布 STR07 可用前不要注册/启用集成测试（避免红）。

## 7. 已知限制与风险

- 整链矩阵尚未执行：依赖 STR07 的真实事件流、每连接/进程上限、取消传播和 Bridge 生命周期。
- txn2 失败与跨实例隔离需要夹具扩展（确定性故障注入、第二 plugin instance seed），不能用
  mock/空流代替；已列入 pending gate。
- Provider fixture 是本地真实 HTTP，不是外部 Provider；secret 只出现在请求头，断言 PG 记录
  不含 provider secret。
- 日志/报告不得包含 prompt/completion 全文或秘密；driver 只打印有界诊断。
