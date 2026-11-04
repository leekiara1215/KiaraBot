# KiaraBot 实现与阶段验收证据

## 0. 当前证据入口（2026-11-04）

本节区分本轮未提交工作区与已提交历史证据。当前基准为
`0496b90053d046fd61d7f106090bb87fff172f8a`；下述任务变更尚未形成新提交，
各任务的实测只适用于各自测试时的差异，不代表后续集成工作区全绿。
测试名存在或 `ctest --show-only` 成功仅表示注册，必须有重新构建后的执行结果才能记为实测。
本节新增证据的日期为 2026-11-04；下方 P14t 门禁保留 2026-11-03 的提交/日志作用域。

### 五项扩展：契约冻结通过、首批实施中（2026-11-04）

用户已继续授权契约冻结后按依赖实现，由主代理统筹验收、subagent 执行，
无需逐步重复确认。公共契约已按 [冻结记录](30-capability-expansion-contract-freeze.md) 收口：
118 RPC 描述/权限、376 跨语言消息、25 wire golden、26 模型流语义用例、固定 Node22 的
67 HTTP operation、WS 36/Host-bootstrap 38 项边界，以及完整 contracts/真实 descriptor 兼容均通过。
新增 RPC、WS/streaming/Host/bootstrap 能力仍未宣告可用，没有把旧 reference 或既有 `167/167` 当新增功能证据。

实际实现已分派 WS-02 owner 持久变化源、STR parser/Provider/纯流端口、HERR-02 和 BOOT-03
安全文件交付切片。运行验收尚未完成；当前文件/构建归属、实际命令和后续依赖见
[执行台账](plans/2026-11-04-capability-execution-ledger.md)。本轮 proto/API 有文档影响，
已同步 proto README、doc/01/运行语义、Core/SDK、Control/Panel 及相关契约测试/生成清单。
基础设施和运维的新增行为随各实现任务更新，受保护 doc/18 保持原样。

首个实现切片 HERR 已有局部实测：新 HostRequest wire 分类接入目标 PluginBridge 后，
重建 `kiarabot_model_bridge_test` exit 0；`p11_model_bridge` 1/1 通过（3.94s），
覆盖 raw-wire 拒绝/错误关联/无下游调用及继续处理合法请求。HERR/BOOT 五个 TU 格式和专项 tidy 已通过；
该结果不证明 STR、WS、Host 管理或 bootstrap 全链完成，细节和日志见执行台账。

后续局部实测：WS-02 真实 PG journal 1/1 通过（4.99s），当前实测 schema 为 I15/R19/S11/M10；
Provider/parser/依赖及旧非流式首轮 13/13，通过修复后的生产三项回归 3/3（5.54s）；
BOOT-03 固定目录秘密文件交付及故障恢复最终 1/1（0.85s）。首轮失败及修正保留在
[执行台账](plans/2026-11-04-capability-execution-ledger.md)，这些均为局部能力证据。
WS owner RPC、Model 应用与 Host SQLite 切片待统一运行，完整链路和最终集成仍未验收。
Host 实际授权链的窄范围 Identity v1.1 契约补充已通过：119 RPC/378 消息/27 golden，生产身份链继续实施。

HERR-03 已单独收口：只把 target 的 `semantic.host.request_operation_classification` 和
`semantic.host.unknown_operation_error` 按真实 TLS raw-wire、格式与 tidy 证据登记 implemented；
reference 状态和 blob 未改，其他新增能力未随之翻转。主代理复跑 registry 与 evidence 9/9 门禁通过；
最终全工作区集成仍待执行。

### 上一轮：批准范围和任务拆分（2026-11-04）

用户已批准目标 Control WS、模型 streaming、未知 Host 请求错误语义对齐、独立 host operation
管理、bootstrap 安全交付及字段级验收进入计划；两主机/断网演练继续延期。
详见 [本轮决策](plans/user-decisions-2026-11-04.md) 和
[扩展计划](29-approved-capability-expansion-plan.md)。旧记录中 WS 延期、streaming 不做、
Host/bootstrap 只文档化的范围限制由新计划替代；现有代码/契约仍按实际实现状态登记。

主代理已向三名 subagent 分派 WS、STR、HERR/HOST/BOOT 工作流，
首批源码核对、契约提案、任务依赖与文件归属已完成并经主代理收口，合计 34 个任务，
三份子计划和本轮用户决策已归档到可被 Git 跟踪的 `doc/plans/`。
当前五项均为**计划与分工已完成、契约待冻结**，
没有新增功能实现或验收通过声明；本轮计划整理不重跑 C++ 构建/测试，之前的全量与静态检查
结果仍限定于下文原工作区。新增协议/API 在实际契约任务中再更新、生成并校验。

本轮实际检查：`git diff --check` exit 0；Python 临时文档检查脚本验证五份新计划/决策文件
的本地 Markdown 链接、UTF-8、末尾换行、无尾空白和 34 个任务 ID 唯一/完整，exit 0；
逐文件 `git check-ignore` 均 exit 1 且无输出，确认不被忽略。基准 SHA-256 对比确认
既有非计划改动与 `doc/18` 未改变。WS subagent 实际运行 `check-asyncapi.sh` exit 0
（5 条现有提示）和 `check-openapi.sh` exit 0（13 条现有 warning）；未改契约，
这两项不能记作新增 WS 的运行验收。C++/Python/Panel 产品测试和构建未运行：本轮只改计划，
没有产品源码、协议、配置或数据库变化；直接需要同步与无需同步的文档及理由见扩展计划第 6 节。

### 最终主工作区验证

任务 1–4 的最后增量已集成到上述基准加未提交差异，使用本机 macOS arm64 的
`full-locked` 锁定依赖、PG16.4、OpenSSL 3.5.0 和 Python 3.11.18 重新配置/全量构建，均 exit 0。
工作区差异清单和内容摘要由 `build/logs/refactor-integration/integrated-files.txt`、
`integrated-file-manifest.json` 记录，文档收口后统一刷新。
本轮唯一一次最终全量 `ctest --preset full-locked --no-tests=error --output-on-failure`
exit 0，**167/167 通过、无跳过**，外层耗时 1032.63s（CTest 自计 1032.53s）。
这证明本轮新二进制在该本机环境通过注册套件，不是新的提交 SHA、目标部署或正式发布验收。
来源：`build/logs/refactor-integration/commands.jsonl`、`configure-final.log`、`build-final.log`、
`full-ctest-final.log`（第 337 行汇总）、`required-test-registration.json`（167 注册；要求的 27 项全部存在且未禁用）。

边界、strict inner、实际 CMake 链接图、编译数据库覆盖、架构自测和 diff 检查均通过，
日志位于同目录 `boundaries-final.log`、`inner-final.log`、`links-final.log`、`database-final.log`、
`architecture-tests-final.log`、`diff-check-final.log`。66 个改动 C++ 文件完整格式检查通过；
全仓格式仍 exit 1，27 个失败路径均未修改，与本轮改动交集为空，见 `changed-format-final.log`、
`whole-format-final.log`、`format-baseline-final.json`。

最后改动 TU 专项 tidy 为 **37/40 通过、3 个失败**：`core/app/scheduler_composition.cpp`、
`core/tests/platform/scheduler_service_test.cpp`、`infrastructure/postgres/tests/scheduler_application_test.cpp`。
10 次原始 error 诊断（9 个源码位置）均逐条匹配任务 1 基线，新增为 0；
这仍是未通过的专项门禁，不表述成 40/40 或全仓质量全绿。
明细：`build/logs/refactor-integration/changed-tidy/results.json`、`tidy-baseline-comparison.json`。
任务 1 的 251 TU 全量 tidy 保留原结果；未对最终源码重复全 251 TU，也未重跑
`scripts/check-toolchain.sh`，因其会重复 dev/standalone/工具链工作及已知格式/tidy 失败项；
本轮实际检查由锁定全量构建/CTest 和上述显式门禁组成。

### 本轮职责整理与实测

- **任务 1，内层依赖与摘要端口：局部实现与实测完成，完整质量门禁未通过。**
  主工作区 `build/full-locked`（锁定依赖、clang-tidy 22.1.8、clang-format 16.0.6）重新配置/构建通过；
  9 项 CTest 通过，分组 6/6（68.39s）和 3/3（2.95s）。边界自测 7、架构自测 11 通过；
  24 个 INNER 目标登记与链接变异检查、251 个手写翻译单元编译数据库覆盖通过。
  SHA-256 适配器、固定编码/失败回滚测试通过，256 组有定义的旧新输入摘要逐字节一致。
  旧取消路径解引用空 optional 属未定义行为，不能声称该路径旧新字节一致；新代码以显式零字段
  和 golden vector 固定预期编码。职责和命令见 [基础设施](05-infrastructure.md)、[工具链](11-toolchain.md)。
  来源：`build/logs/inner-remediation-validation.log`、`inner-remediation-ctest-final.log`、
  `inner-remediation-extra-ctest-final.log`、`inner-remediation-final-checks.json`。
- **任务 1 的未关闭检查：**19 个改动 C++ 文件格式通过，全仓格式仍因 29 个未改文件失败。
  最终全量 tidy 实际执行 251/251，53 个翻译单元失败；最初串行扫描在 118/251 被停止（exit 143），
  后以相同参数逐单元完整复跑，不能将中断算作通过。Runtime 组合根栈地址逃逸、资源应用未初始化
  诊断在基准源码复现；任务 4 明确所有权和正常拆分后专项复查不再出现，
  不能据诊断消失反推原程序一定有可复现的真实未初始化缺陷。
  来源：`build/logs/inner-remediation-full-tidy-results.json`、`inner-remediation-tidy-summary.json`、
  `inner-remediation-format-baseline.json`、`inner-remediation-tidy-baseline.log`。
- **任务 2，Control owner 请求入口：隔离实测及最终主树全量通过。**
  工作区 `/Users/betta/Worktrees/KiaraBot-control-refactor-20261104`，同一基准及锁定依赖。
  11 个资源族 handler 保留原有路由优先级；入口 Lizard 1.25.0 指标从 length 1660 / CCN 500
  变为 length 29 / CCN 4，不能据此推断所有 handler 或跨函数总复杂度同比下降。
  两项单独批准的修复为 Model disable/enable/resolve 透传既有幂等键，以及四条安装嵌套实例路径
  缺 Runtime owner 时稳定 503；相关精确请求捕获、校验顺序和无副作用负例已通过。
  20,160 条旧新请求差分匹配，比较路由/响应/委托/owner 请求字段；已批准修复单独断言，不要求旧缺陷一致。
  5 项相关 CTest 分组 3/3（33.90s）、2/2（32.91s）；14 个改动翻译单元 tidy、16 个改动 C++ 文件格式通过。
  来源为 worker 最终报告 `/tmp/kiarabot-control-refactor-report.txt`，测试日志
  `/tmp/kiarabot-control-ctest.log`、`/tmp/kiarabot-control-additional-ctest.log`、
  `/tmp/kiarabot-control-tidy-results.json` 和 `/tmp/kiarabot-control-differential.log`。
  临时证据路径仅用于本机复核，不等同已归档发布材料；职责与失败映射见 [Control](06-control-api.md)。
- **任务 3，PluginBridge 准入：隔离回归及含最后 Model 增量的主树全量通过。**
  工作区 `/Users/betta/Worktrees/KiaraBot-bridge-refactor-20261104`，基准同上，
  full-locked 共用只读锁定依赖、PG16.4、OpenSSL 3.5.0、clang-tidy 22.1.8。
  Task/Storage/Model 准入归应用；重复成功 ACK 以持久 attempt 定位复用原条件 ACK 事务，
  当前连接返回 DUPLICATE，错事件/旧 fence/改写旧 attempt fence 保持 STALE，保留期、审计和尝试次数不变。
  9 项相关 CTest 通过（95.09s）；随后 PG/Storage/Bridge 3/3（42.96s），最后 Model 增量 2/2（15.40s）。
  新准入测试还以不链接 gRPC 的 C++20 直接编译执行，拒绝路径下游调用数为零。
  8 个改动翻译单元 analyzer 通过，resource 测试的旧 unused `bridge_flow` 留主集成接回原断言复验，未抑制或删除。
  Connect 的 Lizard 1.25.0 length 937 → 79、CCN 213 → 15；职责与既有 refresh 错误偏差见
  [Core](02-core.md#pluginbridge-传输与应用准入职责2026-11-04)、[SDK](03-plugin-sdk.md#pluginbridge-刷新错误的既有偏差2026-11-04)。
  来源：`/tmp/kiarabot-bridge-refactor-report.md`（SHA256 `2c116562d830db9689349bd8407a93b8559d335bcb2262a7d87106de7645b180`）、
  `/tmp/kiarabot-bridge-lizard.json` 与报告逐条命令/退出码/日志。早期新 Storage 准入缺授权、重复 ACK=STALE
  失败均有记录并修复；refresh INTERNAL 偏差明确保留，不能算成正确契约。
  基准完整格式检查 30 路径失败、改动 hunk 检查通过；30 路径逐个基准源码复现，
  与任务 1 的 29 差异来自后者已格式化 scheduler_rules.hpp，不能拼作当前最终计数。
  主树最后 66 个改动文件格式及 167 项全量 CTest 通过，剩余静态检查限制见本节开头。
- **任务 4，RuntimeResource 组件：已实现，本轮本机构建/全量 CTest 通过。**
  主工作区重新配置和全量构建首轮通过（78.83s）；32 方法 facade 向五类应用委派，
  共享分页组件保持跨资源原额度，Adapter 注册改为事务外在线授权再开写 UoW 重核。
  3 项应用/生命周期单测通过，9 个 Task4 翻译单元专项 tidy 通过；原栈地址逃逸/未初始化诊断
  在明确所有权和正常拆分后局部复查不再出现，不据此声称已证实并修复真实未初始化缺陷。
  源文件 3016 行 → facade 188 行；Lizard 的旧单 TU NLOC 2846/max CCN 57，facade NLOC 151/max CCN 3。
  业务组件 register_adapter 的 CCN 为 62（新增授权后重核），lifecycle maintenance 为 51；
  不能用 facade 指标宣称业务检查或跨函数总复杂度同比消失。
  来源：`build/logs/refactor-integration/commands.jsonl`、`build-first.log`、
  `resource-complexity.json` 与 `build/logs/resource-tidy/results.json`；最后集成报告为
  `/tmp/kiarabot-resource-integration-report.txt`，最终命令以本节开头的主树日志为准。

### 主工作区首轮失败、修复与复跑

主树专项首轮 7/10（211.71s），失败为 `p10_timer_e2e`（fixture）、
`p05_runtime_catalog_e2e`（deadline）、`p13_joint_delete_e2e`（GetOperation ABORTED/CONFLICT）。
首轮记录保留，不能用隔离工作树通过覆盖。
命令、环境及失败全文在 `build/logs/refactor-integration/commands.jsonl` 与
`build/logs/refactor-integration/resource-regression-first.log`。
Control/Bridge 专项首轮为 13/15（254.32s），`p05b_task_chain_e2e` 与
`p06_postgres_runtime_events` 失败，外层 fixture 脱敏输出不构成根因结论；
来源为 `build/logs/refactor-integration/control-bridge-regression-first.log`。
该首轮正则还含两个无当前注册的旧名，15 项结果不证明旧名对应能力已覆盖；
最后已按当前 27 个要求名逐项检查存在且未禁用，并在最终 167 全量中实际执行。
按 CTest 原命令/环境/工作目录、无重型工具争用时单独复跑，p10 55.71s、catalog 9.75s、
taskchain 26.30s、joint delete 37.02s 均通过，最终全量也通过；这四项的首次失败原因仍未确定，
不据复跑通过宣称已定因 flake。对应 `*-diagnostic-exact.log` 与 `p13-joint-diagnostic-no-tool-load.log`
保留在同目录；初次诊断 wrapper 解释器错误的失败也保留于命令台账，不充作有效业务复跑。

PG events 失败可重复，根因已核定：任务 1 注入真实 SHA-256 后，旧测试 `FakeRuntimeSecrets`
对空输入给全零，而请求与断言使用真实 SHA256(empty)=`e3b0…`，导致 Timer 接收 CONFLICT。
最小修复只使该测试 fake 委派既有 `identity_crypto::Sha256` 并给该测试 PRIVATE 链接；
生产逻辑、真实摘要及持久接收断言未改变。修复后单跑 1/1（3.34s）、最后有限回归 5/5（5.65s）
及最终 167 全量均通过。证据：`p06_postgres_runtime_events-diagnostic-exact.log`、
`p06-postgres-runtime-events-fixed.log`、`final-finite-regression.log` 和完整命令台账。

本轮未重建目标 Compose，未访问真实 QQ、付费 Provider 或远端 CI/release；
这些目标环境验收不能引用本机全量或下方历史门禁替代。

上述局部实测的复核命令（在各自工作区执行，配置细节和退出码见对应日志）：

```bash
# 任务 1：主工作区，锁定环境见 doc/11
cmake --preset full-locked -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build --preset full-locked --parallel 4
python3 scripts/quality/tests/boundaries_test.py
python3 scripts/quality/tests/architecture_test.py
python3 scripts/quality/check-boundaries.py
python3 scripts/quality/check-architecture.py --inner
python3 scripts/quality/check-architecture.py --links build/full-locked/architecture-targets.json
python3 scripts/quality/check-architecture.py --database build/full-locked/compile_commands.json
ctest --preset full-locked -R '^(p10_scheduler_service|p10_scheduler_rpc|p10_timer_e2e|p11_model_http_provider|p05_runtime_resource_application|p08_host_journal)$' --no-tests=error --output-on-failure
ctest --preset full-locked -R '^(p02_control_application|p10_postgres_scheduler|architecture_sha256)$' --no-tests=error --output-on-failure
# 任务 2：Control 隔离工作树，最终源码和依赖参数见 worker 报告
cmake --build --preset full-locked --target kiarabot_control_owner_gateway_test kiarabot-control --parallel 1
ctest --preset full-locked -R '^(p12_control_owner_gateway|p04_control_auth_https|p10_scheduler_rest)$' --no-tests=error --output-on-failure
ctest --preset full-locked -R '^(p11_control_model_rest|p12_audit_logs_rest)$' --no-tests=error --output-on-failure
# 任务 3：Bridge 隔离工作树；后续 3 项及最后 Model 2 项复跑见报告
ctest --preset full-locked -R '^(p06_runtime_event_delivery|p07_action_rules|p09a_runtime_storage_application|p06_runtime_plugin_capability|p11_model_bridge|p06_postgres_runtime_events|p06_bridge_acceptance|p06_client_e2e|p05_runtime_resource_application)$' --no-tests=error --output-on-failure
# 主工作区专项首轮（7/10，首轮结果保留）
ctest --preset full-locked -R '^(p05_runtime_rules|p05_runtime_resource_application|p05_runtime_identity_e2e|p05_runtime_catalog_e2e|p13_runtime_lifecycle|p13_scheduler_lifecycle|p08_host_bridge_e2e|p13_joint_delete_e2e|p13h3_joint_delete_production_e2e|p10_timer_e2e)$' --no-tests=error --output-on-failure
# 最终主工作区：最后增量重新 configure/build 后，167/167、无跳过
ctest --preset full-locked --no-tests=error --output-on-failure
```

### 已知范围与环境限制

本节沿用下方后续环境矩阵中的最近证据：单机六服务 Compose 的本地门禁、QQ 登录与只读鉴权
已有记录；QQ 的目标 Runtime 注册、真实会话收发及断连恢复仍未验收。真实付费 Provider、
生产 CA/密钥托管和离站备份、远端 CI/release 仍有缺口。两主机/断网演练为用户确认可选延期，
不作为本轮新增强制门槛。历史原生叶证书负例通过不等于 Compose 挂载复演通过。
host operation/bootstrap_credential、目标 Control WS 与模型 streaming 当前仍未完成对应新增能力，
现已按 [五项扩展计划](29-approved-capability-expansion-plan.md) 纳入实施；旧“不扩契约/延期/不做”
范围限制不再适用于新计划，但在真实实现与验收前不得翻转 `implemented`。
ACK/重新注册没有专门的并发屏障测试；当前证据是持久完整 key/fence 条件、锁顺序源码核对、
真实 PG 与 TLS 的重复 ACK/旧代次/重连负例，不推断生产并发压力已验收。
PluginBridge 刷新失败帧的 INTERNAL 与终止状态偏差仍待专项；格式与 tidy 基线限制见本节开头。

## 历史：P14t 门禁与 P04–P14 证据（2026-11-03）

以下各阶段矩阵和“最终门禁”属于原提交/当时环境，保留历史依据；与后附环境进度不同的旧结论
只用于追溯，当前工作区状态读第 0 节，不从某次孤立复跑推断历史首轮或今天全量通过。

2026-11-03 本轮工作树（基于 `254f017`）full-locked 构建通过。全量 CTest 首轮 160/162（1040.38 秒），`p10_timer_e2e` 和 `p02_core_process` 失败；仅重跑这两项，2/2 通过（81.06 秒）。下列带提交号的门禁保留各自历史结果。

- 版本：v1.3（2026-11-03，P14t 最终刷新）
- 基线提交：`ce7993a`（main；P14t/P05b-13 最终文档刷新；final gate 见下）
- 用途：发布前统一回答“哪些已经实现、哪些已实测、哪些仍待验收/缺口/用户确认”，**不替代**各阶段原始报告与测试记录，也不把提交/文档存在当作阶段完成。
- 相关文档：[发布清单](28-p14d-release-checklist.md)、[实施计划](23-implementation-plan.md)、[运维 runbook](08-operations.md)、[CI/CD](12-ci-cd.md)、[契约状态](01-contracts.md)、[最终交付状态](../reports/modules/architecture-delivery-status.md)。
- 最终门禁：previous checkpoint @7b8fb0f：**161/161 CTest passed @7b8fb0f**（`build/logs/recheck-ctest-155131.log`，1051.70s，2026-11-03 15:51:31→16:09:06；本轮一次全绿、无失败；此前 `1213790` gate 148/148（687.02s）的 `p02_core_process`/`p08_managed_host_e2e` 负载时序 flake 未复现）。P00n SQLite 写锁竞态已修（final gate sqlite 0.27s）；正式发布仍须在 release SHA 全量重跑并归档。
- 最终门禁：final HEAD @ce7993a（docs-only 之上最终文档）：**161/162 CTest passed**（`build/logs/recheck-ctest-164849.log`，2026-11-03 17:09:27→17:26:37）；唯一失败 `control_http_websocket_acceptance` 71.48s（WebSocket handshake 超时，套件负载下），孤立复跑 **Passed 53.86s**，判定负载时序 flake。
- 最终门禁：exact-HEAD gate @04f7044：**161/162 CTest passed，1025.87s**（`build/logs/recheck-ctest-162320.log`，2026-11-03 16:23:19→16:40:30）；唯一失败 `p08_managed_host_e2e` 35.77s（psql SSL negotiation invalid response，套件负载下时序问题）；孤立复跑 **2/2 Passed 58.64s/56.72s**（Lead 57.39s），判定负载时序 flake，不是断言/实现回归。此前 161/161@7b8fb0f/1051.70s。
- 原生单机矩阵（P14m 后）：**success 9 + stable-ok 1 + known-gap 0**；`p14_native_single_host`/`p12j_panel_live` 在真实五进程 + PG16.4 + mTLS 栈上通过。

## 1. 状态定义

| 状态 | 含义 |
|---|---|
| 退出通过/范围完成 | 阶段验收项已有真实依赖或明确 loopback 证据，证据测试与报告可复现；残余环境/后续项已列明且不阻塞本阶段目标 |
| 已实现 | 指定提交或明确工作区差异中有实现；须另列是否合入及实测范围，未配置依赖时不假成功 |
| 已实测 | 在真实依赖（PG16.4/SQLite/TLS/进程等）或明确 loopback 假服务上跑过，有命令/测试名/日期/HEAD |
| 待验收 | 实现存在，但验收环境/链路（Docker/跨主机/远端/真实平台/Provider）未满足 |
| 已知缺口 | 明确未实现或仅部分实现，发布说明必须保留 |
| 待用户决策 | 需要用户/产品拍板的语义、策略、范围或环境项 |

`已实现` 不代表 `已实测`；`已实测（本机）` 不代表单主机 Compose、跨主机、远端 CI 或正式发布已验证。

## 2. P04–P14 矩阵

| 阶段 | 状态 | 关键已实测证据（命令/测试/HEAD） | 待验收/已知缺口 | 待用户决策/环境 |
|---|---|---|---|---|
| P04 Identity/Control 认证 | 退出通过（本机） | `ctest -R 'p04_'`；`p04_identity_application`、`p04_identity_rpc`、`p04_control_auth_https`、`p04_identity_daemon_tls_expiry`；P04f/P05f 历史证据后保持绿；final gate 161/161@7b8fb0f | 生产 CA/密钥托管、真实 QQ 登录态复验属环境项 | CA/secret manager、生产证书轮换 |
| P05 Runtime 资源/授权链 | **B1 链完成**（P05b-2/5/6/7/8/9/4/12）；安全评审 0 Critical/High，Medium 全部处置：F-1/F-2 已修、F-3/F-4/F-5/F-6 已加固（P05b-13；F-5 host/core 边界文档化） | `p05b_task_chain_e2e`、`p05b_model_chain_e2e`、`p05b_model_chain_fail_closed_e2e`、`p05b_provider_fixture`、`p05_runtime_identity_e2e`；`reports/modules/architecture-p05b*` | 真实多实例/多安装规模压测；F-5 残余 host/core dump/ptrace 边界（已文档化，主机责任）；生产部署环境 | **P05b B1 已批准并完成本机链**；P11 真实 Provider 验收范围已批；跨主机/生产 CA/密钥 |
| P06 事件/投递/SDK/Adapter | 退出通过（本机/真实进程） | `p06_bridge_acceptance`、`p06_client_e2e`、`p06_postgres_runtime_events`、`p07_adapter_e2e` P06g 全总线（真实 adapter+Runtime+受管 SDK plugin） | 真实 QQ/NapCat 长期稳定性/网络抖动；真实平台链路人工验收 | QQ/NapCat 环境；P05b 相关下游委托 |
| P07 Action/配额/执行 | **退出通过（P07f-2）** | `p07_action_rules`、`p07_postgres_action_records`、`p07_adapter_e2e`、`p06_bridge_acceptance`、`p12_control_owner_gateway`；SDK action pytest | 真实平台 reaction API、Control `listActionMetadata` REST 未接线（环境/后续能力，不阻塞 target 契约退出） | 真实平台动作（Mute/Kick/撤回/审批）环境项 |
| P08 Host/期望状态/配置 | **退出通过（P08f）** | `p08_host_*`、`p08_postgres_host_registration`、`p08_host_bridge_e2e`、`p13_host_sdk_drain`；真实 TLS HostBridge | 真实受管进程/Docker Compose 生命周期；Host/插件 ACK 与 Runtime/Control 三类事实的 Compose 端到端 | Docker Compose 环境；跨主机 |
| P09 Namespace/Storage | 范围完成（本机），部分待验收 | `p09_postgres_storage_records`、`p09a_runtime_storage_application`、`p06_bridge_acceptance` 共享成员/namespace 删除隔离 | Control/Panel 共享 namespace 管理入口 live；P09b/C 分批范围 | 真实 Control/Panel live、生产规模 |
| P10 Scheduler/定时交接 | **退出通过（P10m）；P10k 真实 Scheduler 任务链通过** | `p10_postgres_scheduler`、`p10_scheduler_rpc`、`p10_scheduler_rest`、`p10_sdk_tasks_e2e`、`p10_timer_e2e`、**`p05b_task_chain_e2e`**（真实 managed SDK plugin Bridge task_create/get/cancel→Runtime→真 Scheduler；owner=instance/installation、幂等/取消/COMPLETED、跨实例 NOT_FOUND、离线 fail-closed）；P10m 修复 | 真实 7 天 EXPIRED/多 installation 容量；真实 QQ/平台触发；生产规模 | 真实时钟/时区/平台、跨主机；真实 QQ 环境 |
| P11 Model 网关 | **独立验收通过（本地真实依赖）** | **P11h**：`p11h_managed_model_e2e`（真实 managed Host+SDK plugin→PluginBridge→Runtime→Model→真实 HTTP Provider 正/负矩阵）、`p11h_model_chain_fail_closed`、`p11h_provider_fixture`；相关回归 **37/37 passed**（p11|p05|p08|p06|p03_postgres）；p11 9/9、p03 真 PG 2/2、contract gate PASS | 真实付费外部 Provider（T07b，计费/配额/外部网络）；目标部署环境；streaming 明确不做 | **P11 独立验收通过**；真实付费 Provider/目标部署环境待环境；两条 Model 管理 RPC 的精确授权见下表 |
| P12 Control/Panel | **范围完成**（P12l/P12j/P14g-i）；host operation/bootstrap_credential 保持 not_implemented | `p12_control_owner_gateway`、`p04_control_auth_https`、`p12l` principals、`p12j_panel_live` 严格模式 PASS；原生 Control `/api/v1/health` 200 HEALTHY；P14g occurrence/readiness、P14l/P14m audit/log 修复 | 真实 Panel 浏览器 live 全批准入口；真实四依赖健康环境；host operation/bootstrap_credential 字段级证据待决策 | **P12 host operation / bootstrap_credential 保持 not_implemented（用户确认，不扩契约）**；Docker/跨主机 |
| P13 生命周期/可观测 | **生产退出通过（本验收范围）** | **`p13h3_joint_delete_production_e2e`**：真 PG16.4 四库+真 Identity bootstrap+真 mTLS Runtime/Scheduler+真 plugin-host+受管 SDK 插件；BeginDelete→真实 Host receipt+Scheduler ListTasks/CancelTask→全 Done→DELETED、Host UNKNOWN/离线 BLOCKED、Scheduler denied BLOCKED、Restore 不重建 task/凭据/租约、再次 BeginDelete 重新调度、跨实例任务不受影响；P13h-1 `p10_timer_e2e` platform-peer 矩阵、P13h-2 `p13_scheduler_lifecycle`/`p10_scheduler_task_client` | 真实 Host UNKNOWN 注入（当前 HostBridge 测试客户端）、Scheduler 完全离线主链引用既有生产证据、P14 多主机/真实满盘/生产规模 | **P13 生产退出通过（本验收范围）**；B 未批准；多主机/满盘/生产规模待 P14 环境 |
| P14 部署/故障/交付 | 交付材料/原生矩阵完成；**final exact-HEAD 门禁已回填** | exact-HEAD gate @04f7044：**161/162 CTest passed，1025.87s**（`build/logs/recheck-ctest-162320.log`，2026-11-03 16:23:19→16:40:30）；唯一失败 `p08_managed_host_e2e` 35.77s（psql SSL negotiation invalid response，套件负载下时序问题）；孤立复跑 **2/2 Passed 58.64s/56.72s**（Lead 57.39s），判定负载时序 flake，不是断言/实现回归；P14f/i/j/k/l/m 原生栈矩阵 9 success+1 stable-ok+0 known-gap；P13/P11/P10k/P05b 通过证据见各行 | 两主机/断网、远端 CI/release；真实 QQ/NapCat、真实付费 Provider、生产 CA/密钥/备份离站；部署级错误 Runtime 叶证书 | Docker/两主机/远端 CI/Release 环境（本机 Docker 已就绪，gh 登录暂缓）；QQ/Provider/CA/密钥/备份离站 |

> P00–P03（工具链、契约、PG/UoW/初始化）作为前置基线已由 `p03_postgres_database`、`p02_core_process`、`p01_*`、contract 门禁覆盖，证据见 [architecture-p02-acceptance.md](../reports/modules/architecture-p02-acceptance.md)、[architecture-p03-postgres.md](../reports/modules/architecture-p03-postgres.md)、`doc/11`。

## 3. P10m 修复前后对比（final gate 的前置）

- **修复前**：`p10_timer_e2e` 在 8192 tasks/expiry churn 后 SIGTERM Runtime 偶发失败约 1/3（17–18s；受控诊断 4 次循环中 1 次），退出码 -11，backtrace 为 OpenSSL `EVP_RAND_CTX_free`/`init_thread_stop` atexit 竞争。
- **根因**：OpenSSL 3 `OPENSSL_cleanup` 在 atexit 释放 RAND 状态，gRPC/service worker 线程在进程 teardown 后段退出，TLS 析构访问已释放状态。修复：共用 `run_service` 早期 `OPENSSL_init_crypto(OPENSSL_INIT_NO_ATEXIT)`，不注册退出期全局清理；停机状态机/信号/RPC/DB 语义不变。
- **修复后**：`p10_timer_e2e` 新增 10 次“8192 filler 插入/删除→SIGTERM→Runtime 重启”循环；连续 5 次完整 e2e（50 次停机）全 exit 0，另 12 次完整 e2e 无崩溃；final gate **161/161@7b8fb0f** 包含 `p10_timer_e2e`/p10 抽验通过。提交：`6bff2c9` fix、`532b1d3` test、`e83a988` docs。
- **P00n SQLite 竞态修复**：首跑 `sqlite_runtime_store` 偶发双 Conflict（两个独立 SQLite 连接读同一 revision 后竞争 UPDATE，无 busy timeout 导致双双 `SQLITE_BUSY`）；修复为对 peer writer 最多等待 50ms + 显式 ready latch，WHERE revision 仍决定唯一 winner。final gate `sqlite_runtime_store` Passed 0.27s；正式 release SHA 仍需重跑全量并归档。
- **P06h 真实链补齐**：`p07_adapter_e2e` 扩展 fake OneBot 平台延迟、PG deadline 过期→EXPIRED、迟到 ActionResult 仅写 evidence、SIGKILL adapter 后 recover 生成 UNKNOWN evidence 补报；重复平台帧不新增事件、乱序事件不重复 target。真实 QQ/NapCat 仍需环境。

## 3a. P06h 真实链补齐（2026-11-03）

`tests/architecture/p07_adapter_e2e.py`（真实 adapter/fake OneBot/Runtime/真 PG）扩展：fake OneBot `delay_seconds` 制造平台延迟，Action 派发后把 PG deadline 推进到过去→Runtime maintenance 转 EXPIRED；平台响应迟到时只写 `adapter_result_evidence`，动作终态保持 EXPIRED、plugin 查询不出现 known-success、平台副作用仅一次。SIGKILL adapter during platform call 后重启同一 OUTBOX_PATH，adapter `recover()` 生成 UNKNOWN evidence report 并重连补报；Runtime Action 保持 ACCEPTED/非 known-success、evidence 行存在、平台调用不重复。重复平台 frame 不新增 event；乱序两事件各投递一次、无重复 target。final gate `p07_adapter_e2e` Passed 29.19s。真实 QQ/NapCat 与生产 adapter 重启窗口仍需环境。

## 4. 原生单机目标栈矩阵（P14m 后，final）

`p14_native_single_host`（真实 Identity/Runtime/Scheduler/Model/Control 五进程 + owned PG16.4 + 临时 PKI + Control HTTPS）最终 strict 矩阵：

- **success 9**：health（200 HEALTHY 四真实组件）、principals（P12l 真实页）、logs（200 + identity 日志行）、audit（200 + identity 审计行/分页）、tasks_list（200 空页）、models（200 空页）、action_query（404 NOT_FOUND owner 语义）、delete（404 NOT_FOUND）、restore（409 CONFLICT）。
- **stable-ok 1**：action_policy GET（404 NOT_FOUND，fixture 未播种 account policy，稳定 owner 业务语义）。
- **known-gap 0**。
- 故障/重启/fail-closed（bad CA/unauthenticated DB）矩阵保持通过；`p12j_panel_live` 严格模式在同一栈上通过（健康 200、principals capability/known-gap 口径已随后收口）。

## 5. 后续验收项及进度

下表逐项列出 P14t 基线遗留项的最新验收进度。

| 类别 | 状态 | 说明/退出条件 |
|---|---|---|
| 单机 Docker Compose 六服务栈 | **六服务本地门禁通过** | 官方 PG16.4 上四个 init 容器成功，Identity 引导成功。六个业务容器和 Panel 同时 healthy，Panel 经 TLS 访问 Control 的 readiness 返回 HEALTHY，Control HTTPS 成功登记禁用的 DeepSeek 配置。 |
| 两主机/断网演练 | **延期（用户确认可选）** | 两主机拓扑、网络分区、TLS/撤销/重连、协调恢复未验证 |
| 远端 CI / release | **未运行/未就绪** | `gh` 未认证、GitHub Actions 未触发；正式发布无 release SHA 全量归档、无推送/签名镜像、无 SBOM/cosign/release-gated Environment |
| 真实 QQ/NapCat | **登录与只读鉴权通过** | 从目标 Adapter 镜像经 Compose 网络调用真实 OneBot 的 get_status 和 get_login_info 成功；目标 Runtime 注册、会话收发及断连恢复仍待验证。 |
| 真实付费外部 Model Provider | 待 API Key 和费用上限 | Control HTTPS 已将 DeepSeek 配置登记为禁用。匿名 GET /models 返回 401；付费推理与计费核对仍待验证。 |
| 生产 CA/密钥托管、备份离站 | 待用户环境/决策 | CA/root key 托管、轮换/吊销、KMS/离线/分片；备份保留周期、离站位置/频率、生产数据量恢复演练未定 |
| 部署级错误 Runtime 叶证书 | **原生目标进程通过；Compose 挂载复演待验证** | `p05b_model_chain_e2e` 使用两张同 CA、同 Runtime URI SAN 的叶证书。已登记叶换取子委托并成功调用 Model.Chat，HTTP Provider 新增一次请求；未登记叶建立 mTLS 通道后，ExchangeDelegation 和携同一子委托的 Model.Chat 均返回 UNAUTHENTICATED，Provider 计数不变。 |
| Model Update/RotateProviderSecret `scope_kinds` | **已验收** | `p14_native_single_host` 从 Control HTTPS 发起，两条 RPC 均经真实 Identity 授权。目标模型 HTTP 200，跨模型 HTTP 403 `PERMISSION_DENIED`。 |

## 6. 用户决策与实施状态（2026-11-03，P14t）

用户决策登记于 `reports/analysis/user-decisions-2026-11-03.md`；**已批准不等于全环境完成**。

| 项 | 决策 | 实施/验收状态 | 说明 |
|---|---|---|---|
| P13 | 限定批准 A：Scheduler mTLS 平台身份+白名单+显式授权，仅 ListTasks/CancelTask；ListTasks 限目标实例、CancelTask 核对归属、跨实例/跨安装拒绝；生产验收等真实 Host 回执+Scheduler cleanup→DELETED，UNKNOWN/离线 BLOCKED，Restore 不重建任务/租约 | **生产退出 PASS（本验收范围）**：`p13h3_joint_delete_production_e2e`；P13h-1/2 证据已合入 | 残余：真实 Host UNKNOWN 注入、Scheduler 完全离线主链引用既有证据、多主机/满盘/生产规模属 P14 环境 |
| P05b / P11 / P10k | 批准 B1（Control 管理员触发实例主体父委托）；P11 验收真实 managed plugin+真实 HTTP Provider fixture（非 fake port）+撤销/过期/跨实例负例；P10k 同批解锁 | **B1 链完成；P11 独立验收 PASS（37/37 相关回归）；P10k 真实 Scheduler 任务链 PASS**；安全评审 0 Critical/High，Medium 全部处置：F-1/F-2 已修、F-3/F-4/F-5/F-6 已加固（P05b-13；F-5 host/core 边界文档化） | 真实付费外部 Provider、目标部署环境、部署级错误 Runtime 叶证书仍待验收 |
| P12 | host operation/bootstrap_credential 字段级缺口保持 `not_implemented`（用户确认），只文档化、不扩契约 | 无新增实现任务；契约/文档保持 not_implemented | 不是完成；后续需用户另批契约 |
| P14 | 本机 Docker 已就绪，GitHub 登录暂缓 | Compose 数据层已实测，六服务容器验收进行中 | 远端 CI/release、生产 CA/密钥托管与离站备份待验收 |
| 两主机/跨主机 | 用户确认可选、延期 | 未做 | 延期 |

## 7. 维护规则

- 每次阶段完成或发布准备时更新本矩阵；只把有命令/测试记录/报告支撑的项标为“已实测”。
- 用户确认项变化时同步 `doc/08`、`doc/12`、`doc/23` 与发布清单；`doc/18` 为冻结基线，只读不改。
- 证据引用优先指向 `reports/modules/` 与 `ctest` 名称；测试改名/删除时必须同一变更更新本文件与发布清单。
