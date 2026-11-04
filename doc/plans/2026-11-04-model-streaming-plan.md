# Model streaming 可实施子计划（2026-11-04）

## 1. 授权、范围和本轮产物

用户已把模型 streaming 纳入计划，并要求主代理统筹、subagents 执行。授权来源见 [用户决策注册](user-decisions-2026-11-04.md)。历史 P11 的“streaming 明确不做”只说明当时范围；新授权没有使现有实现或 target 状态自动变成已完成。

本轮只新增本报告，不修改产品代码、共享 proto/API、数据库、依赖锁或保护清单。`doc/18-architecture-baseline.md` 只读。工作区已有未提交重构保持原样；本报告不是这些差异的新验收证据。主代理将计划汇入 `doc/29-approved-capability-expansion-plan.md`，随后按依赖分派实现。

实施目标是 **Provider HTTP SSE → Model 应用/RPC → Runtime Model 客户端/应用 → PluginBridge → Python SDK** 的纯文本增量闭环，保留现有 `Chat`、`HostRequest.model_chat` 和 `Context.model_chat(..., stream=False)`。首批不做工具调用、多模态、正文持久缓存、断点续传、跨主机接管或独立断网演练；不进行付费 Provider 请求或真实 QQ 主动消息。

网页展示由 Control WS 工作流拥有；本工作流交付流事件的语义和边界接口依赖，不修改 Panel App/stream 文件。QQ 默认完整聚合、成功后走现有 Action 一次发送；不逐 token 发消息，不自动发送半截回答。

## 2. 当前实现和证据边界

已阅读根 `AGENTS.md`、`doc/12-cpp-coding-standard.md`、`doc/00-boundaries.md`、`doc/02-core.md`、`doc/03-plugin-sdk.md`、`doc/05-infrastructure.md`、`doc/11-toolchain.md`、`doc/27-phase-exit-evidence.md` 第 0 节，以及 `reports/modules/architecture-p11-model.md` 和直接相关协议、源码、测试入口。最新状态以 doc/27 第 0 节为准，不能用旧报告的中间状态覆盖后续 P11h 正链证据。

源码核实结果：

- `infrastructure/src/model_http_provider.cpp:236` 附近固定 `stream=false`，libcurl 将整个响应收进 `std::string` 后解析 `choices[0].message.content`；目标实现是此文件，`http_model_provider.cpp` 为历史路径。
- `core/include/kiarabot/core/ports/target/model_ports.hpp` 只有 `ModelProviderPort::chat`；`domain/target/model_gateway.hpp` 定义 128 条消息、64 KiB 请求/文本、1 MiB Provider 响应、30 秒最大调用和最多 64 的单模型并发配置。
- `core/src/application/target/model_gateway_application.cpp:226` 起：按 `(owner_instance, logical_call_id)` 与服务端 request hash 防重，txn1 提交 STARTED，事务外调 Provider，txn2 CAS 保存结果。正文不持久、不重放。现有 maintenance 处理 UNKNOWN 保留/处置，没有 STARTED 超期恢复扫描。
- `proto/kiarabot/v1/model.proto` 的 `ModelService.Chat` 是 unary；`plugins.proto` 的 `HostRequest`/`HostResponse` 是一问一答，没有已实现的增量/取消协议。
- `runtime_model_client.cpp:88` 起只调用 `stub_->Chat`；无可恢复正文时返回 `UPSTREAM_UNAVAILABLE / UNKNOWN / QUERY_SAME_INTENT`。当前 gRPC 错误路径按 status code 映射，新增流路径须读取结构化 `RpcFailure`，不能继续丢掉 acceptance/recovery。
- `runtime_bridge.cpp:1911` 起 `handle_model` 同步调用 Model；`runtime_plugin_capability.cpp:54` 起应用层完成 Model 权限、可信实例/安装派生。流式不能把业务准入移回 transport，也不能在这个同步分支中阻塞整个 Bridge 的 heartbeat/refresh/cancel 读取。
- `sdk/python/src/kiarabot_sdk/runtime.py:111` 起遇 `stream=True` 明确拒绝；`_Rpc` 持有最多 64 个 pending Future，reader 一次 `host_response` 即完成 Future。需要独立流 registry，不能重复完成同一个 Future。
- `model_identity_client.cpp` 对 Runtime-origin 方法有精确白名单；新增 `ChatStream` 必须明确加入允许集并测试，不能只新增 RPC 后假定委派天然可用。现有 GetCallStatus 的管理/Runtime caller 限制不能随流式扩宽。

本地真实 HTTP fixture、真实 PG/TLS 与受管插件的非流式 P11h 链已有通过记录；本报告不重跑、不把它作为 streaming 实测。

### 图查询使用与限制

使用 `codebase-memory` skill 的 Verify 工作流。project 为 `Users-betta-Workspace-KiaraBot`，generation `2026-11-04T05:19:50Z`，ready，37 partial/55 excluded。已执行 `list_projects`、`index_status`、schema、精确文件限定 search、Provider snippet、Runtime client `trace_path(direction=both, depth=1)` 和对 12 个证据路径/3 个 scope 的 coverage（分页无剩余）。Provider chat 只有 1 个命中，Runtime client 文件 12 个符号，均无未读分页。

coverage 确认应用层 `core/src/application/target/` excluded，已回读 Model/Bridge/capability 应用源码；SDK 精确 `src/kiarabot_sdk/{runtime,transport}.py` 为 metadata_match，scope 的已知 gap 是 `__pycache__`，不沿用父级泛 scope 的 not_tracked 结论。Provider、Runtime client、Bridge、Model RPC、model_store 和两个 proto 精确路径无已记录问题；这只是 best effort。

图把 `ModelRpcImpl.Chat` 关联成 `RuntimeModelClient.chat` caller，并给出明显跨 reference/crypto 的同名解析边，因此**不把该 trace 当成已验证调用链**。本报告的现状链来自上述接口及调用点源码核对；不声称全仓完整影响或不存在其他调用者。协议 policy 的 partial 范围不用于权限缺失的否定结论。

## 3. 推荐的最小契约

先由 STR-01 契约 owner 冻结以下语义，名称是拟议，字段号须从实际 descriptor 分配，不能由多个 worker 独立挑选。

### 3.1 Model owner RPC 与内部端口

新增 `ModelService.ChatStream(ModelGatewayChatRequest) returns (stream ModelStreamEvent)`，保留原 Chat 的签名、默认行为、字段号及权限。复用 `PERMISSION_MODEL_CHAT`、Model resource scope、online authorization、30 秒默认/最大 deadline；不新增全局权限或让 Control 直接冒充插件 owner。

`ModelStreamEvent` 的唯一来源放 `plugins.proto` 的纯跨语言模型消息区，再由 `model.proto` 引用，避免当前 `model.proto → plugins.proto` import 关系产生循环。共享 stream terminal 中的调用快照需要 owner、model、config/secret version、OperationState、ExecutionOutcome、usage、may_have_billed、reason、disposition；先定义不依赖 `ModelCallRecord` 的流专用快照，字段语义与原投影一致。不要复制 C++ 私有类或修改旧 ModelCallRecord 类型。若契约 owner选择将共享类型提取到新的纯 model-types proto，必须统一拥有 import/生成变化，不能在 worker 内局部搬动冻结类型。

每个流事件携带原 attempt `request_id`、稳定 `logical_call_id`、`sequence`，body 为以下 oneof：

1. `started`：fresh确认受理或replay时恰好一次，sequence=0。携带 `FRESH_RESULT / REPLAY_IN_PROGRESS / REPLAY_TERMINAL` disposition 和调用快照。fresh 必须在 txn1 确认提交后才发；started 不是 Provider 已生成文字的承诺。pre-start拒绝或txn1提交未知可以没有started，且绝无delta。
2. `delta`：sequence 从 1 连续递增，每帧非空 UTF-8 `text_delta`，只做追加。不得把 Provider 原始 chunk/JSON/secret/错误正文交给插件。
3. `terminal`：正常可写连接中恰好一次，sequence=最后 delta sequence+1，携带 `termination`、`partial`、`emitted_bytes`、`emitted_delta_count`、`finish_reason`、最终 usage/billing 和调用快照/结构化错误；之后禁止再发 delta。

没有started的拒绝terminal使用sequence=0、emitted=0，可不带call快照，必须有稳定错误和failure context。Model RPC pre-start返回的结构化gRPC失败由Runtime转换为这一Bridge terminal；SDK据此结束start等待。不得把受理前拒绝塞进等待永久不完成的Future。

拟议 `termination` 的封闭集合：`COMPLETED`（完整文本正常结束）、`TRUNCATED`（例如上游明确 length 截止，协议完成但文本不完整）、`FAILED`、`CANCELLED`、`REPLAY`。它表达流的结束原因，**不增加或重定义**全局 `OperationState`/`ErrorCode`。`partial` 表达“本次 attempt 已产生非空文本但没有完整正常结束”；TRUNCATED 必为不完整，文本为零时 partial=false。`emitted_*` 是 Model 已交给下游的数量，不承诺 SDK/UI 已消费。Runtime 与 SDK 另记录已收到数量，不冒充持久事实。

fresh 完整成功须满足：合法 Provider 终止边界、可接受 finish_reason、结果界限通过、txn2 已确认提交。SDK 应同时收到所有连续 delta 和 terminal 后才判定完整成功。仅有 `finish_reason`、HTTP EOF、gRPC OK 或最后一个可见 token 都不能伪造完整成功。TRUNCATED 可保存 Provider `KnownSucceeded` 事实和真实 usage/billing，消费者仍不得当完整回答发送。

应用内新增非 wire 的 `ModelStreamSink`/取消端口；sink 接收拥有其内容的值或明确同步借用，返回 Continue/Stop，回调异常转换稳定错误。`ModelProviderPort` 新增流式方法/专门可选子端口，`RuntimeModelPort` 同样提供 stream 操作和 cancellation handle。保持旧 fake/非流实现兼容，未实现流式的实现返回 `CAPABILITY_NOT_SUPPORTED`，不能默认调用 Provider 再假流式分片。

### 3.2 PluginBridge 和 SDK

新增独立协商能力 `host.model.stream.v1`，仍需 `model.chat`。只有所有生产环节可用时 advertise；不把 `host.model.v1` 的存在当成 streaming 已支持。

- `HostRequest` 新增 `model_stream_start` 和 `model_stream_cancel` operation（拟用空闲 18/19，最终由契约 owner descriptor 校验）。start 内容复用 model/messages/logical_call_id；Host 的 request_id/trace_id/deadline 仍为 attempt 信息。
- `CoreToPlugin` 新增 `model_stream_event` body，而非给同一 `HostResponse` 重复填结果。它带当前 Fence 及关联 request_id/logical_call_id；started/delta/terminal 沿用同一语义。
- cancel 是独立 HostRequest，携带目标 start request_id 和 logical_call_id，不允许自报 owner；`HostResponse` 新增 cancel receipt：`CANCELLATION_REQUESTED`、`ALREADY_TERMINAL`、`NOT_ACTIVE`。该收据只确认本连接内的取消处理，不承诺上游未执行或未计费。重复 cancel 不产生新 Provider 调用；旧连接/另一插件不能取消当前流。start 终态仍由 stream terminal 给出。
- 建议新增 `Context.model_stream(...)`，返回支持 `async with`、`async for`、`aclose()/cancel()` 的 `ModelStream` 对象；显式暴露 logical_call_id、terminal 和本地 partial 文本。保持 `model_chat` 返回类型不变，旧 `stream=True` 继续提示使用新接口，避免 boolean 改变 await 返回类型。
- SDK 在发送 start 前登记 stream registry；按 request_id + logical_call_id + Fence 校验 sequence。重复/乱序/gap 不盲拼；终止该消费 attempt，标为未知交付并保留本地 partial。cancelled typed outcome/exception 保留 termination、稳定错误和 billing，不能将 asyncio cancellation 伪装为 Provider 未执行。
- shared reader 对每流队列用非阻塞投递；队满触发该流取消和本地终止，不等待慢消费者。heartbeat、refresh、Action、事件 ACK 和其他 stream 继续工作。

### 3.3 逻辑身份、重放与查询

`logical_call_id` 仍为客户端在一次新业务调用前生成并保存的 UUIDv4；request_id 是每个传输 attempt 的独立 UUID。Model 仍以可信 owner + logical_call_id 为唯一防重键，不使用连接 Fence/Runtime epoch 作为计费身份。

推荐流式和非流式共用**现有** `model_request_canonical(model,messages)`，不因输出 transport 模式改变语义指纹。相同 id/相同内容跨 Chat/ChatStream 只重放元数据、Provider 请求总数最多一次；同 id/异内容 CONFLICT。config/secret 轮换不改变旧调用 pinned 版本。旧 alias/UUID 的现有规范化行为保持并测试，不借 streaming 重定义旧 hash。

重复 start 返回 started(replay)+terminal(REPLAY)，零 delta，不加入原流、不回放正文、不重新请求 Provider。Replay 终态携带原 call 快照，即使原 call Succeeded，也不代表重放方拿到了回答。SDK 提供“按原请求和原 id 获取 replay 状态”的显式操作，或消费 `model_stream` 的 REPLAY 终态；不暗中生成新 id。

这条重放路径足够给插件提供 owner 状态观察，不新增插件 GetCallStatus 权限/RPC。现有管理 GetCallStatus 保持其授权边界；若后续要专门的插件状态查询，应另冻结 owner-only inspect 协议，不能把 Control 的管理查询直接暴露。重连只用原身份请求元数据，SSE `id`/`Last-Event-ID` 不能触发 Provider POST 重发。

## 4. 失败、取消、partial、usage 和账单语义

流的交付事实与 Provider 执行事实独立。用户停止显示、Bridge 断开或 SDK 丢 terminal，都不能推翻 Model 已持久化的成功。推荐映射如下；错误始终使用已有稳定码和结构化 acceptance/recovery，不回传上游自由文本。

- 参数/未协商能力/权限拒绝/过期且尚未提交 intent：`INVALID_ARGUMENT / CAPABILITY_NOT_SUPPORTED / PERMISSION_DENIED / UNAUTHENTICATED / TIMEOUT`，`NOT_ACCEPTED`，Provider 0 次，无 partial、无计费。
- txn1 提交结果 Unknown：不发 Provider；返回 `UPSTREAM_UNAVAILABLE / UNKNOWN / QUERY_SAME_INTENT`，只按原 identity 复核。不得因不确定是否插入而重复发起。
- txn1 已提交、Provider 尚未开始时取消：短 txn2 保存已知未执行的失败事实，usage_known=true、tokens=0、may_have_billed=false；流 termination=CANCELLED，receipt 不替代终态。可用既有 `UPSTREAM_UNAVAILABLE` 原因加独立 CANCELLED 结束原因，无需新增全局 CANCELLED 错误码。
- Provider 开始后的取消、deadline、连接/TLS中断、5xx、畸形/超大 SSE、缺 `[DONE]`、写队列超时：在没有可信完整结果时记 `ModelCallState::Unknown / ExecutionOutcome::Unknown`，may_have_billed=true，usage_known=false（除非已收到并验证完整权威 usage），reason 用 TIMEOUT 或 UPSTREAM_UNAVAILABLE。已发 delta 的 attempt partial=true；0 delta 不等于未执行。
- 明确 Provider HTTP 400/422→INVALID_ARGUMENT，401/403→UPSTREAM_UNAVAILABLE，404/405/410→CAPABILITY_NOT_SUPPORTED，429→RATE_LIMITED（bounded Retry-After），其他已知 4xx 维持现有规则：已知拒绝、may_have_billed=false。若已经收到合法文本后出现 SSE error，则不能套用“HTTP 4xx 未计费”的默认 mutation，按未知计费处理。
- HTTP 200 后发现 tools/nontext/不支持的 finish_reason：不重发非流式请求；已发文字保留 partial，返回现有 CAPABILITY_NOT_SUPPORTED 或 UPSTREAM_UNAVAILABLE 并保守 UNKNOWN/billed-possible。不要复用现有 `model_definite_failure_mutation` 将这种情况错误写成未计费。
- 正常完整或明确 length 截断：写 txn2 后给 COMPLETED/TRUNCATED；usage 来自最终权威 chunk。没有 usage 时 usage_known=false、tokens=0，0 不是免费。账单可能已发生，may_have_billed=true。
- Provider 已完整成功，随后 txn2 失败/提交 Unknown：不能发 COMPLETED；返回 `UPSTREAM_UNAVAILABLE / UNKNOWN / QUERY_SAME_INTENT`。txn2 需保留失败的持久上下文，不回滚成“未执行”。
- Model 已保存 Succeeded，但下游断开：保持持久成功；SDK 只知道当前交付中断，重放可读到执行成功但拿不到正文。不能为补正文再开新 id。

Model RPC 在未开始事件流之前的拒绝继续使用现有 `RpcFailure` trailers + gRPC status；进入事件流后的已知终结使用 typed terminal + gRPC OK。硬断连只有 gRPC transport status、没有 terminal，消费者按未知交付。不得同时把 terminal 成功与非 OK 状态作为两份互相冲突的权威结论。

跨边界码沿用 `target_codec.cpp`：INVALID_ARGUMENT→gRPC INVALID_ARGUMENT/HTTP400；UNAUTHENTICATED→401；PERMISSION_DENIED→403；NOT_FOUND→404；CONFLICT→ABORTED/409；RATE_LIMITED→RESOURCE_EXHAUSTED/429；TIMEOUT→DEADLINE_EXCEEDED/504；UPSTREAM_UNAVAILABLE→UNAVAILABLE/503；CAPABILITY_NOT_SUPPORTED→UNIMPLEMENTED/503；INTERNAL→INTERNAL/500。Bridge stream terminal 与 SDK 保留相同 code/context，不只留 retryable。网页开始传输后 HTTP 状态无法重写，WS/in-band terminal 保留这些业务码，不能把它当 HTTP 200 成功。

## 5. deadline、撤权、资源所有权和背压

### 5.1 时间与授权

有效调用期限取 Host deadline、Runtime 客户端上限、Model 配置 timeout、30 秒上限及当前委派有效期的最小值；用单调时钟等待和取消，UTC 仅作线协议时间。给 txn2/terminal 留 100ms 以上明确预算；后续持久收尾另有最多 5 秒的内部预算，不延长文本输出 deadline。清理依赖不可用时保留 UNKNOWN/STARTED 待恢复，不能无限阻塞停机。

Runtime 每个 start/cancel 帧先验证当前 proof/Fence、可信注册 profile/installation 和 feature/permission；流启动 worker 捕获值快照与有生命周期的取消句柄，不捕获 `StreamState&` 或 handler 栈指针。

Model 在 fresh/replay admission 都在线检查准确方法、Model scope 和实际 Runtime peer；新增 ChatStream 的 Runtime-origin 白名单及委派映射必须成组修改。流进行中在首个输出前、最长每 1 秒、最终释放成功前核查当前授权；预算取剩余时间与既有授权上限的较小值。撤权、凭据/连接失效、委派过期或 Identity 故障时停止新 delta、取消 Provider并保守收尾；授权检查期间不继续放行新文字。该方案是有界重新校验，**不承诺瞬时撤权**，检测时延/检查预算写入模块文档和测试。

Bridge 的连接 refresh/关闭/删除 gate 在 reader/生命周期回调中取消该连接所有 active stream；Model 按每调用取消上下文中断 curl 传输。取消只是尽力停止后续工作，不撤回已经生成或计费的上游输出。COMPLETED 与 cancel 竞争由唯一终结仲裁决定；取消不能覆盖已经确认的 Model 成功事实。

### 5.2 有界内存与并发

以下为推荐首批**服务端固定上限**，由 STR-01 冻结并在边界测试，不接受请求自行放大：

- 保持 128 条/64 KiB 输入、64 KiB 累计输出、1 MiB 总 Provider 明文字节。新增单 SSE 行/事件上限 64 KiB、单 delta ≤4 KiB（在 UTF-8 边界分片）；累计输出包含所有 delta，不只看最后一帧。
- 每调用队列同时受 32 帧和 64 KiB 字节双限约束；终止控制信息保留单独槽位，不能因数据队列满丢 terminal。SSE event/line 未闭合时也计入内存上限，不能交给第三方 parser 无界累积。
- Runtime 每连接最多 4 个 active Model stream（含 admitted/closing），进程全局最多 64 个；SDK 采用相同默认值，原 unary pending 64 的限制独立保留。Model 的旧 Chat 和新 Stream 共用 per-model max_concurrency，并增加进程 global 64 上限；必须先查重放，再对 fresh 占并发，不能把重放算成新的付费工作。
- Provider enqueue 最多等待 250ms；下游持续慢时取消该流，按已执行可能性终结。不能在 curl callback 内直接执行阻塞 gRPC Write 或调用未知插件回调。
- 每个 Bridge 连接只有一个 writer，控制帧优先、stream 轮转；一次网络写最多 1 秒/剩余期限，超时取消连接。连接死亡/停机先取消工作、唤醒队列，再 join worker/writer；禁止 detached thread。共享 gRPC 流头阻塞无法靠队列完全消除，超时后断连的代价要明确测试。
- SDK reader 不能对某个慢流 `await queue.put()`；溢出该流时本地终结、发一次 cancel，并继续处理 heartbeat/refresh。消费者退出 async context 发送 best-effort cancel 并释放 registry；不把同步生成器垃圾回收当作可靠取消协议。
- 首批用饱和即拒绝的固定 worker 数，不加无界排队。必须统计队列字节、active workers、取消收尾和 UTF-8/JSON临时缓冲，验证总内存随固定上限而非 Provider 持续输出增长。

## 6. HTTP SSE 选型门槛

沿用成熟 libcurl 做 HTTP/TLS/超时/取消，保持禁代理、禁重定向、HTTPS（本地 fixture 可 loopback HTTP）和秘密只在请求头。SSE 是 WHATWG 标准格式；不能按 TCP/curl callback 块边界解析 JSON，也不能把 `\n\n` split 当作完整协议。

初始选型建议：**libcurl + 可独立喂字节的成熟 C++ SSE parser**。STR-02 先做依赖/一致性小任务，再实现 Provider。候选为 libcpr 的 `ServerSentEventParser`（可独立使用）而不是替换全部现有 HTTP 栈。应锁 reviewed release/commit、归档 hash、license、CMake/private link 和供应链来源，不能在构建时拉浮动 master，不复制第三方源码进 Core。当前锁只有 curl，不包含该新增依赖。

本次查阅上游发现候选不能直接宣称合格：截至本次读取的 libcpr 源码按 LF 扫描、CRLF尾裁剪，裸 CR、空 data 行和边界内存行为须补一致性证明；不能用“库支持 SSE”跳过验收。若冻结版本不能满足下述矩阵，STR-02 必须更换合格依赖或先上游修复并锁经过审查的版本，再合并生产 streaming。不能悄悄改成自创 Provider 行协议或在 Core 写另一套 parser。该依赖 gate 是真实待办，没有已验证版本/hash时不标完成。

WHATWG 解码要求覆盖 UTF-8/BOM、任意网络分段、CR/LF/CRLF、注释、多 `data:` 合并、空字段/空 data、忽略未知字段、事件空行边界和 EOF 不补造未完成事件；不启用 EventSource 自动重连。Provider payload 继续按 OpenAI-compatible Chat Completions 的 `choices[0].delta.content` 解码，接收 role/空 delta、合法 finish_reason、可选 usage-only chunk（choices=[]）和 `[DONE]`。n=1，工具/多模态拒绝；缺 `[DONE]` 不当成功。`include_usage` 的服务兼容性需明示，拒绝时不自动重发 POST。

依据为 [WHATWG SSE 规范](https://html.spec.whatwg.org/multipage/server-sent-events.html#parsing-an-event-stream)、[OpenAI Chat Completions 官方契约](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create)、[libcpr SSE 接口](https://github.com/libcpr/cpr/blob/master/include/cpr/sse.h) 与[实现源码](https://github.com/libcpr/cpr/blob/master/cpr/sse.cpp)。OpenAI 文档说明 usage 可在额外最终 chunk 中出现，流中断可能拿不到它；本计划因此不按本地字符估算权威 usage。以上网页用于方案核对，不是锁定供应链输入。

## 7. PostgreSQL 范围：优先无 schema 扩张，但不回避恢复

推荐首批不存 token、正文、event sequence、重连 buffer、partial 或取消控制句柄；它们只属于当前流。持久层继续写现有 `model_intents` 的 state/error/usage/billing/request_hash/config-secret版本。stream terminal 的 partial 不是新的持久字段；重放只返回执行元数据，不能在进程重启后声称知道已交付多少文字。取消 cause 也不新增长期全局生命周期状态。

仍有**必要数据库实现工作**：Model 当前没有超时 STARTED 恢复。STR-03/04 须新增有界 repository 查询 `list_expired_started(cutoff,limit)`，利用既有 created_at；并在应用层 CAS 将超过“30 秒最大执行+5 秒收尾”的 STARTED 保守转 UNKNOWN、may_have_billed=true。数据库 now 不可信/不可用时 fail closed，不凭本机时钟提前作已知失败判断。相关 worker 的执行与收尾总预算必须真的受此界限约束，created_at 必须只在 intent 初始写入，不能续写延长。

恢复与 txn2 竞争通过 expected version + 仍为 STARTED 的条件仲裁；迟到 worker 不能先读取 recovery后的 UNKNOWN 再无条件覆盖成成功/未计费。已知 Provider 结果若未能在预算内持久，留 UNKNOWN 供原有人工 disposition 流程处理，不自动重发 Provider。不使用“启动时把全部 STARTED 改 UNKNOWN”的做法误伤另一有效 worker；有界年龄判断对本地重启也适用。

该新增 SELECT/CAS 用现有字段，不应因此 bump schema/manifest，但必须增加真 PG16.4 查询/竞争/重启/descriptor不变测试，不能只在内存 fake 里验证。既有非流式也会受 STARTED 恢复边界影响，必须明确记录并回归；这是 streaming 必要收尾，不是隐性无文档内部重构。

如果实现阶段证明固定全局界限不足、需要不同持久 deadline、fencing lease、正文回放或持久 partial，则暂停“无 schema 变化”的假设，由主代理协调 Model owner-local 新字段/表，更新 UoW/codec、空库初始化及迁移策略和真 PG测试。不要把新列随意放 `schema/common.hpp`：`result_model_intents` 等共享 result 模板会影响四库 descriptor。确需改变共享模板时，必须同时更新 Identity/Runtime/Scheduler/Model 对应 schema version/descriptor/relation_count 实测、`record_codec.cpp`、结果快照兼容和文档；不能只改 Model manifest，更不能清空现有数据库绕过迁移问题。初次 schema 改动也须与 Host/Bootstrap 工作流争用的 schema/manifest 统一合并。

## 8. STR 任务、文件所有权与依赖

以下 owner 是主代理分派的职责槽位，不代表已开新 agent。所有 worker 都必须知晓“不是唯一工作者，不回滚他人改动”；CMake/proto/API/共享文档由主代理保留最终写入权。每项先协议/接口，再实现，再验收，不虚构工期。

### STR-01：契约冻结和兼容性（owner：契约 worker；第一批）

允许文件：`proto/kiarabot/v1/{plugins,model}.proto`；必要时新增 `model_stream_types.proto`；`proto/tests/{target_spec,contract_status,enum_baseline}.json`、`target_contract_test.py`、stream golden/roundtrip测试；`proto/README.md`、`doc/01-contracts.md`、`doc/01-runtime-semantics.md`。禁止修改其他模块实现及 `doc/18`。

输入为本报告第 3–7 节与主代理协调的 WS接口；输出为精确字段号、oneof、枚举、method options、feature vocabulary、typed error/context、取消和重放 oracle。依赖无产品实现。验收为新增内容 additive、旧 Chat wire golden 不变、完整 descriptor/跨语言生成和目标语义检查通过。此阶段新增 target 仍 not_implemented，待生产实现/测试后再翻。

### STR-02：HTTP/SSE parser 选型与本地夹具（owner：Provider worker；可与 STR-01 独立准备）

允许文件：拟新增 `infrastructure/tests/model_sse_parser_test.cpp`、`tests/architecture/p11_streaming_provider_fixture.py`；审核后 `dependencies/architecture/vcpkg.json`、`dependencies.lock.json`、`infrastructure/CMakeLists.txt` 的 private 依赖接线由主代理统一；`doc/11-toolchain.md` 的依赖段。

输入是标准 SSE/现有 curl锁；输出是锁定库版本/hash、许可、bounded内存策略和全分段解析矩阵。错误是非法/未完流可辨识拒绝，不发生模型调用。验收含 CR-only、多行空data、UTF-8拆分、BOM、超长无换行、取消、NUL/非法JSON、EOF、库自动重连禁用。选型不合格不得进入“Provider完成”。禁止用真实外部服务补测试。

### STR-03：纯应用流端口与状态机（owner：Model 应用 worker；依赖 STR-01）

允许文件：`core/include/kiarabot/core/{domain,ports,application}/target/model_*`、`core/include/kiarabot/core/ports/target/unit_of_work.hpp` 的 Model 部分、拟新增 stream应用文件、`core/src/application/target/model_gateway_application.cpp`、`core/tests/architecture/model_gateway_application_test.cpp`/stream专项。共享 CMake 由主代理登记。

输入为可信 caller/context、model/messages/logical id、sink/cancel；输出为第 3 节事件和既有 durable intent。实现同一防重/txn1/txn2核心，旧 Chat与Stream共享 fresh并发预算；Provider I/O/sink/Identity检查均在 UoW外。覆盖 duplicate并发、跨模式重放、异内容CONFLICT、config/secret pinning、txn1/txn2 unknown、partial、撤权、deadline、取消和过期STARTED仲裁。输出无SQL/protobuf/HTTP依赖，纯C++测试不需真实网络。

### STR-04：Model 存储恢复查询（owner：Model PG worker；依赖 STR-03端口）

允许文件：`infrastructure/postgres/src/model_store.{hpp,cpp}`、Model内存fake、`infrastructure/postgres/tests/` Model专项及注册；默认不改 schema。输入为可信 cutoff/limit/version，输出为有界超期记录与CAS结果，错误沿用现有 PG 映射。

验收真实PG16.4：有效STARTED不动；超期转UNKNOWN；两个清理器/worker结果竞争唯一终结；提交Unknown不再调Provider；重启仍可查状态；四库descriptor保持原值。若确需schema变更，先按第7节交主代理重新定文件归属，不能私自扩散。

### STR-05：Provider 流式实现（owner：STR-02 Provider worker；依赖 STR-02合格解析器、STR-03端口）

允许文件：`infrastructure/src/model_http_provider.cpp`（建议新增 `model_http_stream_provider.cpp`/私有头拆分，而非扩大现有 perform 巨函数）、`infrastructure/include/kiarabot/infrastructure/model_http_provider.hpp`、`infrastructure/tests/model_http_provider_test.cpp`/stream专项。

输入为 pinned config、借用且生命周期覆盖整个调用的 secret、deadline/cancel、sink；输出为纯领域增量和最终 Provider facts。现有 chat仍 stream=false；流路径独立 stream=true，不自动降级/重试。curl header/secret保持RAII，取消从无新字节的stall中也能退出；解析/累计字节/单帧/队列上限必须全部生效。验收真实loopback HTTP分段矩阵、完整终止/usage-only、429、5xx、异常HTTP、TLS/连接失败、半流断开、慢消费、长行和取消；断言Provider POST计数。

### STR-06：Model RPC 和精确在线授权（owner：Model RPC worker；依赖 STR-01、03、05）

允许文件：`core/src/runtime/platform/model_rpc.cpp`、`model_identity_client.cpp`、对应公共头、`core/app/model_composition.{hpp,cpp}`、`core/tests/platform/{model_rpc,model_identity_client}_test.cpp`。

输入为mTLS/受限Runtime委派+ChatStream请求，输出为ServerWriter事件与结构化失败。新增方法精确白名单，30秒/委派expiry、1秒重新授权和取消贯通，不给Control管理调用额外权限。验收Runtime-origin成功、伪造peer/错误method/scope/父委托/撤权/Identity离线拒绝、pre-start trailers/post-start terminal映射、写失败清理和join；Provider次数在拒绝路径为0。

### STR-07：Runtime 客户端/应用与 Bridge 流调度（owner：Runtime/Bridge worker；依赖 STR-01、03、06）

允许文件：`core/include/kiarabot/core/ports/target/runtime_model.hpp`、`core/include/kiarabot/core/application/target/runtime_plugin_capability.hpp`、`core/src/application/target/runtime_plugin_capability.cpp`、`core/include/kiarabot/core/runtime/platform/runtime_model_client.hpp`、`core/src/runtime/platform/runtime_model_client.cpp`、`core/src/runtime/platform/runtime_bridge.cpp`、必要新私有 `runtime_model_stream*` 组件、`core/app/runtime_composition.cpp`、`core/tests/platform/{runtime_model_client,model_bridge}_test.cpp`、`core/tests/architecture/runtime_plugin_capability_test.cpp`。

输入为逐帧已认证的start/cancel；应用派生owner/installation并维护有界调用句柄；客户端读server stream、解码RpcFailure、传播取消；Bridge唯一writer发送Fence事件，reader继续heartbeat/refresh/ACK/cancel。不得使 transport持有业务规则、数据库或Provider秘密。验收4并流/超限拒绝、两个logical id不串帧、旧Fence/他人cancel拒绝、重复cancel、连接关闭/撤权/停机无悬空引用、慢写有界及并发非流式不回归。路径表述按现有头目录 `core/include/kiarabot/core/runtime/platform/` 和源目录 `core/src/runtime/platform/` 落地，不创建重复namespace目录。

### STR-08：SDK async stream 消费（owner：Python SDK worker；依赖 STR-01、07）

允许文件：`sdk/python/src/kiarabot_sdk/{runtime,transport,errors,testing,__init__}.py`、拟新增 `model_stream.py`、`sdk/python/tests/test_model*.py`/stream专项、`sdk/python/README.md`；不能手工编辑 `build/generated`。

输入为生成proto流事件；输出为typed async events/terminal/cancel。旧 `model_chat`/PluginError兼容；新增stream错误可附acceptance/recovery/partial/usage/billing而不破坏旧构造签名。测试feature缺失/permission、UUID与合计输入、sequence/unknown oneof、stream队列超限、消费者提前退出、任务取消、terminal丢失、重连按原id、future/registry归零、单reader控制帧不被慢流卡住，以及现有model单测全过。

### STR-09：受管插件全链验收（owner：集成 worker；依赖 STR-04至08）

允许文件：拟新增 `tests/architecture/p11_streaming_managed_model_e2e.py`、本地stream fixture、必要test-only seed/helper、`cmake/platform.cmake`/CTest登记由主代理合并。复用P11h自有PG16.4/TLS/Identity/Runtime/Model/真plugin-host管理SDK子进程，不用FakeHost结果代替生产链。

输入为HTTP fixture受控gates；输出为带精确命令/退出码/Provider计数/顺序marker/持久call事实的归档。必须让fixture在第一段后等待测试屏障，证明SDK在上游完成前收到增量；不能仅比较最终拼接文本。覆盖success、empty、usage缺失、length截断、半流失败、deadline、显式cancel、Bridge断开/重连、Model重启、撤权/Identity不可达、txn2失败、慢消费者/队满、同id并发/跨模式重放、跨实例隔离和非流式回归。取消后Provider最多一次，未知/重放不再次POST。

### STR-10：网页展示/QQ聚合接口交接（owner：WS工作流；本流提供语义，依赖 STR-01、09）

本流不拥有Panel App或WS stream文件。交接 `logical_call_id/sequence/termination/partial/usage/billing`、可见scope和不恢复正文规则。当前 Control WS没有天然的插件正文事件来源；仅新增管理WS并不会让模型token到网页。要网页正文展示，WS owner必须先冻结从Model owner到Control的**独立、授权明确、只订阅已存在调用**端口、调用者精确权限、owner/model scope和无持久回放的输入输出，再更新 `api/asyncapi.yaml`/必要REST和文档；不能由Panel直连Model，也不能让浏览器订阅启动Provider或继承插件委派。

推荐把网页正文展示作为依赖STR完成后的独立WS子任务，不阻塞Provider→SDK首批闭环；在该端口未实施前网页只展示可授权的调用元数据，保留“正文流待接线”状态。QQ如接入示例，文件只在另分派的 `plugins/ai-reply/`，复用已存logical意图、完整聚合≤64KiB、仅COMPLETED后提交同一幂等Action；REPLAY无正文、TRUNCATED、FAILED、CANCELLED、UNKNOWN均不发半截消息。不调用真实QQ作本批验收。

### STR-11：文档、登记和集成门禁（owner：主代理；依赖全部必需任务）

主代理统一合并共享文件，回填实际源码/测试证据后才翻target implemented；更新doc/27第0节、doc/29及本报告完成记录。首批完成定义是STR-01至09闭环+STR-11必需门禁，STR-10网页正文为单列依赖项，不能在交付中隐去或宣称已完成。

## 9. 构建、生成、测试和格式命令

以下为实施后计划执行，**本报告未运行**。主构建树 `build/full-locked` 由主代理串行租用，worker不能同时configure/build；重型PG/TLS/进程测试排队。隔离构建须共用只读依赖、独立build和fixture端口/数据库，不把主树已有二进制当新实现。

契约与SDK：

```bash
python3 proto/tests/check_enum_consistency.py
python3 proto/tests/check_implementation_status.py
python3 proto/tests/check_semantic_rules.py
./proto/tests/check_contracts.sh
KIARABOT_TARGET_PYTHON=sdk/python/.venv/bin/python ./proto/tests/check_target.sh
./proto/tests/descriptor_compatibility_test.sh
uv sync --project sdk/python --locked --python 3.11.18
sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py
sdk/python/.venv/bin/python -m pytest -q sdk/python/tests/test_model.py sdk/python/tests/test_model_sdk.py sdk/python/tests/test_model_stream.py
```

`test_model_stream.py` 为拟新增文件，只有登记后才运行；不能把文件不存在的命令算通过。与发布版兼容检查须显式提供 `KIARABOT_BASELINE_DESCRIPTOR` 再跑契约门禁，未提供只说明本次additive夹具通过。新增枚举/方法/feature vocabulary会触发target_spec和闭合集合检查，不能删除旧断言绕过。

锁定构建、现有回归和拟新增专项：

```bash
cmake --preset full-locked -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build --preset full-locked --parallel 4
ctest --preset full-locked -R '^(p11_model_gateway_application|p11_model_http_provider|p11_model_rpc|p11_runtime_model_client|p11_model_identity_client|p11_model_bridge|p06_runtime_plugin_capability|p11h_managed_model_e2e|p11h_model_chain_fail_closed|p11h_provider_fixture)$' --no-tests=error --output-on-failure
ctest --preset full-locked -R '^p11_streaming_' --no-tests=error --output-on-failure
sdk/python/.venv/bin/python -m pytest -q sdk/python/tests
python3 scripts/quality/check-boundaries.py
python3 scripts/quality/check-architecture.py --inner
python3 scripts/quality/check-architecture.py --links build/full-locked/architecture-targets.json
python3 scripts/quality/check-architecture.py --database build/full-locked/compile_commands.json
./scripts/quality/check-format.sh
KIARABOT_TIDY_BUILD_DIR=build/full-locked ./scripts/quality/check-tidy.sh
git diff --check
```

主代理先用 `ctest --show-only=json-v1` 对每个必需名字逐项核对存在/未禁用，再实际执行；正则命中一项不能证明全部已注册。`p11_streaming_*` 是计划的新测试族：parser/provider、application、rpc、runtime/bridge、postgres recovery、managed e2e；STR-11须归档最终精确名称。

依赖变化另跑 `python3 scripts/quality/check-dependency-lock.py` 和其自测；schema变化另跑当前 `p03_postgres_*`/Model专项和四库descriptor实测，不能只以构建代替。最终按主代理协调运行一次 `ctest --preset full-locked --no-tests=error --output-on-failure`；统一 `./scripts/check-toolchain.sh` 若因已知全仓format/tidy基线不能全绿，保留失败清单、改动文件专项和基准对照，不能改称完整通过或借机重排全仓。

## 10. 文档影响和交付核对

本次实际更新仅本报告；这是计划说明，没有改变运行时可见接口，产品协议/模块文档本轮不提前同步成“已实现”。直接实施时必须同步：

- `proto/README.md`、`doc/01-contracts.md`：新RPC/stream帧/feature/枚举、状态登记、精确权限和错误映射。
- `doc/01-runtime-semantics.md`：logical身份、跨模式防重、partial、重放无正文、取消/断线/UNKNOWN和STARTED恢复。
- `doc/02-core.md`：Model两事务流状态机、Runtime应用/Bridge调度、资源与授权边界。
- `doc/03-plugin-sdk.md`、`sdk/python/README.md`：async接口、兼容性、取消/重连/队列与错误语义。
- `doc/05-infrastructure.md`：标准SSE/Provider转换、限额、usage/billing、存储恢复；不得把新实现写到reference T06b记录里。
- `doc/08-operations.md`：新增stream运行限额、活跃/取消/UNKNOWN/cleanup指标、日志字段及有界停机；日志不得含prompt、completion或秘密。
- `doc/11-toolchain.md`：解析器锁、构建注册、生成与验收命令；`doc/27-phase-exit-evidence.md`第0节和`doc/29-approved-capability-expansion-plan.md`：当前实现/实测/缺口。
- `reports/modules/architecture-p11-model.md`：新增有日期的stream报告入口；保留过去“未做streaming”的历史作用域。
- `api/asyncapi.yaml`、`doc/06-control-api.md`：仅STR-10引入网页流可见协议时由WS owner更新。`api/openapi.yaml`仅在确有REST端点/schema变化时同步，不为内部gRPC/SDK增量臆造REST改动。

无需本批修改：`doc/04-nonebot-adapter.md`及适配器协议（QQ仍复用完整Action，不引入平台增量）；`doc/07-control-panel.md`和Panel实现（STR-01至09没有网页正文入口，STR-10另有owner）；无关路线图/reference代码/历史验收原文；`doc/18`与保护清单任何阶段均不改。若增加schema/配置/运行方式，按第7节/第5节收紧判断，不能继续援引“内部变化”豁免。

实施后的文档校验命令包含本节契约门禁、`git diff --check`，有API变化再跑 `./scripts/quality/check-openapi.sh`、`./scripts/quality/check-asyncapi.sh`；检查所有新增相对链接指向实际文件、文档枚举/状态/默认值与契约一致。计划阶段只做报告的链接/必需主题检查和diff检查，不执行耗时构建、代码生成或模型HTTP调用。

主代理需协调的共享写入清单：两个proto及其target_spec/status/enum基线、`unit_of_work.hpp` Model端口、`runtime_model.hpp`、`runtime_plugin_capability.*`、`runtime_bridge.cpp`、Model/Runtime组合根、`core/CMakeLists.txt`、`cmake/platform.cmake`、`infrastructure/CMakeLists.txt`、依赖锁，以及doc/01/02/03/05/08/11/27/29。若STR-10启动，再由WS工作流拥有API/Panel写入；若有schema扩张，再统一处理四库manifest，不允许各worker分别覆盖。

## 11. 最终验收条件和本轮检查

完成streaming必须同时证明：真实受管插件在Provider完成前收到文本；正常结束和partial失败可区分；取消、deadline、撤权及慢消费可有界收尾；多流不串帧且控制帧继续工作；重复logical id最多一次Provider POST；UNKNOWN/usage/billing可正确观察；崩溃后STARTED不永久伪装在途；旧非流式链保持通过；契约/文档/生成/格式与相关构建测试有实际结果。仅把请求改为stream=true、最终拼接后返回或只在FakeHost演示不满足验收。

本轮实际执行：只读源码/文档/图查询、报告新增、Python核对本报告内部链接及11项STR任务/必需主题，均通过；`git diff --check -- doc/plans/2026-11-04-model-streaming-plan.md` exit 0，但新文件尚未跟踪，另以Python实际读文件检查行尾空白/末尾换行，并运行 `git diff --no-index --check /dev/null doc/plans/2026-11-04-model-streaming-plan.md`，后者exit 1且无空白诊断（no-index发现新增差异）。`git check-ignore` exit 1且无输出，确认新路径未被忽略；旧reports路径已移走。链接检查首轮因主代理同步迁移决策文件而失败，已改成同目录链接并复跑通过。未运行C++构建/CTest/格式/SDK生成/付费Provider/真实QQ：没有产品代码变动，主构建树由主代理协调，本轮仅交付计划。
