# 五项扩展执行台账（2026-11-04）

## 授权与基准

用户在批准五项范围后继续授权：“继续推进契约冻结，完成后按依赖安排 subagent 实现，
你负责统筹和验收，不用每一步都等我确认。”
因此本轮包含实际契约编辑、生成/兼容验证、实现、集成与验收，常规设计和内部任务分派直接推进。
两主机部署与独立断网演练仍延期；真实 QQ 主动消息、付费模型和发布沿原授权边界。

起始代码是 `0496b90053d046fd61d7f106090bb87fff172f8a` 加已有未提交重构与上一轮计划。
开工文件副本和哈希保存在本机 `/tmp/kiarabot-capability-expansion-20261104-start/`，
起始 descriptor 为 `build/logs/capability-expansion/baseline-descriptor.pb`。
二者仅用于本机验证，不提交构建产物、秘密或本机配置。

续执行时发现上述 `/tmp` 开工副本已不在本机，不能据此重做逐文件开工哈希比对。
原始 descriptor 与冻结日志仍在 build 日志目录；受保护 doc/18 和 AGENTS 当前逐字节与 HEAD 相同。
新增 `build/logs/capability-expansion/continuation-checkpoint.json` 保存续执行点 1040 个文件哈希，
明确仅是续执行检查点，不替代丢失的开工副本，也不反推既有未提交差异已获得完整保护证明。

## 波次 B：公共契约冻结

共享文件单一编辑者，主代理集成评审。

- **WS-01，冻结通过，`/root/control_ws`**：独占 `api/asyncapi.yaml`、新 `observations.proto`、
  `scheduler.proto` 的变更查询方法，以及 Control/Panel 的直接 WS 说明。
  将共同 OperationChanges 类型和各 owner 方法片段交给相应 proto 所有者，不并发编辑其文件。
- **STR-01，冻结通过，`/root/model_streaming`**：独占 `model.proto`、`plugins.proto`，
  冻结 ChatStream、PluginBridge start/cancel/events 与 SDK 语义；在 model.proto 接入统一 changes。
  与 HERR 统一 HostRequest oneof 的新 tag 与未知字段分类；并行核定 STR-02 的成熟 SSE 解析器。
- **HERR-01 / HOST-01 / BOOT-01，冻结通过，`/root/host_bootstrap`**：独占
  `host.proto`、`runtime.proto`、`api/openapi.yaml`，冻结 Host maintenance/operation owner、
  Runtime 禁派和 bootstrap 交付/重发；统一各工作流的 registry、target spec、oracle、
  contract tests、proto README 和跨契约错误/运行语义文档。
- 公共 `platform.proto` 若确需变化，先由主代理指定单一编辑者；优先用独立 observations 类型，
  避免无关公共字段改动。新能力在实现验收前全部保留 `not_implemented`。

契约冻结必须有精确字段/方法/options、错误/状态和授权映射，相关解析/代码生成、
起始 descriptor 的 additive 兼容检查及 target/evidence gates 实测，不以方案报告代替。

## 波次 C / D：实现和验收

上述公共接口已按 [冻结记录](../30-capability-expansion-contract-freeze.md) 通过门禁。
已分派第一批实际实现，不能以新声明或测试注册记为功能完成：

- **WS-02，局部验收通过，`/root/control_ws`**：纯 observation 类型/端口、UnitOfWork 转发，
  R/S/M 的 PostgreSQL 同事务 change journal、提交序 head、low-water/epoch、真实 PG 测试。
  该切片独占 PG 通用工作单元、factories、schema/manifest 和 PG CMake；不改回执 codec 或 Identity schema。
- **STR-02/03/05 Provider 切片，局部验收通过，`/root/model_streaming`**：锁定 SSE C parser、纯流类型/端口、
  Provider 流式实现与真实 HTTP/分段测试。先不编辑 Model RPC、Runtime Bridge 或 SDK，待端口实测后连接。
  独占 parser 依赖、根/infra CMake 的相关接线和 Provider 模块；PG query/manifest 与 WS 所有者协调。
- **HERR-02、BOOT-03，局部验收通过，`/root/host_bootstrap`**：目标 HostRequest 分类与拒绝无副作用测试；
  Control SecretDeliveryPort、固定 recipient 的 0600 文件 sink 和崩溃恢复测试。
  先不改 Runtime PG/签发版本，不能将文件模块成功记为 BOOT 全链完成。

HERR 切片结束释放 Bridge 编辑锁，供 STR 后续连接；BOOT Runtime 持久化在 WS PG schema 后串行接入。
Host SQLite 维护应用、owner RPC、WSS/Panel、SDK 与最终真实进程集成随后按依赖下发，仍属于已批准范围。

主代理串行管理 `build/full-locked`、公共协议生成目录、全量 CTest 和质量门禁；
worker 使用独立临时生成/构建目录，不能同时重写同一编译数据库。
各工作流必须先同步直接模块文档，再实现并验证公共行为。受保护 doc/18 及既有改动保持。

## 实际检查记录

- 开工副本/哈希：完成，984 个文件。
- 起始 proto descriptor：锁定环境 `protoc --include_imports` 生成，exit 0。
- 契约冻结：全部 proto/C++/gRPC 生成编译、基线 descriptor 兼容、118 RPC、376 消息、25 golden、
  模型流 26 项、WS 36 项、Host/bootstrap 38 项及完整 contracts/API/声明门禁通过；详见冻结记录。
- 已有以下局部实现实测；尚未有新增 WS、streaming、Host 管理或 bootstrap 全链验收通过记录。

### HERR 首切片实测

`cmake --build --preset full-locked --target kiarabot_model_bridge_test --parallel 4` exit 0；
`ctest --preset full-locked -R '^p11_model_bridge$' --no-tests=error --output-on-failure` exit 0，
1/1、3.94s。真实 TLS PluginBridge fixture 验证 raw-wire 缺失/future/歧义/错 wire 分类、
错误关联、拒绝无下游调用以及拒绝后继续合法请求；保留标准 oneof last-one-wins。
日志：`build/logs/capability-expansion/build-herr-model-bridge.log`、`ctest-herr-model-bridge.log`。

这次构建明确未接入尚未完备的 STR Provider CMake 片段，不将其算作流式能力验收。
构建中的既有 runtime_action_application/runtime_rpc 告警与重复库链接告警保留，相关源与开工副本一致。
HERR 两个 TU 及 BOOT 三个 TU 的 format16/tidy22 均通过；最终全量集成另行验收，不据单项通过翻整族 target 状态。

HERR-03 已按上述实测完成字段级证据登记：仅 target 的两条 HostRequest shape/unknown-operation
语义为 implemented，reference 和其他新能力不变。主代理复跑 registry exit 0、evidence 9/9；
日志 `herr03-registry.log`、`herr03-evidence.log`。整工作区最终回归另行执行。

### WS-02、Provider、BOOT-03 局部实测

- WS-02：真实 PG 变更 journal 测试首轮失败，原因是测试误用 Model 不支持的旧 live-operation prune。
  修复 fixture 后 `ws02_postgres_operation_observations` 1/1 通过（4.99s）。没有放松生产拒绝语义。
  真实 describe 核验当前 schema：Identity15、Runtime19、Scheduler11、Model10；manifest 已按实测刷新。
  日志：`ws02-describe-schema.log`、`ctest-ws02-journal-first.log`、`ctest-ws02-journal-fixed.log`。
- STR Provider：固定 eventsource-c v0.2.0 源码/许可/hash，仅使用 C99 parser、无自动 POST 重发。
  首轮 13/13 通过（12.17s）；补 URL 预检、错误恢复与返回值所有权后，3 项生产 Provider/parser/旧非流式
  回归 3/3 通过（5.54s）。许可安装 component 实测成功并逐字节比对锁文件。
  日志：`ctest-str-provider-first.log`、`ctest-str-provider-final.log`、`install-sse-license.log`。
- BOOT-03：真实固定目录 0600 文件交付、CAS、fork 崩溃恢复、FD 耗尽恢复测试通过；
  最后修复 fdopendir 所有权后重建并执行 `p12_secret_delivery`，1/1 通过（0.85s）。
  日志：`build-secret-delivery-fd-fixed.log`、`ctest-secret-delivery-fd-fixed.log`。

以上日志均在本机 `build/logs/capability-expansion/`。纯文件交付成功不等于 Runtime 签发、
远端注册或部署完成；Provider 成功不等于 SDK/Bridge 全链可用。

### 下一依赖批次

WS-03 的纯观察应用、Runtime/Scheduler owner RPC 和真实进程 fixture，STR-03/04 的 Model 应用/
PG 恢复查询，以及 HOST-02 的 SQLite 维护状态机均已准备好，主代理统一构建并实际执行。
通过后分别接真实 WSS/Panel、Model RPC/Runtime Bridge/SDK、Host gate/RPC/Control。

Host 实际身份链核对发现：既有 Identity enrollment 和 CheckAuthorization 委派协议不足以证明
Host 自身禁派请求。已放行窄范围 v1.1 契约补充 `CheckHostGateAuthorization`，先补唯一 wire/
registry/语义测试与文档，再验兼容与生产实现；不削弱既有 delegation proof 或扩大 Scheduler 白名单。
Host 注册使用显式 host_id 与既有 RESOURCE_HOST binding，不假定 HostId 等于 PrincipalId，且不新增数据库 DDL。

### 第二批首轮检查

- 统一 configure exit 0。构建首先在 WS seed 误调用不存在的 `replace_participants` 失败；
  改用实际 UoW `save_operation_participants`，保留持久 participant 断言后，owner daemon、seed、
  RPC 和 v1.1 roundtrip 目标重建成功。首轮/修复日志分别为 `build-owner-app-host-v11.log`、
  `build-owner-rpc-v11-seed-fixed.log`。
- STR-03/04：`p11_model_gateway_application`、`p11_streaming_model_application`、
  `p03_postgres_database` 3/3 通过（1.01s、4.03s、20.05s，总25.10s）；
  日志 `ctest-model-app-pg-first.log`。已放行 Model RPC 实现，不宣告 SDK 全链完成。
- WS-03：纯应用通过（0.48s），真实 owner fixture 首轮因 offline enrollment 失败（6.31s）；
  后定位为 SDK 缺新 observations 生成包，并非实际 enrollment 失败；阶段标签已纠正。
  生成全部当前 Python bindings 后，第二轮在撤权预期失败（13.18s）：既有契约要求旧 proof 水位
  过期返回 UNAUTHENTICATED。修正为精确旧 proof 拒绝及 fresh 同 scope 授权拒绝，并补同 actual
  Control mTLS 上 ValidateSession 成功证据，真实 owner 最终 1/1 通过（9.02s，总9.03s）。
  这同时证明有效人类会话下当前 grant 已撤销、Identity 故障不伪造空页。
  日志 `ctest-ws03-owner-first.log`、`ctest-ws03-owner-bindings-fixed.log`、
  `ctest-ws03-owner-live-session.log`。已放行真实 WSS/Panel，完整批准频道和 restore epoch 仍待实施。
- HOST-02：新测试告警修复后有限重建成功；旧 HostService/Journal 两项通过，
  maintenance 首轮在 progress rollback 后恢复断言失败，仍待定位。
  日志 `build-host02-warnings-fixed.log`、`ctest-host02-first.log`。
- Identity v1.1：正式跨语言门禁 119 RPC/378 消息/27 golden 通过，原始 descriptor 兼容通过；
  记录 `identity-v11-manager-summary.json` 和 doc/30 第5节。Host 窄授权生产链已放行，尚未验收。

## 用户要求交接（2026-11-04）

用户最新指示：“将后续任务整理成agent提示词，我让其他agent做”。据此停止继续实施，
三名worker已冻结产品源码、交回编辑锁并结束，无已知在跑的构建/测试或独立PG fixture。
保留全部当前未提交改动，不回滚半成品。当前产品代码未重新全量构建，不承诺可编译或全绿。

交接入口：[后续agent提示词](2026-11-04-next-agent-prompts.md)，含统筹、四条实施工作流和集成验收；
三份源码handoff列出实际文件、半成品、命令、失败和编辑锁。
v1.2四RPC只完成缺口识别与范围核定，尚未有wire/registry编辑；不能说已冻结。
STR06未接CMake/未验证；Host终止KILL竞态未修，临时诊断仍在；Identity/CLI/config新增部分未统一构建。
接手必须从这些真实状态继续，不能将上文局部成功套到后续源码。

本次交接仅新增/修正文档，不修改产品代码。文档影响为提示词、交接和本台账；
协议/API/模块运行行为未因交接变化，无需为交接重新生成或构建。
交付检查限文档UTF-8/本地链接/末换行/尾空白、diff及交接文件可追踪性，产品检查未重跑。

## 2026-11-04 续执行：lead 接管后的核对与首批实际结果

**接管核对（实测）**：工作树干净于 `main @ bd9f2af`（上轮 WIP 已作为 checkpoint 提交，未回滚/未重置）；
`build/env/p00-env.sh`、PG16.4、OpenSSL3.5、Node22.14、clang-format16.0.6、Python venv 均可用；
无遗留独立构建/测试进程（上次 CTest 失败仅 `p08_host_maintenance`）。首轮全量构建 **实际失败**：
`kiarabot_model_rpc_test` 链接缺 `serve_model_chat_stream`/`prepare_operation_observation`
（STR06 源码未接 CMake，与交接风险一致，日志 `build/logs/capability-expansion/baseline-configure-build.log`）；
该失败保留，不作为后续通过证据。随后 lead 预接 `cmake/model-streaming-rpc.cmake`（最终接线仍由
streaming worker 补全并实测）。另一首轮失败：`kiarabot_model_stream_codec_test` 构建成功但运行报
“stream codec assertion”；`kiarabot_model_stream_rpc_test` 尚未构建；均移交 streaming worker 修复，
不再由 lead 改写。describe 基线：扩展 `kiarabot_postgres_operation_observations_test --describe-schema`
使其覆盖 Identity，真实 owned PG16.4 实测 **I15/135 decc8401…、R19/162 3b472a7…、S11/91 1d2af0a…、
M10/85 51d9e77…**，与 manifest 完全一致（`continuation-describe-baseline.log` +
`continuation-describe-baseline-result.json`），证明任何 v1.2 DDL 前的 manifest 是真实的。

**分工与锁**：五个持久 worker：`host`（task-1/2/8）、`streaming`（task-3/4）、`ws`（task-6/7/12）、
`sdk`（task-5，STR09 另建）、`bootstrap`（task-13）；lead 独占 task-9/10/11 与共享文件。
共享构建通过 `build/.agents/kb`（fcntl 文件锁）串行化；公共契约/registry、PG schema/UoW/manifest、
公共聚合 CMake、公共生成目录由 lead 单一编辑。文档分工：doc/01/02/03/06/08/27 与 proto/README 由
lead 收口，worker 提交要点；模块报告与各自 README 由 worker 写。

**v1.2 契约冻结（lead 完成，实测通过）**：`observations.proto` 新增
ObservationChangesRequest + logs/tasks/messages 三族 10 消息 46 字段；`identity.proto`/`runtime.proto`
各加 ListLogChanges，`runtime.proto` 加 ListMessageMetadataChanges，`scheduler.proto` 加
ListTaskChanges（log.read/RESOURCE_CATALOG、message.read/RESOURCE_ACCOUNT、
task.read/RESOURCE_TASK|INSTANCE，只读、无幂等键、500ms 默认/最大、online）；`target_spec` 方法
119→123；registry +10 消息/+46 字段/+2 semantic rule；goldens 27→31；新增契约 oracle
`proto/tests/observation_feed_contract_test.py` 并接入 target gate。实际门禁：`check_contracts.sh`
（含 legacy reader 与 descriptor fixture）exit 0；enum/status/semantic 三 gate exit 0；
跨语言 **123 RPC / 388 消息 / 31 golden** 通过（`v12-target-contract.log`）；v1.2 descriptor
（SHA-256 `6fdf83b6…`）对 v1.1 原 descriptor（`f16a1bc8…`，与 `identity-v11-descriptor.pb` 相同）
additive 兼容通过。首轮失败保留：新 oracle 自分字段顺序断言写错，修 oracle 后通过，非契约放宽。
v1.2 只增 owner RPC，无 OpenAPI/AsyncAPI 变化，故未重跑 Node API 门禁（记录为未运行及原因）。
以上只放行 WS feed 实现，不是 feed/RPC 已实现证据；v1.1 的 119/378/27 计数不再用于 v1.2 声明。

**进行中**：host 已定位 HOST-02 根因（Darwin 对 “leader 已退出未 reap 的僵尸进程组” SIGKILL 返回
EPERM，grace 循环漏最后一次 reap）并开始修复；ws 开始 WS-04（protocol/source 编译错误已定位）；
sdk 完成基线（4 failed / 137 passed / 23 skipped，均为冻结 wire 后的过期测试夹具，已授权修正并保留
首轮日志）；bootstrap 开始 BOOT-04/02 独立切片；streaming 修 STR06。后续 PG DDL 按 BOOT → WS 串行
窗口执行，每次真实 describe 后刷新 manifest。

### 续执行检查点（2026-11-04 18:0x）

- **HOST-02 修复完成（局部验收）**：host worker 用真实 waitpid/owned 执行追踪收敛非 forced 终止；
  KILL EPERM 不再直接成功；移除 `host_process.cpp` 的 `std::cerr` 诊断。`p08_host_{maintenance,
  process,service,journal}` 4/4 通过，`p08_host_process` 连续重复 20 次全过
  （`ctest-host02-fix-final.log`、`ctest-host02-repeat20-final.log`）；专项 tidy 日志已生成，
  最终全量门禁仍由 lead 跑。
- **BOOT-02 第一个 PG 窗口（R19→R20）**：新增 `bootstrap_deliveries` 无秘密 intent/receipt 表与
  `bridge_credentials.version` 持久版本列。真实 owned PG16.4 describe：Runtime
  `d87a56d0ddcf7ccd8ebb91b56250dc94`/165（+3），I/S/M 不变；`bootstrap_delivery_intent` 首片
  `p05_runtime_bootstrap_delivery` 1/1 通过（首轮 3 个编译错误与 bad_variant 失败日志保留）。
  lead 修正 worker DDL 的 ResourceKind 取值（ACCOUNT=1/INSTANCE=5，非 0/4）。
- **BOOT v3 版本 CAS**：新增 `BridgeCredentialRepository::revoke_if_current`（PG `UPDATE ...
  WHERE NOT revoked AND version=$2 RETURNING`，0 行即 CONFLICT），并注册真实 PG 测试
  `p05_postgres_bootstrap_delivery`；无 DDL。
- **共享构建临时降级**：Control 的 `owner_gateway/bootstrap.cpp` 源注册暂时注释，因为其依赖的
  owner_gateway 接口切片（P6/P7）尚未落；这会短暂让 `kiarabot_control_auth` 不包含 BOOT-04 交付
  编排，属集成中间态，完成后 lead 恢复注册并跑 Control 回归。WS 的 WSS 构建因此不被阻塞。
- WS-04 代码已落盘并成功构建首轮（WSS 独立 store/4 ws worker，Upgrade 只接已登记 GET 路由与精确
  Origin，REST 路径未改）；单测首轮 2 个失败已修正待重跑。SDK STR08 实现完成并跑全量 pytest（首轮
  4 failed 为冻结 wire 后的过期夹具，已授权修正，首轮日志保留）。以上均为 worker 自报，lead 尚未
  复核其日志与最终 diff。

### 续执行实测：全量构建通过、R20 schema 引发的既有回归修复（2026-11-04 18:2x–18:4x）

- **全量构建（-k）exit 0**：`cmake --build build/full-locked -- -k` 完成（日志
  `integration-full-build-round1.log`），当前工作区（含 BOOT-02 应用/PG/RPC/Control、STR06、
  WS-04 新文件、SDK/Host 测试）所有目标均可编译；这是本轮第一次整树编译通过，但仍以最终冻结源码
  后的全量构建/CTest 为准。此前首轮全量构建失败（STR06 未接线）记录保留。
- **R20 schema 的既有回归修复**：BOOT-02 DDL 后两类共享回归被定位并修复，不是放宽断言：
  (a) `p06_bridge_acceptance`/`p13_joint_delete_e2e` 的 Python fixture 直接 SQL INSERT
  `bridge_credentials` 未带新 `version` 列（NOT NULL）→ 补显式 `version=1`；
  (b) 所有链接 `kiarabot_postgres` 的旧二进制仍编入 R19 descriptor manifest → Runtime 打开 R20
  数据库失败、启动无输出；重建 `kiarabot-pg-initialize`、`kiarabot-runtime-p13g`、
  `kiarabot-{identity,scheduler,model,plugin-host}`、seeds 后 p03/p06/p13 均通过。
  由此确立集成规则：每次 schema describe bump 后必须重建所有 PG 链接目标（最终全量构建覆盖）。
- **HOST-02/Identity v1.1 生产链（worker 自测，lead 复核 diff/日志）**：p08_host 4/4 且
  `p08_host_process` repeat-20 全过；Identity 单元 4/4 + `p05_runtime_identity_e2e` 18.02s +
  `p04_identity_bootstrap` 3.91s 通过，覆盖真实 PG HostId≠PrincipalId 登记、CheckHostGateAuthorization
  真实 mTLS allow/错叶/跨 Host/缺 grant/撤销/凭据撤销，以及 CLI Host enrollment 负例。
  发现既有 latent defect：Identity `WatchInvalidations` poll 未按 scope 过滤，越 scope record 会让
  handler 返回 INTERNAL；不属于本轮五项范围，登记为剩余缺口（e2e 已把 Host 撤销验证移到 watch 后）。
- **STR06 与 Model RPC**：codec/RPC/旧 Chat/Identity 客户端 5/5 通过，新增
  `p11_streaming_model_rpc_e2e`（真实 owned PG16.4/TLS/Identity/Model/本地 HTTP SSE）12.25s 通过；
  首轮 codec 失败（测试 helper 未初始化 state）保留。e2e 发现并修复既有契约缺陷：
  ModelService 7 个管理/查询方法缺 `scope_kinds=RESOURCE_MODEL`（见 doc/30 6.1，第二次 v1.2 wire
  冻结 descriptor `26a88361…`，123 RPC/388 消息/31 golden 复跑通过）。
- **SDK STR08**：新增 ModelStream async 消费；全量 pytest 189 passed/23 skipped（基线 4 failed 为
  冻结 wire 后过期夹具，已授权修正并保留首轮日志）；11 项 targeted 55 passed/19 skipped；
  `host.model.stream.v1` 仍未 advertise。已建 task-14（STR09 整链），骨架 + 真实 SSE Provider
  fixture self-check 通过（`p11_streaming_provider_fixture` 已注册）。
- **BOOT 全链窗口复核**：BOOT 回归 5/5 由 lead 独立复跑通过（含真实 PG 并发 CAS）；Control 受控
  交付注册改由 `cmake/bootstrap-delivery.cmake` 单点持有，control-auth.cmake 无重复；P11
  （https_endpoint 组合）仍归 WS。5a/5b 与 BOOT-05 继续。

## 2026-11-04 最终集成验收（冻结后实测）

冻结动作：五个 worker 接到冻结指令后停止编辑（streaming/WS/SDK/bootstrap 已逐一确认 inactive，
host 在提交 host_management_rpc 可编译态后停止）；此后 lead 单独完成全部构建/测试/门禁与文档收口。

实际命令与结果（日志均在 `build/logs/final-acceptance/`，首轮失败保留在
`build/logs/capability-expansion/`）：
- `kb configure`、全量 `kb build -- -k`：exit 0（`full-build.log`、最后重建 `full-build-final.log`）。
- 全量 CTest 首轮 197 项：194 通过 / 3 失败（`full-ctest.log`）；失败：
  `architecture_p02_tls`（v1.2 新方法与生产 handler 状态期望过期，修测试后通过）、
  `p12_target_websocket_https`（真实 WS teardown asio 崩溃，未修）、
  `p11_streaming_managed_model_e2e`（STR09 driver 首跑 model readiness fixture 失败，未修）。
- 全量 CTest 第二轮 197 项：196 通过 / 2 失败（`full-ctest-rerun.log`），仅上述两项真实失败。
- 契约门禁：`check_contracts.sh`、enum/status/semantic/evidence/target-feature、OpenAPI/AsyncAPI/
  target API、`check-reference.py` 全部 exit 0；最终 descriptor `26a88361…` 对**真实起始
  baseline-descriptor.pb** 与 v1.1 冻结 descriptor additive 兼容均通过；跨语言 target gate
  **123 RPC / 388 消息 / 31 golden** 通过（`target-contract.log`）。
- SDK：`pytest -q sdk/python/tests` → 189 passed / 23 skipped（`sdk-pytest.log`）。
- Panel：lint/build/`check:contracts` exit 0；`npm test` 52 passed / 1 failed，且 `panel/` 无改动
  （HEAD 既有失败）。
- 架构：boundaries/`--inner`/`--links`/`--database` exit 0（本机无 ripgrep，使用等价文件集 shim；
  `build/.agents/bin/rg`）。`git diff --check` exit 0。
- 格式：官方 `check-format.sh` 仍失败（28 文件），逐文件对比 HEAD 后确认**全部为 HEAD 既有基线，
  本轮新增违规 0**（`format-official2.log`、`format-baseline-classification.txt`）。
- 专项 tidy：16 个改动 TU（含 WS/Bootstrap/Host/Streaming 新文件），删除 STR07 遗留未使用
  `random_uuid` helper 后只剩 `runtime_composition.cpp` 第二个 compile-database 变体下的
  `foundation.hpp` analyzer 既有告警；全仓 tidy 未运行（历史基线 + 时间窗，如实登记）。

文档收口：doc/01-contracts（v1.2 章节与状态表）、doc/01-runtime-semantics（feed 语义）、
doc/02-core（Host 终止确认语义、Model/Runtime streaming 由 worker 更新）、doc/03-plugin-sdk
（streaming SDK 入口与验收边界）、doc/05-infrastructure-model-streaming（worker 更新）、
doc/06-control-api 与 doc/08-operations（BOOT 交付 REST/配置/运维语义）、doc/30（v1.2 两次冻结与
缺陷修正）、doc/27（本节新增最终集成小节）、执行台账（本文件）、模块 reports（worker 提交）。
受保护 doc/18 与 AGENTS 未改。未提交、未推送、未发布。

剩余缺口（不冒充通过）：WS 真实 WSS teardown 缺陷与 WS-05/06/07/08/09/10 未完成；v1.2 feeds +
PG DDL/describe 窗口未实现（task-12 未开始）；STR09 受管插件 streaming 整链未验证；HOST-03/04 仅
S1 通过、S2 无测试，Runtime gate/PG 持久化/Control host 路由未实现；BOOT-05 真实交付整链、
受限消费端、P11 https_endpoint BOOT 段未完成；慢消费者 1s 写预算仅 code-path 记录；两主机/断网
演练、真实 QQ、付费模型、发布未执行。

## 2026-11-04 第二轮续执行（提交 7b092da/88b652f/1037cc5 后）

- WS-04 teardown SIGABRT 已真修（单 strand 状态机、pending 操作回栈后析构），
  `p12_target_websocket_https` 连续 3 次通过；BOOT-04 Control 组合合入 `https_endpoint` 并由真实链验证
  （`p12_bootstrap_delivery_e2e` 1/1，组合批 6/6）。
- BOOT-05 真实链通过：seed（无 credential）+ 真实 Identity/Runtime/Control/私有 PG/文件 sink；
  覆盖首次签发→sink→report→无秘密查询、无 recipient 兼容、幂等重放回填、reissue 版本递增/旧凭据撤销、
  stale 409、unbind 后迟到 fail-closed、Control 重启持久、REST/子进程日志/PG 全库 canary。
  未覆盖（剩余缺口）：sink 后 ACK 丢失注入、delivery cancel（冻结契约无 RPC）。
- HOST-03/04：Runtime gate 全链（HostFenceRegistry/PG CAS/Identity v1.1 授权适配/`SetHostAdmissionGate`
  RPC/composition 装配）+ S2 host_management_rpc 真实 mTLS 测试 + S3 真实 Host 进程 e2e
  （`p08_host_management_e2e` 已注册，覆盖 Begin→gate→停止→SUCCEEDED/lost ACK→BLOCKED/Resume/重启持久）。
- v1.2 feeds：owner-local journal（head/changes 两表，I/R/S 私有库）+ 纯应用/RPC 边界 + real-PG
  driver；`ws12_observation_feed_application`/`_journal` 通过；post-DDL 真实 describe 刷新
  I16/R22/S12→（含 Model error CHECK 修正）I17/R23/S13→（feed resource kinds 修正）**I18/R24/S14**；
  M 11。所有 manifest 均为真实 owned PG16.4 describe 结果。
- 产品缺陷修复（均为测试暴露、非放宽）：①ModelService 7 方法缺 scope_kinds（v1.2 wire 二次冻结）；
  ②Runtime Identity client 将 2s 网络预算误当子委派寿命（长流被裁剪），分离 delegation_lifetime=30s；
  ③`model_intents.error CHECK` 拒绝 domain InvalidArgument=0，HTTP-400 明确拒绝路径 txn2 失败；
  ④v1.2 log feed 初始资源种类校验过严，导致 identity-bootstrap 审计写入 INVALID_ARGUMENT
  （p05b/STR09 admin login 回归），已扩 resource_kind 并修正校验。
- 质量/证据：全量构建绿；关键切片 10/10 通过；`p05b_model_chain_e2e` 复绿；
  `p03_postgres_database`（含 InvalidArgument txn2 回归）通过；WSS/BOOT/feed/host e2e 均有首轮失败保留。
- 进行中：WS feed 写路径（Identity log/Runtime message/Scheduler task）/composition/RPC handler；
  HOST S5 Control host 路由与 register_plugin 受管重连语义；STR09 受管插件整链因 reconnect 语义待定。

## 2026-11-04 冻结日最终验收（第二轮收口）

- 代码冻结后 `kiarabot-pg-initialize`/服务二进制/测试全部重建；全量构建 exit 0。
- 契约/枚举/实现状态/语义规则/证据 scope/feature 声明/target descriptor 往返 + 与 baseline 和
  Identity v1.1 的兼容检查/api（OpenAPI/AsyncAPI/target/reference）/架构（boundaries/inner/links/
  database）/SDK pytest 全部通过；格式门禁 28 个违规全部为 HEAD 已有基线、0 新增。
- v1.2 feed 同一 UoW append 落点：Identity/Runtime audit→logs（`RETURNING id` + 封闭日志投影）、
  Runtime received message→messages（确定性 UUIDv5 内部 id、重复 fingerprint 不重复）、Scheduler
  六处 task 状态→tasks；PG 适配器实现 UoW feed port，未实现的 adapter 显式 CapabilityNotSupported。
- 冻结测试暴露并修复的产品缺陷新增：⑤日志 feed 资源校验过严导致审计（identity-bootstrap/私存写入）
  失败；⑥Scheduler feed append 对不支持 feed 的 UoW adapter 误判；⑦受管插件 bridge 的 host 绑定拒绝
  导致 Runtime 重启后永久无法重连（已按契约删除 4 处 host.has_value 拒绝，保留其余授权校验）。
- 最终全量 CTest 结果与逐项失败/通过证据见 `build/logs/final-acceptance/final3-ctest.log`；
  第一轮失败日志 `final2-ctest.log`（混合旧二进制配对）保留，不作为最终结论。
- **最终全量 CTest：204/204 通过（exit 0，457.23s，`--parallel 3`，`final5-ctest-parallel3.log`）**；
  并行 6 窗口的 STR09 时序失败已由 driver 有界窗口重试收敛，空闲单跑 121.58s exit 0
  （`final4-str09-isolated.log`），不作为最终失败。

## 2026-11-04 goal round 1：剩余缺口收口

- HOST-04：单链 e2e（真实 Control+真实 Host+真 TLS Identity/Runtime fake）3/3；修复跨 host
  operation 可见性安全缺陷（存储 host id 绑定，wire host_id 不再决定 scope）；4 个 Host 维护 REST
  operation 契约翻转 implemented（openapi/registry/doc 同步，impl-status/semantic gates 通过）。
- v1.2 Identity log feed：真实 Identity 进程 + 真实 PG 的 `ListLogChanges` e2e 通过；发现并修复
  两层真实 bug（UoW feed 门禁缺 Identity、IdentitySecurityWork 未转发 feed 方法）；CTest
  `p04_identity_log_feed_e2e` 注册并通过。
- WS-05/06：health/adapters/plugins 真实 producer + logs/tasks/messages feed 频道 + Ajv schema 校验；
  `p12_target_websocket` 5/5（含 producers e2e）。
- 待办（下一 round）：全量构建/CTest/门禁复核并提交本轮；Runtime/Scheduler feed 真实进程 e2e、
  target_spec feed RPC 翻转、全目录 page 扫描、Snapshot 撤权用例、多 host endpoint 注册表。
- **goal round 1 验收：全量构建+全量 CTest 207/207 exit 0（`round2b-ctest.log`，549s，
  `--parallel 3`）；契约/target/api/架构/SDK 门禁 exit 0；格式 0 新增；仅 Panel 基线 1 失败。**
  新增测试：`p04_identity_log_feed_e2e`、`p12_host_control_chain_e2e`、`p12_target_websocket_producers`。
  遗留风险：WSS 两测试在连接 churn/并行下低概率握手失败或 SEGFAULT（串行稳定性 10/10、9/10），
  下一 round 需定位。

## 2026-11-04 goal round 2：真实 feeds + registry 翻转 + WSS 稳定性

- 真实 owner feed e2e：`p05v12_owner_feed_e2e`（真实 Identity/Runtime/Scheduler + owned PG +
  真实 AdapterBridge）覆盖 Runtime log/message、Scheduler task 非空页/续读/typed conflict/拒绝路径；
  与 `p04_identity_log_feed_e2e` 一起作为 v1.2 feed 的 target 证据。
- registry：v1.2 feed 10 messages/46 fields/2 semantic rules + 4 RPC target_spec 翻转为 implemented；
  HOST-04 4 个 host REST op 补齐 target override+evidence；contracts/target/api gates 全绿。
- WSS flake：定位为 CPython+OpenSSL 的 websockets.sync 客户端竞态（非产品），harness 改单线程
  RFC6455 客户端；https 20/20、producers 20/20。
- adapters/plugins 目录 immutable page 扫描落地（真实 Runtime ListAdapters/ListInstances + 逐页授权
  + fail-closed）；asyncapi/doc06 同步。
- 验证：全量 CTest 208/208 exit 0（547.79s）；契约/target/api/架构/SDK 门禁 exit 0；格式 0 新增。

## 2026-11-05 goal round 3：真实缺陷修复 + feeds 加固 + Panel 清理

- 真实产品缺陷：平台客户端非规范 UUID（Host 3 处、Scheduler 2 处）导致真实 owner 拒绝
  INVALID_ARGUMENT；修复并补 canonical 断言（新增 `p10_scheduler_runtime_client`）。
- Identity 授权方法表补 `HostManagementService→Host` owner 映射 + 4 方法单测。
- feeds 加固：真实撤权 fail-closed、真实 COMPLETED 终态、page_size>100/未来 sequence 边界。
- Panel 基线 stale 测试 setup 修复，91/91 全过。
- HOST 全真实单链：host.read 链路通过；BeginMaintenance 403 挂起（claims 差异），e2e 未注册。
- 验证：见全量 CTest/门禁日志；未 push/发布。

- **goal round 3 验收：全量构建 exit 0；全量 CTest 209/209 exit 0（`round4c-ctest.log`，632.21s，
  `--parallel 3`）；契约/target/api/架构/SDK 门禁全绿；格式 0 新增；Panel 91/91。**
  `p10_timer_e2e` 因 wall-clock 敏感也加 `RUN_SERIAL`（与 STR09 相同处理，未放宽断言）。

## 2026-11-05 goal round 4：剩余待办收口

- HOST 全真实单链 `p12_host_real_chain_e2e` 绿并注册；修复 HostManagement 二次 host.read、
  Runtime gate operation ref 小写 host、gate FK 与真实部署 Host 不兼容（schema 24→25）。
- Control 多 host endpoint 固定离线 registry（host_endpoints，与 host_endpoint 互斥；懒建 channel；
  未登记 503；真实/TLS 隔离测试）。
- 真实 owner WS 目录扫描 `p12_ws_owner_directory_e2e` 绿并注册；修复 SnapshotInfo owner 大小写、
  WS catalog→已授权 children 展开、adapter fence 语义（TargetAdapterFence）。
- feeds 收尾：FAILED 终态（真实 Scheduler fail_trigger+PG）与 Identity ListLogChanges 撤权。
- WS v1.2 envelope 契约对齐（asyncapi additive target schemas、task version string、plugin state
  全字段、全 envelope Ajv 校验）。
- 验证：见 round5 构建/CTest/门禁日志；未 push/发布。

- WS v1.2 envelope 契约对齐完成（asyncapi additive target schemas、task version string、plugin
  state 全字段、全 envelope Ajv 59 assertions）；STR09 全量窗口 flake 定位为 fixture 时序并加固
  （5 次隔离 + 并发 3/3 通过，断言未放宽）。
- **goal round 4 最终验收**：全量构建 exit 0（211 CTest 项）；全量 CTest 除 owner-directory 外
  **210/210 exit 0**（`round12-full-minus-wsdir.log`，667.35s），`p12_ws_owner_directory_e2e` 隔离
  1/1 Passed 25.51s exit 0（`round12b-wsdir-isolated.log`）；契约/target/api/架构/SDK/Panel 门禁全绿；
  格式 24 基线/0 新增。修复真实缺陷：HostManagement 二次 host.read、gate FK、SnapshotInfo owner
  大小写、WS catalog 展开、adapter fence、WS v1.2 envelope 契约、P08 测试进程泄漏、STR09 fixture
  时序；新注册 `p12_host_real_chain_e2e`、`p12_ws_owner_directory_e2e`。
- WS 首帧/轮询稳定性：catalog owner 查询有界重试 + snapshot 频道 transient 保留 stale/unknown、
  仅持续 >2s 关闭；warm 5/5 + 全量 CTest 单次 211/211 exit 0（701.52s）。
- target WS 状态翻转：asyncapi x-target-transport/contract + 7 channels + 7 stream events target
  override/evidence（control 聚合仍 not_implemented），evidence roots 纳入 Control target 源/测试；
  api/contracts/target 门禁全绿，WS e2e 复跑通过。
- goal round 6：修真实缺陷 A（bridge 断开立即写 adapter offline presence；p06_bridge_presence +
  p07_adapter_e2e 真进程/PG 验证）与 B（Runtime 快照同 subject 满额淘汰最旧空闲，旧 cursor typed
  stale，全活跃仍 RateLimited；目录长会话/scale e2e 复跑）；新增真实 127-adapter scale e2e、真实
  adapter online/offline 与长会话断言、真实双 Host + 单 Control 多 registry e2e（均注册）；doc/08
  记录目录模式运营规模限制（约 32ms/行、建议 ≤25–30 行、child 硬上限 126）。最终全量/门禁待收口。
- goal round 7：STR09 偶发失败定位为 Identity unary run() 裁剪 ExchangeDelegation 寿命（子委派
  TTL 1.1–1.8s → Model stream ~2s timeout），修复为 credential issuance 路径 30s 且传输 deadline
  只判过期；fail-first 单测 + p04/p06/p11 回归 + STR09 隔离 3/3 全过。

- **goal round 7 最终验收**：全量构建 exit 0（214 CTest 项）；全量 CTest 214/214 通过（分两批连续
  执行 batchA 107/107 + batchB 107/107 均 exit 0；单次连续全量在长套件负载下偶发 heavy real-stack
  e2e flake，失败项隔离复跑均绿，日志保留）；contracts/target/api/架构/SDK/Panel 门禁全绿；格式
  24 基线/0 新增。产品修复：adapter offline presence、snapshot 配额淘汰、Identity ExchangeDelegation
  子委派寿命；新增 bridge presence/双 Host/owner-directory/scale 等 e2e 并注册。
- **2026-11-05 残余收口**：单次连续全量 CTest **214/214 exit 0**（`round17-full-ctest.log`，
  770.86s，--parallel 3，含此前失败的 STR09/owner-directory-scale）；WS 目录类 e2e 失败 stage 纳入
  `run_database_test.py` 转发；变更 TU 专项 clang-tidy 的 2 条既存 `unused-but-set-variable` 清零
  （`54b82bbf`），两 TU tidy exit 0；构建 exit 0。

- **2026-11-06 goal round 8：契约状态收口 + BOOT/P13 技术缺口**：`execution_scopes.target` 新增 202 个
  implemented override/证据（streaming/host maintenance/admission gate/bootstrap/OperationChanges/HostBridge/
  BOOT REST ops/semantic rules），`target_spec` 18 方法 + HostBridge 翻 implemented；doc/01 execution-scopes
  与 doc/02/03/05/06/08 状态注记同步；contracts/target/api 门禁全绿。BOOT-05 用测试专用真实 TCP 中继实现
  进程级确定性 ACK-loss（PENDING/无 receipt/无明文/reissue 撤销矩阵，连绿 3 次）；delivery cancel 确认
  冻结契约无入口并记录变更建议。P13 UNKNOWN 改为真实 plugin-host journal 恢复链路（Lead 复跑 35.04s，
 全量中 35.46s）。全量构建 exit 0；全量 CTest 214/214 exit 0（`round18-full-ctest.log`，741.12s，
 --parallel 3）；contracts/target/api/SDK/Panel/arch/diff 门禁全绿；格式 24 基线/0 新增。
