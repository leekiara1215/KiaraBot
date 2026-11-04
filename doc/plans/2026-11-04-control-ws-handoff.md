# Control WebSocket 工作流交接（2026-11-04）

本文件记录用户要求停止扩展实现、改由其他 agent 接手时的真实工作区状态。原工作流已停止编码；没有回滚、提交、stash 或覆盖其他工作者的改动。本次交接仅新增本文件。仓库 `/Users/betta/Workspace/KiaraBot`，HEAD 基于 `0496b90` 加大量既有未提交重构与并行实现；不得把整个 diff 当作本工作流所有。

## 给接手 agent 的提示词

你接手 KiaraBot 已获批的完整 target Control WebSocket 工作流。继续使用 GPT-6.1 Sol/high。先读当前 AGENTS.md、`doc/12-cpp-coding-standard.md`、`doc/00-boundaries.md`、`doc/06-control-api.md`、`doc/07-control-panel.md`、`doc/11-toolchain.md`、本交接、`doc/plans/2026-11-04-control-ws-plan.md`、`doc/29-approved-capability-expansion-plan.md` 和 `doc/30-capability-expansion-contract-freeze.md`。保留所有既有 WIP，禁止 reset/revert/stash，禁止编辑受保护 `doc/18-architecture-baseline.md`、保护清单、reference blob、生成物。不要把 reference WebSocket 或临时收集 operation ref 当作 target 的完整 owner 数据源。

用户已授权五项能力按依赖继续实现，不需逐步向用户确认；由主代理协调文件锁、契约 gate、共享构建。此交接时用户要求当前 agent 停止，所以你必须先与主代理核对接手范围及锁再恢复工作。不能因首批 operations 可用而取消其他已批准频道、Host 接入、真实 restore epoch hook 或 Panel 恢复。

结构探索使用 codebase-memory skill；最近已查 `Users-betta-Workspace-KiaraBot` generation `2026-11-04T08:26:54Z`，ready、40 partial、55 excluded。Control target/app 和 Core application target 子树 excluded，相关结构结论必须读源码；不要用空图结果断言无实现。shared `build/full-locked` 只由主代理串行 configure/build/生成/重型测试，不能自行重写 compile DB 或并发安装 vcpkg。

## 一、已完成且有实测的切片

### WS-01 契约冻结

- `proto/kiarabot/v1/observations.proto` 当前只有 `OperationChangesRequest`、`OperationChange`、`OperationChangesResponse` 三个消息。
- Runtime/Scheduler/Model/Host 各自追加 `ListOperationChanges`；单 owner、精确 scope，每页当前在线授权，500ms。Runtime/Model `resource.read`、Scheduler `task.read`、Host `host.read`；WS 不新增业务权限。
- `api/asyncapi.yaml` 已冻结 operations/health/adapters/plugins/tasks/logs/messages/control 全部 target 目标边界、首帧、恢复、限额和 close code。新 target 能力登记继续为 `not_implemented`，没有因模块测试通过提前宣告 WSS/Panel 可用。
- `proto/tests/ws_expansion_schema_test.cjs` 有36项 schema 正负例，已由主代理接 API gate 并验收。
- 本工作流 WS-01 文件还包括 `proto/kiarabot/v1/scheduler.proto` 的该 RPC、`doc/06-control-api.md` WS节、`doc/07-control-panel.md` WS节；I/R/M/H proto 中的对应方法由其他 owner 集成。

### WS-02 真实 PG operation journal

已验收 Runtime/Scheduler/Model，同业务 UoW 的 operation 最终快照和 participant-only 修订，事务计数 head 按提交次序推进，600秒和容量 prefix 淘汰推进持久 low-water，普通进程重启保留 dataset epoch。Identity 未改。没有 hook `record_codec` 回执来制造伪变化。

精确文件：

- `core/include/kiarabot/core/domain/target/operation_observation.hpp`
- `core/include/kiarabot/core/ports/target/operation_observation_ports.hpp`
- `core/include/kiarabot/core/ports/target/unit_of_work.hpp` 的 query/prune 端口默认 fail-closed；该文件后续已交给 Model worker，当前不能独占。
- `infrastructure/postgres/src/operation_observations.hpp`
- `infrastructure/postgres/src/operation_observations.cpp`
- `infrastructure/postgres/src/common_work.cpp` 的 dirty operation final flush
- `infrastructure/postgres/src/factories.cpp` 的观察端口及 ModelWork 显式转发；后续该文件已交 Model worker。
- `infrastructure/postgres/schema/operation_observations.hpp`
- `infrastructure/postgres/schema/services.hpp`、`v1.hpp`、`manifest.hpp`
- `infrastructure/postgres/CMakeLists.txt` 的专属测试/源登记
- `infrastructure/postgres/tests/operation_observations_test.cpp`
- `infrastructure/postgres/tests/run_database_test.py` 的新 driver 名登记
- `infrastructure/postgres/tests/scheduler_application_test.cpp` 仅 schema10→11 断言同步
- `doc/05-infrastructure.md` journal 模块章节及 Model streaming 文档链接

实际版本 I15/R19/S11/M10；真实 owned PG16.4 describe 值：Identity 原值 `decc8401bef9d3ac3c92116e7608b2f4`/135 未变；Runtime `3b472a7194e1d5417caeee118e58087f`/162；Scheduler `1d2af0ada31b0233a160a34715870dc2`/91；Model `51d9e7782bb74769dd2a1cc5fee58408`/85。不要猜 digest 或放宽生产 opener 的 expected digest gate。后续 BOOT/新 feed DDL 必须由主代理串行分配并重新真实 describe。

目标 `kiarabot_postgres_operation_observations_test`，CTest `ws02_postgres_operation_observations`，固定后 1/1 passed4.99s。涵盖 rollback、幂等回执无新事件、participant final coalescing、scope/paging、low-water/容量/期限、live prune 不破坏 journal、普通进程重开、epoch 失效、uint64 与真实 PG lock 提交排序屏障。

### WS-03 共用纯应用、真实 Runtime/Scheduler RPC

已验收：事务外在线核验，实际 Control mTLS leaf/SAN 与委派证书绑定，完整当前 scope/permission/subject/version/checked_at 决策，500ms clip，之后只读 owner UoW 并 rollback。空 cursor 只捕获 head，gap 保留 typed Conflict。安全序列化包含闭集 reason、participants、observed_at，拒绝跨 scope 返回。没有扩大 Scheduler 既有 Runtime peer 白名单。

精确文件：

- `core/include/kiarabot/core/application/target/operation_observation_application.hpp`
- `core/src/application/target/operation_observation_application.cpp`
- `core/tests/architecture/operation_observation_application_test.cpp`
- `core/cmake/operation-observation.cmake`；Host worker 已在 `core/CMakeLists.txt` 接入 include。
- `core/include/kiarabot/core/runtime/platform/operation_observation_rpc.hpp`
- `core/src/runtime/platform/operation_observation_rpc.cpp`
- `core/include/kiarabot/core/runtime/platform/runtime_rpc.hpp`、`scheduler_rpc.hpp` 尾部追加 observations pointer。
- `core/src/runtime/platform/runtime_rpc.cpp`、`scheduler_rpc.cpp` 的 ListOperationChanges 分支。
- `core/app/runtime_composition.cpp`、`scheduler_composition.cpp` 最小注入；保留其中其他工作者的 SHA256/既有重构差异。
- `core/tests/platform/operation_observation_seed.cpp`
- `core/tests/platform/operation_observation_e2e_test.py`
- `cmake/platform.cmake` 的共用 RPC library、seed 和两个 WS03 CTest。
- `infrastructure/postgres/tests/run_database_test.py` 只追加本工作流两个 driver 名，不改旧 driver 行为。
- `doc/02-core.md` 观察用例节、`doc/06-control-api.md` 真实 source 节、`doc/22-transactions-and-migration-design.md` 追加 journal/epoch/版本与实测语义。

共用 API：`make_operation_observation_application({owner, begin()->Outcome<unique_ptr<UnitOfWork>>, RuntimeAuthorizationPort&, now})`；`query(OperationObservationCommand{CommandContext,OperationObservationQuery})`。outer `prepare_operation_observation(ServerContext&,OperationChangesRequest&,owner,invocation_deadline)` 和 `encode_operation_observation_page(page,RequestContext,response)`。Model RPC/app/组合根由 Model worker 接，不编辑 `model_rpc.cpp` 或 `model_gateway_application.*`。

另外已按父授权最小扩展 `core/src/runtime/platform/runtime_identity_client.cpp::decode_delegation_claims` 接受 Model audience，并在 `core/tests/platform/runtime_identity_client_test.cpp` 加 Model audience/Control actor 正例及错误 audience/certificate/expiry/default-Runtime 拒绝的负例。该 decoder 只是 metadata；完整边界仍需 prepare 的实际 Control SAN 和纯应用在线 Identity。`p05_runtime_identity_client` 1/1 passed2.09s，已通知 Model worker 可复用。

## 二、当前半成品：WS-04 基础模块，未编译、未接网络

以下是已写入但未通过构建/测试的 WIP，不能标 source-ready 或上线；保留编辑并由接手者复核：

- 新增 `control/api/include/kiarabot/control/target/websocket_types.hpp`
- 新增 `control/api/include/kiarabot/control/target/websocket_protocol.hpp`
- 新增 `control/api/src/target/websocket_protocol.cpp`
- 新增 `control/api/include/kiarabot/control/target/websocket_source_client.hpp`
- 新增 `control/api/src/target/websocket_source_client.cpp`
- 修改 `control/api/include/kiarabot/control/target/identity_auth_client.hpp`：新增 typed `validate_stream`/`delegate_stream` 公共方法。
- 修改 `control/api/src/target/identity_auth_client.cpp`：上述方法复用现有 Identity channel、RPC cancellation registry、`principal_json` 校验，不改原 HTTP 路由。

当前 protocol 模块有目标 route/first-frame/scope/cursor 校验、duplicate JSON key 拒绝、Origin 精确列表匹配、operation 安全闭集 JSON 投影、closeCode 映射。source client 规划每轮 ValidateSession→IssueDelegation→R/S/M ListOperationChanges，总预算500ms；每页验证 owner/scope/游标/安全变化，typed Conflict/NOT_ACCEPTED/RESYNC_SNAPSHOT 才标 owner gap，不读自由文本。

仍须复核的具体问题：这些新 CPP 完全没编译；补显式标准库 include（例如 source client 使用 array/algorithm）、首帧 JSON parser 的最大 nesting 深度限制、安全时间边界与所有正负例；用真正 AsyncAPI schema 校验输出（尤其 observed_at/uint64/blocked/participant）。未知来源当前在认证后显式失败 CAPABILITY_NOT_SUPPORTED→1011，不能把该分支视为频道实现。source client 内没有 Host source，其他非operation source 均未完成。

**尚未创建或修改：** retained stream store、WebSocket session/Beast transport、新 WSS CMake fragment/target/CTest、target Upgrade 接线、Panel target stream client/恢复 UI。`control/api/src/target/https_endpoint.cpp` 仍是原 target HTTPS 一次请求→REST4worker→响应关闭，没有 Upgrade；本轮没有改该文件。`cmake/control-auth.cmake` 当前没有 include WSS fragment。

Host worker 已把 `ControlApiMaterial.websocket_origins` 和 `websocket_allow_missing_origin` 及 service_config validator/parse 落到当前源码；本工作流只给片段，未编辑它们。缺省空 origins+false 应使所有 Upgrade fail-closed；当前尚无 Upgrade，因此配置本身不证明 WSS 可用。Host 的这些配置改动/文档/测试验收状态由主代理及其交接说明确认。

`doc/06-control-api.md` 已有 WS-04“实施中”最小模块说明。该说明是实现目标，不能当实测；其余 target 状态没有翻转。

## 三、v1.2 精确状态：尚无 wire/registry 改动

v1.2 仅完成 source gap 识别与四 RPC 范围批准，**没有新增任何 message、RPC、import、enum、registry/spec/golden 或专项 oracle**。没有生成 v1.2 descriptor，也没有 v1.2 baseline 副本。当前 `observations.proto` 仍只有原三个 operation 消息；I/R/S proto 仍为父已验的 v1.1 加 WS-01 方法。

当前 wire 的最近实际 parser/descriptor/往返成功证据是主代理的 v1.1 gate（见 `build/logs/capability-expansion/identity-v11-contracts-result.json`、`identity-v11-descriptor.pb`、`identity-v11-generation-results.json`）。本交接没有重新运行解析；不能把不存在的 v1.2 描述为 parseable 或 frozen。

主代理在停止前把以下契约窗口转给本工作流单 editor：`identity.proto`、`runtime.proto`、`observations.proto`、`scheduler.proto`、`proto/tests/contract_status.json`、`target_spec.json`、`target_wire_goldens.json`、专项 contracttest、`doc/01-contracts.md` 和 `proto/README.md` 的通用增量。Host 已确认释放 identity/runtime proto，Model/plugins wire仍归 Model owner。**该锁现在随本交接返回主代理重新分派；在新 owner source-ready 前不要从半编辑 v1.2 live proto 做 shared 生成。** 当前没有 v1.2 半编辑源，因此主代理可自行解除生成暂停，不能让过期锁永久阻断其他工作流。

批准的最小后续契约范围（仅提案，名字/字段尚未冻结）：

- 共用 `ObservationChangesRequest`：`context=1,scope=2,optional cursor=3,page_size=4`；保留原 OperationChangesRequest descriptor 不动。
- `IdentityService.ListLogChanges`、`RuntimeService.ListLogChanges`：`log.read`，scope恰为 `RESOURCE_CATALOG/control`；caller为当前注册 Control mTLS peer，I/R沿各 owner既有认证形式。Log typed metadata 只可含安全 entry ID、时间、闭集 DEBUG/INFO/WARN/ERROR、闭集 request.recorded/request.rejected 及必要关联字段；禁止 audit 原payload/free context/秘密。具体字段尚未写入，不要按这个概念描述直接冒充已冻结 schema。
- `RuntimeService.ListMessageMetadataChanges`：核对过 `PERMISSION_MESSAGE_READ=40`，精确真实 `RESOURCE_ACCOUNT`，仅 received 的安全 message/conversation metadata，没有正文、platform raw payload、sent 推断。message/conversation安全ID与平台外部ID如何映射仍需确认；不要未经设计直接透传 platform raw ID。
- `SchedulerService.ListTaskChanges`：`task.read`，精确 `RESOURCE_TASK|RESOURCE_INSTANCE`。安全 TaskMetadata 仅 task_id/owner_instance_id/state/version/updated_at/cancel_requested/optional occurrence_id/optional reason_code，不含 TaskRecord.payload/target 原包；状态枚举已经在 common.proto，避免 observations 反向 import scheduler 引发环。
- 各 typed change 为 metadata+逐项 cursor；各 response 为 changes1/next_cursor2/has_more3/checked_at4。所有新 RPC read-only、no idempotency、default/max500ms、requires_online_authorization=true、状态先 not_implemented；未知/拒绝/依赖失败不能成功空页。
- 各 `(owner,feed family)` 独立持久随机 epoch/head/low-water，方法绑定 feed。不能复用单 owner epoch/head：Runtime operation/messages可同Account scope，Scheduler operation/tasks可同Instance scope。跨 family cursor 双向都必须 typed Conflict/resync，不能解释为另一条流的位置。普通进程重启保持，各恢复/rebuild统一轮换对应持久 dataset epoch。
- 600s、容量上限/低水位、同业务UoW提交序、安全最终快照/去重、REST恢复沿已冻结语义；I/R/S新 DDL/UoW/manifest 仍由主代理后续分派，当前绝对不要并发编辑 PG。

真实必要性证据：`factories.cpp::list_tasks` 仅 SQL `state IN(0,1,2)` 且按 task_id keyset，`scheduler_service.cpp::list_tasks` 后过滤，不是 immutable terminal完整快照；terminal task消失不能推导最终状态。I/R ListLogs 现有 occurred_at+id快照分页无提交序 tail 保证，新事务可有较早 occurred_at，不能靠 Control 已见ID集合补全。Runtime 现无安全 message metadata 查询 RPC。operation journal不能替代这些源。

## 四、剩余实现与验收顺序

1. 补齐并测试当前 WS04 protocol/source 模块，接独立 CMake fragment；原 REST4worker不能被长连接耗尽。沿已锁 Boost.Beast/Asio/OpenSSL，无新增依赖。不要手写 RFC6455 masking/frame/TLS 协议。
2. 实现 bounded retained session store 与 Beast async session，单 stream所有权、单 writer，read/control并发，5s首帧、64KiB、128帧/1MiB待写、4MiB/4096 replay、1024 sessions/64MiB全局保留、600s；active/admission/auth quotas独立。过载1013，终止写真正cancel socket，shutdown取消RPC并join；不能只设无效expiry。
3. 每250ms启动或共享当前认证检查，500ms单轮预算，最近确认超过750ms停止交付并关闭，总撤权≤1s。**没有业务变化/成功空owner页时同样检查**；测试 idle/replay/slow-write 下撤权/会话失效。绑定principal、Identity session、授权版本、频道、owner、规范化scope。
4. 新hello先owner head与SESSION_READY，再REST快照并缓冲；resume重新认证并同时核对client已处理Control sequence与对应owner cursor，先replay再live。owner cursor仅整页消费后推进，客户端仅事件实际应用后推进；Control seq和owner uint64不同。typed owner gap→CURSOR_EXPIRED，local invalid→INVALID_CURSOR，Control进程epoch改变→SERVER_RESTART。
5. R/S真实已具备source，Model worker用共用app/helper接实际Model source；Host source等其独立SQLite/current-authority链，未就绪1011明确失败。health用真实依赖观测；adapters/plugins逐全部selected exact resource真实GetAdapter/GetInstance安全投影比较，忽略observed_at-only。随后按v1.2补tasks/logs/messages真实源及control同owner汇聚，不漏批准范围。
6. Panel仍只有reference `panel/src/stream/control-stream.ts`，`views/p12-live.tsx::OperationTracker`仍2s REST轮询；新增target scoped client而非把reference KNOWN_EVENTS当目标验收。token provider、scope切换、轮换、旧socket失效、首事件前断线、重复/跳序、snapshot race、resume/resync/REST fallback、4401/4403/1013及不重发管理写均须实际测。
7. WS06/恢复还需受控真实离线restore工具/hook：owned PG backup→restore→rotate各feed epoch→旧cursor确定gap，普通restart保epoch；SQLite同型Host负责。局部rotate port与文档约定不足。`scripts/ops`锁届时由父派发，恢复使用备份manifest版本，不能硬编码旧schema。
8. 真实WSS/Panel/各频道覆盖完成后才更新registry/API状态、doc07/doc08/运行说明与最终gate；共享文档/状态写入先核对其他owner。

## 五、文件锁与协作边界

停止前本工作流持有：generic observation app/RPC、Runtime/Scheduler RPC/组合根、`cmake/platform.cmake`、Control新ws文件/https_endpoint/IdentityAuthClient必要方法、Panelstream/OperationTracker/views/App/tests、WSS CMake fragment。Host已释放 `cmake/control-auth.cmake` 给WS，仅允许追加独立fragment，不重写BOOT03目标；尚未实际编辑。

Host继续持有 service_config.hpp/cpp、doc08、Host maintenance/Identity gate及其应用/RPC；Model继续持有 Model应用/RPC/组合根、ModelCallRepository/ModelStore/typed_work_checks Modelblock和相关factory/UoW区域。主代理统一分配PG新schema、manifest、shared构建及Core顶层CMake。不要因为本工作流过去编辑过UoW/factories就重新抢锁。

本文件发出后所有上述本工作流锁归主代理重新分配，其他agent必须兼容当前WIP。不要只取HEAD重建，也不要stage其他人的变更。

## 六、已跑结果、历史失败与下一命令

固定工具：`build/format-venv/bin/clang-format`16.0.6，clang-tidy22.1.8；PG `build/pg-16.4-install/bin`；OpenSSL `build/openssl-3.5.0-install/bin/openssl`；Python `sdk/python/.venv/bin/python`。以下命令均从仓库根运行；shared命令只由主代理排队执行。

已有通过结果：

- WS02：`ctest-ws02-journal-fixed.log`，1/1 pass4.99s；describe日志/result存于 `ws02-describe-schema.log`、`ws02-describe-schema-result.json`。
- WS03 pure app：`ctest-ws03-owner-first.log`内该项pass0.48s。
- WS03真实owner最终：`ctest-ws03-owner-live-session.log`，1/1 pass9.02s；包括撤权后同actualControl channel ValidateSession成功、旧proof精确UNAUTHENTICATED、新同scope IssueDelegation精确PERMISSION_DENIED，及I停机R/S失败。
- Model decoder：`ctest-host02-enum-runtime-decoder.log` 中 `p05_runtime_identity_client` pass2.09s。**同日志另一项p08_host_maintenance曾失败，不是整份日志全绿；该Host问题由Host owner/父处理，本工作流没有验证其后续状态。**
- WS03阶段12个CPP格式dry-run、`python3 scripts/quality/check-boundaries.py`、doc02/06/22本地链接检查均exit0；WSS半成品只有写入后clang-format，没有构建或测试，也没有WSS/Panel evidence。

保留的失败记录与已查原因：

- `ctest-ws02-journal-first.log`：测试错误假设Model原`prune_operations`可用；固定为明确unsupported断言+owned exact cleanup，journal断言未放宽。
- `build-owner-app-host-v11.log`：seed误调用Repository不存在的replace_participants；已改为真实UoW `save_operation_participants`，后续实际build通过。
- `ctest-ws03-owner-first.log` e2e：SDK生成目录缺observations_pb2；父用 canonical generator补齐后通过该阶段。
- `ctest-ws03-owner-bindings-fixed.log`：撤权后旧proof错误期待403；Identity授权版本失效真实语义为401。固定为旧proof精确401+live session+新委派精确403两层证据；没有放宽为任意失败。
- `ctest-ws03-owner-revocation-fixed.log`为1/1 pass9.96s的中间加强版，最终live-session版见上。

下一已有目标构建/回归命令（不是WSS新模块验收）：

```sh
cmake --build build/full-locked --target kiarabot_operation_observation_application_test kiarabot_operation_observation_seed kiarabot_operation_observation_rpc kiarabot-runtime kiarabot-scheduler kiarabot-identity kiarabot-identity-bootstrap kiarabot-pg-initialize kiarabot_runtime_identity_client_test -j 2
ctest --test-dir build/full-locked -R '^(ws03_operation_observation_(application|e2e)|p05_runtime_identity_client)$' --output-on-failure
ctest --test-dir build/full-locked -R '^ws02_postgres_operation_observations$' --output-on-failure
```

canonical Python生成（wire变化后由父统一执行，禁止手改生成文件）：

```sh
sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py
```

新增WSS module尚无CMake目标/测试，不存在可诚实执行的WSS target命令。接手者应先补fragment、专属tests并向父报告准确目标，父统一configure后才build。v1.2应在修改前保存当前v1.1 descriptor，追加后单独parse并做相对v1.1兼容；可参考当前 `build/logs/capability-expansion/identity-v11-descriptor.pb`，不要把更早pre-expansion baseline当唯一比较对象。共享完整 `check_contracts.sh`/API/roundtrip由父排，旧contract_status中Host刚验的语义不要被重写。

本交接时没有未结束独立exec、PG fixture或shared build；本agent最后两个独立CTest session已完成（47890 exit0、17319 exit0），不存在需接续或终止的PID。主代理最新消息明确没有已启动sharedbuild。

## 七、文档影响判断

WS01~03已同步直接契约/模块文档；WS04仅新增实施中模块说明，不代表功能启用。本次交接只新增本文件，不再编辑产品源码、其他docs或registry。其余文档无需因“停止与交接”改写；受保护doc18保持只读，实现差异由主代理交付给人类维护者决定是否修订。交接文档只检查UTF-8、末换行与尾空白；未运行构建/契约/Panel检查，因为本次只记录冻结状态，WSS半成品的未验收风险已明确列出。
