# 五项扩展执行台账（2026-11-04）

## 授权与基准

用户在批准五项范围后继续授权：“继续推进契约冻结，完成后按依赖安排 subagent 实现，
你负责统筹和验收，继续使用 6.1 sol high，不用每一步都等我确认。”
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

三名执行者均为 GPT-6.1 Sol / high；共享文件单一编辑者，主代理集成评审。

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
