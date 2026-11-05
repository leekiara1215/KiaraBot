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

## 8. 首次真实整链执行记录（2026-11-04，checkpoint 7b092da）

`build/.agents/kb ctest -R '^p11_streaming_managed_model_e2e$' --no-tests=error` 首轮 exit 8/1（driver exit 1），
CTest 输出重定向日志 `build/logs/final-acceptance/str09-round1.log`，driver 未脱敏输出保留于
`build/logs/final-acceptance/str09-round1-driver.out` 及 `/tmp/str09-driver.out` 复现包装（`/tmp/str09_repro.py`：
用 CTest json-v1 的 command/env 替换 `--test` 为 stderr 捕获 wrapper，不改产品与测试语义）。

定位并已修复的 **fixture 缺陷**：

1. model readiness 判断错误。真实 owned service 启动行是
   `service=model probe_port=... grpc_port=... bridge_port=0 ready=false`；依赖收敛前 `ready=false`
   是正常状态，HTTP probe 才是最终就绪权威（与 P05b 一致）。原 driver 要求 `ready==true` 导致
   `model readiness` 失败。现只校验 `ready` token 存在并等待 `/health/ready` probe。
2. 插件 journal 缺口：subject 在 `context.model_stream()` 抛本地错误时异常发生在 try 之外，
   probe 只看到 "无事件"。现已包入 try 并写 `error` journal，probe 能精确报告失败码。

定位到的 **产品侧缺口（非本文件范围）**：managed SDK 插件注册后
`context.model_stream()` 本地抛 `CAPABILITY_NOT_SUPPORTED`（journal code=6，probe 报
"feature not enabled at registration"）。根因是 Runtime Bridge 的 trusted feature catalog
（`core/app/runtime_composition.cpp` 创建 `make_runtime_bridge_application` 处）仍为
`{"plugin.config.v1","action.result.v1","host.tasks.v1","host.model.v1"}`，缺
`host.model.stream.v1`；registration 的 `enabled_features` 只取请求 ∩ catalog，故生产
`kiarabot-runtime` 永远不会 advertise。建议最小修：在 `config.model_endpoint` 存在时把
`host.model.stream.v1` 加入该 vector 后传入。该修复属于 STR07 owner 的允许文件，需 lead
开窗/指派；修复并重建 Runtime 后本 driver 才能进入 18 项矩阵实际执行。

已确认的 fixture 行为（在本轮可通过的真实阶段）：identity bootstrap、runtime seed、Identity
daemon readiness、Model daemon 启动行、admin login、model seed、instance principal 注册、
sealed 56s parent 写入、managed plugin-host 启动与 SDK 插件注册（注册本身成功）。

## 9. 冻结过程与最终收口注意（2026-11-04）

- reconnect gate 契约（经 host/streaming 复核）：Runtime 优雅 SIGTERM 会向已连 Bridge 发
  ShutdownNotice，SDK 按冻结契约 drain 且不自动重连，这是预期；恢复必须经真实 managed Host
  路径的新 apply（新 execution_generation）并等待新的 plugin-ready/Fence，然后再进行 unary。
  fixture 已改为：断言 2s 内无自动恢复事件 → `execution_generation=2, desired_version=2` 的
  managed 命令 → 等 plugin-ready 新 mtime（60s）→ retry unary。SIGKILL 自动重连属另一契约路径，
  本轮未加用例以控制 180s wrapper 预算。
- 失败证据保留：matrix/reconnect/after-bootstrap 失败时，`preserve_fixture_artifacts()` 把
  host.err、plugin.log、plugin-results/commands.jsonl、runtime/model/identity err、
  host.sqlite/json、sealed envelope、provider 记录复制到
  `build/logs/final-acceptance/str09-artifacts/<label>-<UTC>/`；重启 Host 前的 host.err/plugin.log
  另存为 host-first.err/plugin-first.log，避免 `wb` 重开丢失第一段证据。
- 待核产品项（host 提出，非本 fixture）：Runtime bridge `register_plugin` 对
  `instance.host.has_value()` 直接 PERMISSION_DENIED 与“受管实例用 Host 下发的实例级可撤销
  bootstrap credential 重连”契约不一致；若 STR09 gen2 重启后插件重注册被拒，应先核该项。
- 运行预算：run_database_test 内层对 driver 有 180s 超时；矩阵恢复/重试路径已按有界预算收紧，
  保留 stage 计时行（`t=...s`）便于定位慢 gate。

## 10. STR09 最终验收结果（2026-11-04，冻结后）

- 最终命令：`build/.agents/kb ctest -R '^p11_streaming_managed_model_e2e$' --no-tests=error`
  → **exit 0**，121.51s（日志 `build/logs/final-acceptance/str09-final-pass.log`）。
- 直接 driver 同轮证据：`build/logs/final-acceptance/str09-final14-driver.log`（exit 0，
  全 stage 计时、`STR09 pending gates: none`）。
- 18 项矩阵全部真实执行通过（真实 owned PG16.4/TLS/Identity/Runtime/Model/plugin-host/本地
  HTTP SSE Provider）：success 因果 gate（首个 delta 早于 Provider 完成，Provider POST 计数与
  持久 model_intents 事实）、empty、usage-missing（usage_known=false/tokens=0/may_have_billed）、
  length-truncated、half-close unknown delivery+partial、provider 429/500/400/401/404 稳定码、
  显式 cancel receipt+非 COMPLETED、deadline、慢消费者队满、同 logical id 并发、
  跨模式 replay（零 delta、无 Provider 重调）、txn2 注入失败、sealed-parent 撤权 fail-closed、
  Identity 不可达 in-flight、in-flight instance 撤销、Bridge/Runtime 优雅断连（不自动恢复）
  + 新 execution_generation 的受管恢复、Model 中途重启、跨实例隔离（真实 TLS PluginBridge
  raw wire + NOT_ACTIVE + 无事件泄漏）。
- 首轮/中途失败与归属（全部保留日志 str09-round*/repro*/final*）：
  fixture：model ready=false 误判、插件本地异常未写 journal、SSE usage chunk 形状、half_close
  模式漏配、reconnect 恢复序列（gen2/fresh ready/不自动恢复）、若干委派窗口与高负载时序
  断言的受限重试；产品侧（对应 owner 已修）：Runtime bridge feature catalog 缺
  host.model.stream.v1、stream start 未派生 installation、PG CHECK 拒绝 domain error 0、
  Runtime→Model exchange 子委派寿命被 2s 网络期限封顶、audit→log feed 资源种类校验导致
  identity-bootstrap INVALID_ARGUMENT、受管实例 register_plugin host-binding 误拒。
- 无待补/未覆盖 gate；未使用 FakeHost、空流或 mock 切片代替真实链。

## 11. task-30：全量窗口 flake 诊断与有界窗口加固（2026-11-05）

- 证据：`build/logs/final-acceptance/str09-artifacts/after-bootstrap-failure-20261105T062137Z/`
  （全量窗口失败轮）。plugin journal 末段：identity-in-flight gate 的首个 stream 在 begin 后
  立即收到 `connection_closed`（Bridge 瞬时关闭），随后两次重试均为无 cause 的
  UPSTREAM_UNAVAILABLE——SDK 正处于 shrink 重连窗口，`stream_gate` 3 次尝试耗尽后 gate 红。
  同轮后续又暴露：fence 轮换（authorization refresh -> Fence 轮转会取消在途流）产生的
  partial=CANCELLED 被 success/replay/txn2/concurrency/provider gate 当作结果；以及高负载下
  Model/Runtime 重启启动行 20s 读取超时/就绪探针失败。
- 结论：**fixture/driver 时序类，不是产品 bug**（Bridge 行为与冻结契约一致：重连窗口、
  refresh 轮换取消在途流、优雅关机不自动恢复）。
- 加固（仅 `tests/architecture/p11_streaming_managed_model_e2e.py`）：
  1) 新增插件 `ping` 命令（不发 Provider 调用）+ `wait_control_plane()`：窗口重试前等待 SDK
     重新注册出真实 Bridge 会话，替代盲目单次重试；
  2) 有界 backoff（1-3s）与每 gate 有限 attempts；`stream_gate` 返回该 attempt 的
     Provider POST 基线，所有 POST 计数断言改为按成功 attempt 计数（避免重试污染）；
  3) `fence_rotation_cancel()`：partial CANCELLED（无消费者 cancel）视为可重试窗口，用于
     success/batch/half-close/slow-consumer/replay-source/txn2/concurrency/provider-map gate；
  4) 重启恢复：Model/Runtime 启动行读取与就绪探针在有界窗口内重试（30s/attempt，最多 2 次，
     失败时保留 stderr/exit 证据），避免高负载把正常慢启动误判为失败；
  5) 首轮 attempt/journal 始终打印保留，产品期望/断言未放宽。
- 稳定性证明（机器由 lead 清空负载后）：
  - 连续 5 次隔离 `build/.agents/kb ctest -R '^p11_streaming_managed_model_e2e$'` 全绿：
    `str09-t30f-run1..5.log`（127.17/127.92/131.53/124.86s，run1 Passed；exit 0）；
  - 并发负载窗口 `ctest -j 3 -R '^(p11_streaming_managed_model_e2e|p11h_managed_model_e2e|p05b_model_chain_e2e)$'`
    → 3/3 通过，STR09 131.86s（`str09-t30-parallel3.log`，exit 0）；
  - 最后加固后的确认单跑：`str09-t30-final.log` Passed 124.72s（exit 0）。
  - 加固前失败轮次日志保留：`str09-t30-run2/3/5.log`、`str09-t30b-run1..5.log`、
    `str09-t30c-run2/3.log`、`str09-t30d-run4.log`、`str09-t30e-run4.log` 及对应 artifacts。
