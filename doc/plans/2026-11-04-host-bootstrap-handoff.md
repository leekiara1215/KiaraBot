# Host / Bootstrap / Identity v1.1 交接提示词

日期：2026-11-04。用户要求停止当前扩展实现、将后续任务交其他agent。本文写入时所有产品源码冻结，未回滚任何编辑；后续agent须先阅读本文件再接手。本文件记录实际状态，不将源码存在或局部替身等同完成验收。

## 所有接手agent共用约束

你在`/Users/betta/Workspace/KiaraBot`工作，不是唯一工作者。保留大量既有未提交重构及其他agent编辑，不reset/revert/stash，不提交别人的差异，不手改生成文件。先读AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md和当前模块文档。`doc/18-architecture-baseline.md`及保护清单只读，reference blob/保护manifest不可改。任务不涉及游戏存档，不调用GM服务。

结构探索遵循codebase-memory skill：图generation最后通知为2026-11-04T08:26:54Z；target application/Control代码excluded，runtime_rpc/service_config partial，须做coverage并回读源码，不能以图缺失证明实现不存在。

共享full-locked configure/build/PG/HTTP/重型tidy由主代理串行调度，不擅自并发。锁定format工具为`build/format-venv/bin/clang-format`16.0.6，tidy同目录22.1.8。Node使用`build/toolchain/node-v22.14.0/bin`。不降低已有gate、不删原断言、不虚构implemented。

**契约锁已整体转给WS agent**：identity.proto、runtime.proto、observations.proto、scheduler.proto、共享registry/target_spec/goldens/直接contract tests及doc01通用新增内容。WS正推进v1.2；本agent不可再并发编辑。HERR03既有登记保留；后续状态翻转先联系WS/主代理。Model持model/plugins/model_stream及RuntimeIdentity decoder相关协作锁；不要并发改Bridge或decoder。cmake/control-auth.cmake已释放给WS，仅追加WSS fragment；BOOT已有目标须保留。

已读总体子计划：`doc/plans/2026-11-04-host-bootstrap-plan.md`。主计划/决策台账由主代理维护。

## 提示词A：先解决HOST-02真实终止竞态，再验收纯应用/SQLite

你的首要任务是完成已有HOST-02，不另开新功能。模块为Host纯维护应用、Host私有SQLite及受控POSIX执行；尚未接HostManagement RPC、Control路由或真实RuntimeAdmissionGate backend。授权/Gate在专项测试中是明确局部替身，不能作为整个HOST退出证据。

允许接手文件：

- `core/include/kiarabot/core/{domain,ports,application}/target/host_maintenance.hpp`
- `core/src/application/target/host_maintenance.cpp`
- `infrastructure/include/kiarabot/infrastructure/host/host_maintenance_sqlite.hpp`
- `infrastructure/src/host_maintenance_sqlite.cpp`
- `infrastructure/tests/host_maintenance_test.cpp`
- `cmake/host-maintenance.cmake`，core/CMakeLists.txt仅必要include整合（先联系主代理）。
- 已批准直接受影响的`core/include/kiarabot/core/application/target/host_service.hpp`、`core/src/application/target/host_service.cpp`、`core/tests/platform/host_service_test.cpp`、`infrastructure/src/host_journal.cpp`。
- 本次已额外批准`core/src/runtime/platform/host_process.cpp`终止竞态最小修复；原保护marker/进程组/白名单不得放松。
- 文档影响限doc02/doc08、`doc/modules/host-maintenance-storage.md`及模块交付报告，doc05/PG文件仍须协调owner。

现有应用：Accepted先可取消；Begin先持久LocalClosed，再RuntimeGate确认，冻结最多256执行集合，每次最多停一个并持久known事实；Resume先确认无活动/未知执行，再RuntimeOpen、本地Open；Cancel仅Accepted。SQLite同事务保存幂等身份、state/op/audit及复用WS OperationObservationPort的commit序变化，不造第二cursor。scope单自身RESOURCE_HOST、持久epoch/head/low-water、600秒/65536条/64MiB。HostService逐stop失败已传播，HostJournal保留Exited+UNKNOWN。无PG/schema改动。

两项微修已完成：SQLite读取拒绝admission非0/1；真实fixture异常路径有OwnedProcessCleanup RAII。新增Failure/Dependencies显式初始化已修，主代理有限重编无新增warning。

### 当前真实失败与临时诊断（必须先处理）

**竞态尚未修复。`host_process.cpp`仍保留临时signal诊断和iostream include，交付前必须去掉。** TERM/KILL实际返回码/errno已分别保存。KILL错误分支临时调用`alive(pid,marker)`打印，`alive`内部会reap，不能直接把这段诊断搬成正式恢复判断。

实际链：TERM成功后grace=10ms到期，随后group SIGKILL返回EPERM；精确诊断为：

```text
owned fixture signal diagnostic stage=KILL errno=1 same_execution_alive=0
```

外层probe：group signal0 errno0、PGID==child_pid true、alive_before true，terminate约12–14ms后Failure(UpstreamUnavailable)。HostJournal最终`state=4`（ManagedExecutionState::Unknown，**不是Exited3**）、`outcome=3`（Unknown）。维护进度故障trigger又将blocked写回滚，故新断言先失败：`confirmed stop survives failed maintenance progress write in journal`。原`known journal evidence recovers operation after progress rollback`断言保留，不能删除或把Unknown当KnownStopped。

固定fixture已加主进程ready文件握手（owned temp、child main O_EXCL/NOFOLLOW0600创建，parent有界等待并核对原marker/自身PGID）；不是用任意sleep掩盖失败。`ObservedSupervisor`仅局部转发真实POSIX实现，仍有enum/errno/耗时诊断；成功收口后整理为必要安全诊断或移除，不遗留调试噪音。

独立对照：同一MachO child和`/bin/sleep`分别用Python空env启动，在新session及同session/process_group=0两种情况下，same UID、PGID==child_pid均true，group TERM errno0、确认退出。所以不能宣称环境限制self-MachO。原`p08_host_process`持续通过。

主代理要求的正式修复方向：TERM发出后KILL失败时，只在能确认**原owned child真实waitpid退出事实**时保守返回非forced终止成功；不能仅凭proc_pidinfo失败或alive=false推断成功。仍活着、身份不符、无法确认的情况继续Failure/Unknown。注意临时诊断已reap；正式实现需设计正确证据收集顺序/所有权，不依赖已丢失waitpid事实。不得降低进程组或marker保护、不得把EPERM成功化。补合法短grace **0/1/10ms**及“signal failure且仍live不吞拒绝”回归，不只把fixture grace调大。

已跑证据/失败日志均在`build/logs/capability-expansion/`：

- 首轮HostService 0.50s与HostJournal 0.50s通过；maintenance失败。`ctest-host02-first.log`。
- `ctest-host02-recovery-diagnostic.log`、`ctest-host02-enum-runtime-decoder.log`、`ctest-host02-child-readiness.log`、`ctest-host02-signal-probe.log`。
- 最后精确branch：`ctest-host02-signal-branch.log`，HostProcess 1.19s通过、maintenance约0.64s失败。
- 主代理已编过HOST全部新target，补warnings后有限build exit0；尚无HOST02完整通过声明。

下一构建/验收由主代理运行：

```bash
cmake --build --preset full-locked --target kiarabot_host_maintenance_test kiarabot_host_process_test kiarabot_host_service_test host_journal_test
ctest --preset full-locked -R '^p08_host_(maintenance|process|service|journal)$' --no-tests=error --output-on-failure
```

maintenance executable实际在`build/full-locked/core/kiarabot_host_maintenance_test`。不要再次误用不存在的root路径或target `host_process_test`。完成后锁定format/tidy、边界/inner gate，并更新模块报告；真实RuntimeGate授权接线另派HOST-03/04。

## 提示词B：完成Identity v1.1生产窄检查、Host登记与配置

依赖：v1.1契约已经主代理验收冻结，当前生产源码是**未构建的半成品**，不能宣称ready。你接手后先审阅差异、补测试，再请主代理给编译窗口。

已冻结协议：Identity.CheckHostGateAuthorization只接受current registered Runtime mTLS调用者+自身Principal显式identity.authorization.check；固定用途Runtime.SetHostAdmissionGate，request仅context/单RESOURCE_HOST/实际TLS叶SHA256（32字节），不接受客户端自报principal/动态方法/permission。按指纹读取current Host credential/principal及**显式存储RESOURCE_HOST(host_id)绑定**，核定精确host.execute。响应仅decision和Host credential/principal版本，非可复用ticket。Runtime以后必须同时核对actualTLS、原HostBridge allowlist/fence、intent/version。HostId与PrincipalId独立。

原CheckAuthorization的非空delegation proof语义不可削弱；HostManagement的人类请求仍用该协议，不持久Control bearer；Scheduler受限A仍只有原两方法。Identity无新增DDL/UoW/repo；复用既有Principal.binding与Credential.binding，仅Host登记绑定Host，其余平台仍自身Principal。

已编辑但**未编译/未跑真实应用测试**的文件与状态：

- `core/include/kiarabot/core/application/target/identity_authorization_application.hpp`：新增typed request/reply与纯virtual check_host_gate_authorization。
- `core/src/application/target/identity_authorization_application.cpp`：实际窄检查已写，Runtime caller、当前自检查cap、Host指纹/current head/有效注册/Host绑定/显式精确grant；双只读UoW水位与trusted clock再读，无crypto伪proof。原check_authorization逻辑未改。待审计/编译。
- `core/tests/architecture/identity_authorization_application_test.cpp`：新增15分支（allow/deny、错scope/叶、撤销/disabled、非Runtime、缺caller cap、错binding、超时/依赖/重读水位/错kind/不可信时钟）；未执行，部分新单行if尚需按C++标准补大括号后format。
- `core/src/runtime/platform/identity_rpc.cpp`：CheckHostGateAuthorization handler已写，严格32bytes及Host kind，调用真实应用、校验输出、映射decision/版本；未编译/未补真实TLS RPC负例。
- `core/include/kiarabot/core/application/target/identity_admin_application.hpp`：PlatformPeerEnrollment末尾新增optional Host资源。
- `core/src/application/target/identity_admin_application.cpp`：允许Host enrollment、HostId必填且批内不重复、Host Principal/credential绑定Host、read_peer仅Host例外核对该绑定；其他平台原规则保留。未编译，尚需Host正/负登记测试；原旧Host拒绝测试只变service但证书仍Runtime/缺host，仍应拒绝，需补新准确说明。
- `core/app/identity_bootstrap_main.cpp`：parser允许service=host且exact JSON额外必填host_id，其他service拒绝该额外key；未编译/未跑CLI验收。
- `core/include/kiarabot/core/runtime/platform/service_config.hpp`、`core/src/runtime/platform/service_config.cpp`：Host允许identity_endpoint；同时按WS协作已追加ControlApiMaterial.websocket_origins（默认空）/websocket_allow_missing_origin（默认false），严格bare HTTPS origins validator与JSON parse，≤32去重、IPv6 bracket、端口1..65535、拒路径/query/userinfo/wildcard/非法字符。**未编译/未补service_config_test.py，WS运行配置说明尚未补doc08**。这两文件锁需与WS协调后移交；不要重复追加字段。

Host composition、Identity client/在线管理认证、Runtime真正gate授权检查backend均**尚未接**；不是以上函数存在就生产成功。RuntimeIdentity decoder由WS单editor，若需Host audience发精确需求，不并发改。根/平台CMake片段由各owner协调。

v1.1已跑契约证据：identity.proto冻结时SHA256 `4aa85a1897eeaf5365863635c70638c0d3e4c75cd1c97bff28a5ac848c9a651b`（WS后续v1.2追加后应不同，不强行还原）。独立生成/descriptor对上一freeze兼容exit0，fullcontracts含legacy=1 exit0；主代理重链正式roundtrip **119RPC/378messages/27goldens/26stream+Hostgate oracle** exit0，`identity-v11-roundtrip.log`。独立Python目录`build/logs/capability-expansion/identity-v11-python`、descriptor `identity-v11-descriptor.pb`、`identity-v11-contracts.log/result.json`。新RPC/fields/semantic保持not_implemented，待生产实测再由契约owner登记。

后续精确有限目标/CTest：

```bash
cmake --build --preset full-locked --target kiarabot_identity_authorization_application_test kiarabot_identity_admin_application_test kiarabot_identity_rpc_test kiarabot_service_config_driver kiarabot-identity-bootstrap
ctest --preset full-locked -R '^(p04_identity_authorization_application|p04_identity_admin_application|p04_identity_rpc|p02_service_config)$' --no-tests=error --output-on-failure
```

先实际确认daemon target名，再安排真实Identity/PG/mTLS链与CLI enrollment测试；不擅自启动真实用户库。新增正例必须HostId不同于PrincipalId，错误叶/旧证书/撤销/跨Host/缺grant/错caller/依赖故障均fail-closed，查询无秘密。无普通REST新增面，API schema不用仅为内部RPC改动。

## 提示词C：继续BOOT-02/04/05闭环（先端口/持久版本，再接线）

BOOT-03模块已有纯SecretDeliveryApplication/Port与固定recipient原子0600文件sink：Control文件位于`control/api/include/kiarabot/control/{application/target/secret_delivery.hpp,target/secret_file_delivery.hpp}`及对应src，测试`control/api/tests/secret_delivery_test.cpp`。根片段`cmake/control-auth.cmake`中的3个BOOT目标已存在，WS正在仅追加自己的include。

原真实p12_secret_delivery 1/1、1.00s、零新C++warning。后续tidy发现dup/fdopendir真实错误，已用FD RAII及成功转移DIR所有权修复，并加仅fork子进程EMFILE限额负例；5个HERR/BOOT改动TU最终tidy全0（`herr-boot-tidy-final-results.json`、`boot03-tidy-final-{3,4}.log`）。统筹已对最终sink及新增EMFILE负例有限重建并实跑，`p12_secret_delivery` 1/1通过0.85s（总0.86s）；日志`build-secret-delivery-fd-fixed.log`、`ctest-secret-delivery-fd-fixed.log`。这只是文件模块证据，不能扩大为Runtime签发/消费整链通过。

该sink已有每peer flock、intent先行、版本CAS、fsync/rename/目录fsync、无秘密receipt/query、四边界fork崩溃、ACK丢失/查询、迟到旧worker、symlink/hardlink/owner/mode/路径边界；Runtime签发/撤销/真实version/report/reissue及自动交付接线仍未做。无新secret DB或自研加密协议。

按既有子计划下发：

- **BOOT-02**：Runtime typed签发/版本/无秘密交付intent与operation、report/query/reissue、旧凭据失效，Runtime只存摘要和真实credential_version。现有bind/create未带bootstrap_recipient_id路径完全保留；仅非空显式opt-in进入自动交付。涉及Runtime资源common/account_adapter/instances、runtime_rpc/composition、必要PG存储及schema。PGschema/UoW/common_work/factories正有WS/Model工作，先向主代理申请串行精确窗口，不能并发改。所有operation变化复用既有journal，不手动重复append。
- **BOOT-04**：把已批准固定recipient/文件路径端口与真实Control→受控部署helper/secret store接通，managed插件与externally_managed路径分别处理；不混同Host mTLS、QQ密码。普通REST持续删除bootstrap秘密字段，失败显式重新发放，不能从SHA恢复明文。Control owner_gateway accounts/instances当前脱敏保护必须保留。
- **BOOT-05**：在私有测试PG/Host SQLite、真实SDK或受控adapter fixture跑完整签发→交付→注册→查询/重发/恢复；覆盖响应丢失、Control/Runtime重启、旧worker CAS、撤销、错recipient/资源、删除/取消迟到等。交付成功、插件注册、进程运行、配置应用是不同事实，不能相互替代。实测后再登记字段级target状态，保留整族未完成项目。

BOOT最后模块回归：

```bash
cmake --build --preset full-locked --target kiarabot_secret_delivery_test
ctest --preset full-locked -R '^p12_secret_delivery$' --no-tests=error --output-on-failure
```

目前PG源schema为I15/R19/S11/M10；doc08已同步当前源并保留历史记录。BOOT若新增DDL由主代理统一bump/manifest/真实describe，不能操作现有运行库或改旧历史记录。公共wire变更先联系当前WS契约owner，不能直接重新拿runtime.proto锁。

## 已完成的HERR与交付证据边界

HERR-02真实TLS raw-wire测试已通过p11_model_bridge 1/1、3.94s：空operation INVALID_ARGUMENT；单future length-delimited候选CAPABILITY_NOT_SUPPORTED；known+unknown/多unknown/wrong-wire拒绝，下游Model/Task/Storage零调用；metadata128兼容；标准已知oneof last-wins。typed解析后不承诺识别已丢失的重复已知oneof历史。Bridge已释放给Model，后续agent不可并发编辑。

HERR-03仅target `semantic.host.request_operation_classification`与`semantic.host.unknown_operation_error`登记implemented，evidence含helper/Bridge/TLS测试/模块报告，status gate0、evidence9/9；旧reference仍not_implemented且blob/manifest未改，旧证据路径已纠正到core/reference。未翻streaming或Host管理族。主代理最终集成将重跑。

交付汇总在`reports/modules/architecture-host-bootstrap-slices.md`，主代理负责doc27/台账。后续必须区分已跑/未跑/失败、文档影响与无影响理由；完成相应format、改动TU tidy、boundaries/inner gate，任何未执行要说明具体原因。当前自有工具任务均已结束，无已知由本agent遗留的独立后台构建/测试进程；其他agent/主代理任务需另查。
