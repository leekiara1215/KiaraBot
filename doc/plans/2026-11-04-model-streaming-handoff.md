# Model streaming 工作流交接（2026-11-04）

## 接手指令与边界

用户已授权依依赖继续契约冻结与实现，但本 agent 按用户最新指示停止扩展实现；本次最后操作仅写本交接。仓库有大量多人未提交修改，禁止 reset、stash、回滚他人修改或整体覆盖文件；不提交既有差异。不改 `doc/18-architecture-baseline.md`、保护清单、reference blob 或生成物。共享 `build/full-locked` configure、生成、编译、昂贵测试由统筹 agent 串行调度，接手 agent 不自行并行运行。仅本地 HTTP/TLS/PG fixture，无付费调用、QQ 主动消息；QQ 默认最终聚合一次发送，网页承载归 WS 工作流。

开工先读 AGENTS、doc/12、doc/00、doc/02 的 Model 章节、doc/03 streaming 章节、doc/05-infrastructure-model-streaming.md、doc/11 新第三方 C99 节、doc/plans/2026-11-04-model-streaming-plan.md 及相应 proto。结构查询使用 codebase-memory skill；最近已核对 generation 2026-11-04T08:26:54Z，application/target 等未覆盖范围必须回读源，不能用空图证明缺失。

## 冻结时的实际状态

- STR01 契约已冻结且统一 gate 曾通过：ChatStream、纯 model_stream 类型、HostRequest start/cancel、ordered stream_response、observations、HERR 等。新增 target 注册仍是 `not_implemented`；尚未形成 Runtime→Bridge→SDK 闭环。
- STR02/05 Provider/parser 第一切片已由统筹实际构建测试：首轮 13/13 通过 12.17s，微修后生产 parser/stream Provider/旧 Provider 3/3 通过 5.54s。eventsource-c 固定 commit `95309f6191df705e7c00acfba0675a110ebb79fa`，仅使用 C parser，复用现锁 curl；锁记录来源/hash/MIT，第三方源码仅 build 缓存，已验证许可 component install。不要另换协议或恢复 POST 自动重连。
- STR03/04 Model 应用与恢复已由统筹实际构建测试：`p11_model_gateway_application` 1.01s、`p11_streaming_model_application` 4.03s、`p03_postgres_database` 20.05s，3/3 共25.10s；日志 `build/logs/capability-expansion/ctest-model-app-pg-first.log`。包含旧 Chat/新 Stream 共享 fingerprint/逻辑调用幂等、同 id 不重调 Provider、32帧/64KiB bounded queue、非阻塞 try_emit、当前授权 drain、partial/usage/计费、STARTED35s恢复及 UNKNOWN30天最小修复、真实 PG 29/30天与 CAS 边界。call 文本不持久化，无 schema/journal/manifest 修改。
- **STR06 是半成品：已有真实编辑，尚未接 CMake，未完成编译、链接或运行测试；不能借上述 3/3 证明 RPC 通过。** 当前 ModelRPC.cpp 已调用新 helper，因此下一轮重新链接 Model 前必须先补接线，否则会产生缺符号风险。

## STR06 文件与已写行为（未验收）

新增文件：

- `core/include/kiarabot/core/runtime/platform/model_stream_codec.hpp`
- `core/src/runtime/platform/model_stream_codec.cpp`
- `core/include/kiarabot/core/runtime/platform/model_stream_rpc.hpp`
- `core/src/runtime/platform/model_stream_rpc.cpp`
- `core/tests/platform/model_stream_codec_test.cpp`
- `core/tests/platform/model_stream_rpc_test.cpp`
- `core/tests/platform/model_stream_rpc_e2e_test.py`

现有文件中的 STR06 编辑（保留此前其他切片和他人修改）：

- `core/include/kiarabot/core/ports/target/model_ports.hpp`：ModelCaller 末尾新增 optional delegation_expires_at；未发现 ModelCaller positional aggregate initializer，但最终编译仍需核查新增告警。
- `core/src/runtime/platform/model_identity_client.cpp`、`core/tests/platform/model_identity_client_test.cpp`：保留 proof expiry；Runtime 方法明确仅 Chat/ChatStream；ListOperationChanges 加入 management 分类。源码核对现有 RuntimeModelClient 仅使用 Chat，GetCallStatus 没有合法 Runtime 调用路径；旧管理方法继续 Control。相关新断言尚未运行。
- `core/include/kiarabot/core/runtime/platform/model_rpc.hpp`、对应 cpp：make_model_rpc 新增末尾 optional observation app 指针（默认 null，保留旧两参数调用）；ChatStream 严格输入/关联/空幂等键/消息64KiB预检、Runtime 身份与 proof expiry/min deadline；ListOperationChanges 复用 WS prepare/pure app/encode。
- `core/app/model_composition.cpp`：持有 Model audience RuntimeIdentityClient 与纯 observation app，Model UoW begin 转 base，注入 RPC。新依赖还没有 CMake 显式接入或完整编译。

codec 已写有状态 Decoder：连续序号、request/logical/trace UUID 关联、started/prestart terminal、replay 无 delta、UTF8/4KiB delta/64KiB总文本、terminal partial/counts、usage/optional billing presence、call pinned identity、failure 关联与未知枚举拒绝。非法前缀会 poison；缺 terminal 仅能作为未知传输结束，不得伪造完成。encode 是转换入口，跨帧安全由 Decoder 执行。

RPC helper 已写独立 application worker、非阻塞 bounded sink、terminal 单独槽、唯一 RPC 线程 Write、10ms watchdog、每次 Write 最长1s、取消传播与 join。Provider/应用 worker 不执行 ServerWriter。每秒当前授权在已验收纯应用内完成。queue 满前先判容量，不推进 Decoder；取消后仍允许 terminal 入槽。上述线程/传输行为仍需真实运行验证，不能当作已经合格。

新 codec 测试拟覆盖 fresh/truncated/replay/prestart、关联/sequence/UTF8/presence/usage/billing/partial/counts 等正负例；新 RPC loopback 测试拟覆盖因果增量先达、重放零 Provider、输入/来源拒绝、取消和 proof expiry。新 Python fixture 拟跑真实 Identity+Model+mTLS+PG+本地 HTTP SSE、旧 Chat↔Stream 重放、429/partial/truncated、错误证书、撤权与观察 cursor，尚未首次执行。

## 接手必须先核对的具体风险

1. Python fixture `cancel and durable UNKNOWN` 当前在 `running.cancel()` 后立即 `provider.release.set()`，存在 Provider 已完成与取消到达竞争。应先保持 Provider 阻塞，因果等待 durable UNKNOWN，再释放；不要通过放宽数据库断言掩盖问题。
2. Python fixture 观察断言当前写 `item.operation.operation...`；`observations.proto` 注释使用 `ref`，必须回读实际 Operation message 与既有 WS fixture 确认字段，不能假设当前访问有效。同样先确认 fixture SQL 中 model_intents/state 列和状态值、注册 fixture helper 的参数与返回类型。
3. 已发现并修过 codec 的 `FailureContext.retry_after_ms` 标量误用 presence、ServerWriter 仅前置声明导致 incomplete template。最后一次只读语法检查启动后会话丢失，结果未取回，**不能认为这些修复后通过**。sync_stream.h 已加入；接手重新语法/构建检查。
4. replay started 后最终授权撤销时，应用可能产生 failed/cancelled terminal，而冻结 oracle/Decoder 对 replay 只允许 replay terminal；目前 decoder fail-closed 将变成 structured unknown 传输错误。核定预期行为并覆盖，不能偷偷放宽协议或误报成功。
5. 真实 gRPC 慢消费者能否触发1s watchdog/正确解除阻塞尚无实证；64KiB上限可能全部进入 gRPC buffer，当前 loopback 测试不能证明慢写预算。接手应设计能因果验证 watchdog/取消 join 的回归，避免仅靠 sleep 声称覆盖。还需检查所有异常路径的线程生命周期和 terminal 丢失语义。
6. Parent 要求 Model 配置并发上限继续旧 Chat/Stream 共享；Runtime 后续实现 process64/per-connection4。不要把 RPC worker 无界扩张或把 ServerWriter 放入 try_emit。当前 helper 每 RPC 建 worker，需评估入站饱和下的线程边界，与应用已实现的64 fresh调用cap协调。

## CMake 归属与待集成片段

`cmake/platform.cmake` 单一 editor 为 WS/control_ws；root/core CMake 为 Host/host_bootstrap；本 agent **尚未向 WS 提交或落盘 STR06 接线**。以下是应交给其最小集成请求，目标名是拟新增，尚不存在：

```cmake
add_library(kiarabot_model_stream_codec STATIC
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/model_stream_codec.cpp)
target_include_directories(kiarabot_model_stream_codec PUBLIC ${CMAKE_SOURCE_DIR}/core/include)
kiarabot_platform_warnings(kiarabot_model_stream_codec)
target_link_libraries(kiarabot_model_stream_codec PUBLIC kiarabot_proto
  PRIVATE kiarabot_target_codec)
target_sources(kiarabot_model_rpc PRIVATE
  ${CMAKE_SOURCE_DIR}/core/src/runtime/platform/model_stream_rpc.cpp)
target_link_libraries(kiarabot_model_rpc PUBLIC kiarabot_model_stream_codec
  kiarabot_operation_observation_rpc PRIVATE Threads::Threads)
# 放在 kiarabot-model 目标已定义之后，显式记录 composition 的新依赖。
target_link_libraries(kiarabot-model PRIVATE kiarabot_runtime_identity_client)
add_executable(kiarabot_model_stream_codec_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/model_stream_codec_test.cpp)
target_link_libraries(kiarabot_model_stream_codec_test PRIVATE kiarabot_model_stream_codec)
kiarabot_platform_warnings(kiarabot_model_stream_codec_test)
add_test(NAME p11_streaming_model_codec COMMAND kiarabot_model_stream_codec_test)
set_tests_properties(p11_streaming_model_codec PROPERTIES LABELS "p11;streaming;unit" TIMEOUT 20)
add_executable(kiarabot_model_stream_rpc_test
  ${CMAKE_SOURCE_DIR}/core/tests/platform/model_stream_rpc_test.cpp)
target_link_libraries(kiarabot_model_stream_rpc_test PRIVATE
  kiarabot_model_rpc kiarabot_model_gateway_application)
kiarabot_platform_warnings(kiarabot_model_stream_rpc_test)
add_test(NAME p11_streaming_model_rpc COMMAND kiarabot_model_stream_rpc_test)
set_tests_properties(p11_streaming_model_rpc PROPERTIES LABELS "p11;streaming;integration" TIMEOUT 30)
```

Python e2e 的拟 CTest `p11_streaming_model_rpc_e2e` 应照现有 ws03_operation_observation_e2e 接 `run_database_test.py`：`--pg-bin ${KIARABOT_TEST_PG_BIN}`、`--openssl ${KIARABOT_TEST_OPENSSL}`、`--initializer $<TARGET_FILE:kiarabot-pg-initialize>`、`--test` 新 Python 文件，四个顺序 test-argument 为 identity-bootstrap target、PG_BIN、identity target、model target。TIMEOUT180；环境沿用 RUNTIME_E2E_OPENSSL_BIN_DIR、RUNTIME_E2E_PYTHON_BIN_DIR 和 SDK generated PYTHONPATH。wrapper 固定前置参数后，脚本 sys.argv[5] 是 bootstrap、[6] PG_BIN、[7] identity、[8] model。先核查独占 fixture ownership/SHOW data_directory 的实际返回和 marker 路径，不能动外部数据库。

源 proto 已冻结生成，根/infra 的 model_stream.proto 与 observations.proto 清单已由前轮接入。本切片不改 wire/schema，无需 PG bump；Python fixture 仍依赖生成的最新 model_stream/observations bindings。生成由统筹调度，禁止手编生成物。WS 已最小扩展 shared runtime_identity_client decoder 支持 Model audience；root 实际测 `p05_runtime_identity_client`1/1 通过2.09s。该 decoder 仅解 metadata，不能代替 Control SAN/实际证书绑定和在线 Identity 授权。

## 下一验证命令（只有统筹获串行窗口后执行）

先完成上述修复、CMake、格式与 fixture 核对，再：

```sh
source build/env/p00-env.sh
cmake --preset full-locked -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/full-locked --target kiarabot_model_stream_codec_test kiarabot_model_stream_rpc_test kiarabot_model_identity_client_test kiarabot_model_rpc_test kiarabot-model kiarabot-identity kiarabot-identity-bootstrap kiarabot-pg-initialize
ctest --test-dir build/full-locked --output-on-failure -R '^(p11_streaming_model_codec|p11_streaming_model_rpc|p11_model_identity_client|p11_model_rpc)$'
ctest --test-dir build/full-locked --output-on-failure -R '^p11_streaming_model_rpc_e2e$'
```

configure 应沿用统筹已有 locked cache/preset 参数，不能用无参数命令覆盖依赖锁设置。先运行 `ctest -N` 确认新测试已注册，避免0 tests假通过。旧 Chat/ModelIdentity/ModelRPC 必须保留回归；若改应用/PG，再按统筹窗口复跑前述3项，不能仅改 mock 断言。格式工具 `build/format-venv/bin/clang-format` 固定16.0.6，tidy同目录22.1.8；格式只处理所属文件。可先 `python3 -m py_compile core/tests/platform/model_stream_rpc_e2e_test.py`，但静态语法不能证明 fixture 有效。

实际检查记录：STR06 初次 syntax-only 曾明确失败于 retry_after presence，随后 codec syntax-only 一次成功；RPC 曾明确失败于 ServerWriter incomplete template，随后加头但未取得复跑结果。最后 Python py_compile 命令与 syntax-only 一起启动，其结果未取回，故均不登记为成功。无 STR06 shared configure/build/CTest 记录。独立 exec session5929 已查询为 Unknown process id，当前无本 agent 仍持有的构建/测试进程。

## 后续 agent 任务提示词与依赖

**RPC 接手 agent（先做 STR06）**：按本交接从半成品继续，不重做已通过 Provider/应用切片。独占上述 STR06 文件及 Model 文档章节；与 WS 协调 platform CMake，保持共享 decoder 的单 editor。先修具体风险、跑严格 codec/loopback、真实TLS/PG/HTTP fixture、旧 syncChat 回归，取得统筹可审查日志后冻结源码。未通过前不更新能力 target 为 implemented、不宣告 streaming 可用。不得扩工具调用/multimodal或改 PG schema。

**Runtime/Bridge agent（STR07，依赖 STR06 严格 codec 与 RPC 通过）**：实现 runtime_model_client ChatStream 消费，复用严格 Decoder 检查全部 presence/关联/sequence；取消、proof min expiry、缺 terminal→UNKNOWN/保守 may_have_billed，不做重连重调 Provider。设计异步 bridge 会话与 Host start/cancel、CoreToPlugin 有序事件；禁止阻塞 Host 主循环；实现 process64、per-connection4、bounded queues/慢消费者 failclosed、撤权/deadline/worker join。保留既有非流式、HERR 已验收逻辑和 Runtime 既有调用白名单。开始前让统筹分配 runtime_bridge/runtime_model_client/runtime composition/测试的独占锁，勿同时改 WS R/S/平台接线。

**SDK agent（STR08，依赖 STR07 wire 生命周期稳定）**：实现 Python async iterator/显式 cancel/context lifecycle、capability host.model.stream.v1 与能力缺失拒绝、逻辑调用 id 和最终结果保留，严验事件序号/关联/terminal/usage/billing，不从异常推断免费，不因重放假装返回历史 delta。普通 chat 保持旧行为；SDK 不直接访问 Provider 或平台。与 Runtime agent 先约定 session ownership、取消 acknowledgment 和 event callback 不阻塞策略，再做真实受管插件闭环。

**最终验收 agent（STR09..11，依赖完整闭环）**：真实本地 HTTP→Model TLS/PG→Runtime→Bridge→受管 SDK 插件验证第一 delta 在 Provider 完成前到达、成功/截断/失败/partial/取消/撤权/deadline/重放/崩溃恢复/背压/不支持能力；同时跑旧非流式回归。更新直接模块/运行语义/工具链及 target registry 实现状态时只能登记实际已验收能力。网页承载与 WS owner 对接，QQ 默认完整聚合一次发送。两主机/断网演练不加入本轮；不以 mock 切片、stream=true或规格oracle代替生产闭环证据。

## 文档影响与交付限制

STR06 写代码前已更新 `doc/02-core.md` Model STR06 节、`doc/03-plugin-sdk.md` streaming 状态、`doc/05-infrastructure-model-streaming.md` 实现边界；这些内容说明开发中，接手应以实际验收继续同步。doc/11 前轮已有第三方C99/锁/许可说明。未改 proto/API，因此本切片无需新 wire 文档或生成 schema；doc01/运行语义若实现结果改变公共错误/状态应协调其 owner 同步，不能自改冻结协议。doc18 只读；根计划/registry 由统筹统一。本文只交接现状，不将任何新 RPC/Runtime/SDK 功能宣称完成。
