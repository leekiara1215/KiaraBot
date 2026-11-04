# P05 Runtime 资源、绑定与当前代次（第一阶段）

## 2026-11-04 已批准的实施范围

维护者批准按创建/目录授权与五项持久回执两份方案推进，使用独立 worktree、teammate、能力颗粒度本地提交。持久回执只在隔离新库验收，保留数据的原地迁移另行审查。通过原生检查后重建 Compose 镜像并使用新项目、新卷验收；不推送。目录完整未知对象发现、Host 持久登记等两份方案明确列出的后续项继续按各自阶段处理。

文档影响包括 proto/runtime 与 OpenAPI 唯一来源、doc/01、doc/02、doc/06 的授权/分页规则，doc/08、doc/22 的新库初始化与回执语义，以及必要的 proto/README、运行语义和实际测试报告。doc/18 只读。原地迁移工具和升级已有部署库不在本轮验收内。


基点：`main`（P06/P07 契约前快照）+ P05 分支 `codex/p05-runtime`。本报告只描述本分支已实现
并实际验证的范围；未接线部分见“未完成/降级项”。受保护 doc/18 未修改。

## 目标模块与允许修改范围

- Core：`core/include/kiarabot/core/{domain,application,ports}/target/`、`core/src/application/target/`、
  `core/src/runtime/platform/`、`core/tests/architecture/`、`core/app/runtime_*`。
- 基础设施：`infrastructure/postgres/{schema,src,tests}`（Runtime 私有库）。
- 构建：`core/CMakeLists.txt`、`cmake/platform.cmake`。
- 文档：本报告、`doc/02-core.md`。契约 `proto/`、`api/` 未修改（目标状态仍为 not_implemented，
  原因见下）。

## 输入输出与责任边界

Runtime 应用层接收类型化命令上下文（认证上下文、请求/链路 ID、幂等键、fingerprint、deadline、
expected_version）与命令载荷，返回类型化记录或稳定 `Failure`。业务规则全部在
`domain/target/runtime_rules.hpp` 与 `runtime_resource_application.cpp`；PG 仓储只执行
owner 本地事务；gRPC 适配层只做协议转换、deadline/幂等键/请求 ID 校验与稳定错误映射，不接受
请求自报身份。Control/Identity 不进入 Runtime 私有库。

已实现能力：账号 Create/Update/Disable/Get/List；显式 Bind/Unbind；Adapter 注册/心跳的
应用接口（供 P06 Bridge 复用）与 Get/List；ValidateManifest、安装 Create/Get/List、实例
Create/Get/List、GetInstanceConfig、SetDesiredState、UpdateConfig；GetResourceState、
GetOperation。9 个 P06 `internal_dispatcher` unary 方法不在 P05 生产传输中实现。

## 错误与状态语义

- 预期版本不匹配不覆盖（CONFLICT/ReadCurrent）；禁用/删除资源与未注册连接 fail closed。
- RegisterAdapter 只在 Runtime epoch 下分配/递增 `connection_generation`；adapter Fence 不填
  `execution_generation`（外部 adapter 无受信进程执行，P08 才建立可信绑定）。旧
  connection_generation 后续帧 CONFLICT，旧 runtime_epoch 帧 UNAUTHENTICATED。
- BindAdapter 必须在 RegisterAdapter 之后；注册只校验声明账号存在/可用/未绑定其他 adapter，
  不自动建绑；声明账号数量有界且去重。
- 每条变更命令：在线授权（decision 的 subject/permission/exact scope/版本/新鲜度必须完全绑定）、
  与权威库时间比较 deadline、同 UoW 提交 receipt/audit/operation；幂等重放返回原快照并恢复原
  `OperationRef`（operation id 取自命令身份）。
- List 是当前权威读取：keyset（`id>after ORDER BY id LIMIT n`）+ 服务端不可变 opaque cursor，
  绑定主体/查询/授权版本/page_size/十分钟窗口；非法、跨主体、跨版本、改页大小则
  CONFLICT(CorrectInput)；依赖或授权故障整页失败，不返回空页/部分页；逐项按 exact resource
  过滤当前授权，catalog 权限不授予 child。跨页不冻结行集（与 Identity session snapshot 不同）。
- Readiness 由组合根按真实观测判定：应用已装配 + Runtime 私有库/schema writer 可用 +
  Identity 当前授权链路可用才 ready=true；未观测保持 Unverified/ready=false。单测 fake 只验证
  判定逻辑，不能使组合进程 ready。

## 持久化

Runtime 私有 PG 库新增 `adapters`、`adapter_bindings`、`hosts`、`instance_configs` 关系与
`result_*` 快照，`installations.manifest`、`instances.desired` 列；共享 `replay_results.kind`
范围扩到 0..18，`ReplayResult` 新类型追加在既有索引之后。该版本只有 v1 空库初始化、无迁移
机制，因此结构变更全部写入同一初始化 schema。使用 PG16.4 临时空库实测并写回：
Runtime descriptor `6cfedcb0c23c88b128986d041e601f44`、relation_count 81、schema version 6；
identity/scheduler/model 因共享 schema 变更同步更新 descriptor 与 relation_count，版本分别
11/6/6。

## 验证

本地固定环境（vcpkg/PG16.4/OpenSSL3.5，`full-locked` preset）：

- `cmake --build --preset full-locked --parallel 3`：通过（运行时告警仅重复静态库链接提示）。
- `ctest --preset full-locked -R 'p05|p02|p03_postgres|p04_identity_rpc|p03_service_composition'`
  ：全部通过。
- 全量 `ctest --preset full-locked`：除环境/负载项外通过。`p01_target_api` 需要
  `npm ci --prefix scripts/ci`（本 worktree 未安装 node_modules，属环境限制）；`p01_target_roundtrip`
  与 `p04_control_auth_https` 在串行全量负载下曾超时/连接计数抖动，单独重跑均通过。
- clang-format 16.0.6 对所有改动 C++ 文件执行；`git diff --check` 通过；未运行 tidy/boundaries
  全量脚本。

## 集成收尾 A：P06 外部 Bridge bootstrap 凭据（Runtime 侧）

rebase 到含 P04/P06 契约的 main 后新增：`RuntimeSecretPort`（32 字节 CSPRNG opaque + SHA-256）、
Runtime 私有 `bridge_credentials` 表（只有摘要、active 唯一索引、撤销标记）、
`BridgeCredentialRepository`/`active_bindings` 仓储。BindAdapter 创建新绑定且该 adapter
instance 无 active 凭据时生成一次并放入 `RuntimeBindAdapterResponse.bootstrap_credential`；
CreateInstance 仅 externally_managed 生成并放入 `RuntimeCreateInstanceResponse.bootstrap_credential`；
明文只在响应中出现一次，解绑最后一个绑定撤销凭据。运行时应用测试覆盖“摘要存储/不重复发放/
撤销”，PG typed 测试覆盖 active/insert-conflict/revoke；schema 重新在 PG16.4 空库实测：
Identity descriptor `0d9975965aa851b433887e949f782167`/129/v13，Runtime
`f3c187c4cb886d70e6ae5f627ffaede6`/84/v7，Scheduler/Model 保持 71/66/v6。

## 集成收尾 B：Runtime 在线授权接入真实 Identity

新增 `RuntimeIdentityClient`（`core/src/runtime/platform/runtime_identity_client.cpp`）：
Runtime→Identity 的 mTLS unary 客户端，实现 `RuntimeAuthorizationPort`（`CheckAuthorization`，
500ms/1s 预算、命令 deadline 取更早、失败 fail-closed）与 PeerAuthorizer（真实 inbound 证书叶
指纹 + 委托公开 claims 结构校验，audience=runtime、有效期≤60s、client_certificate 与叶指纹
一致；公开 claims 只作请求元数据，业务授权仍由在线 CheckAuthorization 决定）。组合根新增
Runtime-only `identity_endpoint` 配置（复用服务 TLS 作为出站 mTLS；缺失时保持 fail-closed 与
ready=false），readiness 依据 channel READY + DB/schema + 应用装配判定。delegation proof 只
驻留请求内存，不进入收据/审计/日志/operation。

测试：`p05_runtime_identity_client` C++ 单测（JWT claims 解码正/负例、fake Identity gRPC server
允许/拒绝/不可达 fail-closed）；`p02_service_config` 覆盖 identity_endpoint 正/负配置。

## 未完成 / 降级项

1. **Identity 真实授权链路未接线 → 已由 P05 follow-up 关闭**：组合根在配置
   `identity_endpoint` 时注入真实 `RuntimeIdentityClient`（mTLS CheckAuthorization/委托校验），
   缺配置时保持 fail-closed。真实双进程 e2e `p05_runtime_identity_e2e` 覆盖允许、撤销后拒绝、
   凭据撤销、Identity 不可用 readiness=false 与缺配置 fail-closed；剩余未标注 `scope_kinds`
   的方法仍 fail closed（见 follow-up 报告）。
2. **bootstrap_credential 未生成/返回**：`RuntimeBindAdapterResponse.bootstrap_credential` 与
   `RuntimeCreateInstanceResponse.bootstrap_credential`（externally_managed 一次性 opaque）依赖
   P06 C1 契约，当前分支不含该契约，按 Lead 指示 deferred 到 rebase 后的小 commit。
3. **契约状态未翻 target implemented**：`proto/tests/contract_status.json`、
   `api/openapi.yaml` 的 target 标注保持 not_implemented，未伪造状态。真实授权链 e2e 只证明
   Identity/Runtime 边界与平台 peer 供给；Runtime 业务方法仍有未标注 scope/未实现能力，
   翻转须按方法逐项验收后另行登记。
4. **Host 登记写入口**：按裁决只建记录与仓储，RegisterHost/ReportInventory 等写流程归 P08。
5. **RPC 传输测试 → 已补真实 e2e**：`p05_runtime_identity_e2e` 使用真实 Identity/Runtime
   双进程、真实 mTLS/委托/PG16.4 验证 Control→Runtime 业务命令的允许与撤销后拒绝。
6. Register/heartbeat 拒绝路径当前只写成功审计；拒绝审计与 P06 Bridge 一起补（避免在 P05 单机
   驱动路径引入半套审计语义）。

## P05 follow-up（2026-11-02）

新增离线平台 peer 供给（`kiarabot-identity-bootstrap` 的 `platform_peers`，Runtime/Scheduler/
Model 的 Principal+clientAuth 证书凭据+显式 assignment，同 once-only 事务）、Runtime exact
资源方法的既有 `scope_kinds` 元数据、descriptor 兼容工具 additive-option 规则与真实双进程
mTLS/PG e2e。过程中修复两个真实链路缺陷：委托 claims 的规范十进制字符串编码（Runtime
decoder 误要求 JSON integer）与 Assignment 收据 24h<7 天最小保留期。详见
`reports/modules/architecture-p05-peer-provisioning.md`。可关闭 P05“Identity 真实授权链路未接线”
与“RPC 传输测试未验”两项；Create/List/GetResourceState/GetOperation 的 scope 与应用输入、
P06 bootstrap_credential 消费、生产部署编排仍在后续任务。

## P05f 退出证据矩阵（2026-11-02，main@439fb9e）

本节补退出证据与 Runtime target 状态核对，不改变实现/契约；历史段仍作为当时实现记录。

### 1. 本机验证（worktree `codex/p05f`，HEAD `439fb9e`）

```bash
cmake --preset full-locked && cmake --build --preset full-locked --parallel 6
ctest --test-dir build/full-locked \
  -R 'p04_identity|p04_control_auth_https|p05_runtime|p03_postgres|p02|architecture_p02' \
  --output-on-failure -j 1
```

结果：**46/46 passed**，全量构建 0 error。Runtime 直接相关：
- `p05_runtime_rules`（1.46s）：应用规则/幂等/版本/授权失败传播。
- `p05_runtime_resource_application`（0.74s）：资源/绑定/安装实例/配置 applied。
- `p05_runtime_identity_client`（2.12s）：Runtime→Identity 在线授权/委托 claims/不可达 fail-closed。
- `p05_runtime_identity_e2e`（15.04s）：真实 Identity/Runtime 双进程 mTLS/PG16.4，允许、撤销后拒绝、身份不可达 readiness、缺配置 fail-closed。
- 关联：`p03_postgres_database`（真实 PG16.4）、`p02_core_process`/`p02_service_config`、`p04_identity_*` 28 项。

### 2. Runtime target 状态核对（契约只读）

- `RuntimeService` 方法 target_spec 标 implemented：**10/55** — SetSharedMembers、DisableSharedNamespace、CreateSharedNamespace、GetSharedNamespace、GetOperation、BeginDelete、RestoreResource、QueryActionResult、ListAudit、ListLogs（P09a-2/P13a/P12e + P12c-7）。
- target override 已登记：**23 个 Runtime proto message、77 个 Runtime 字段** implemented。
- 未翻的 P05 核心资源方法（实现/本机测试已存在，target 仍未 flip）：CreateAccount/UpdateAccount/DisableAccount、BindAdapter/UnbindAdapter、ValidateManifest、CreateInstallation/CreateInstance、SetDesiredState/UpdateConfig、GetResourceState、Get/UpdateActionPolicy、GetAccount/ListAccounts、GetInstallation/ListInstallations、GetInstance/ListInstances、GetAdapter/ListAdapters、GetHost/ListHosts、GetInstanceConfig。
  - 原因：P05 报告已记录“未翻 target implemented，须按方法逐项验收后登记”；这些方法依赖真实目标栈/HTTP 操作面与逐方法 evidence，不在 P05f 批量翻转；本机 e2e 证据不自动等于生产 target。
- P06 内部 dispatcher 方法（RegisterAdapter/Heartbeat/PublishEvent、RegisterPlugin/HeartbeatPlugin/ClaimEvent/DeliverEvent/AckDelivery/RenewDelivery）target 不翻转：`exposure=internal_dispatcher`，只走同进程 Bridge，不对外客户端。
- P07/P09/P10/P11 范围方法（SubmitAction/QueryAction/CancelAction/ListActionMetadata/InspectActionMetadata、unary storage 方法、AcceptTimerTrigger、ListDeadLetters/ReplayDelivery 等）中部分为 internal/未实现或由后续阶段单独登记，本表不把它们计入 P05 出口。
- `HostRequest.storage_compare_and_set` 仍 not_implemented（Host operation 契约项）；Control operation `host` owner 未在 OpenAPI 枚举声明，P12f 已记录为 400 而非伪造。

### 3. 未翻/延期项与原因

- **bootstrap_credential 字段级**：`RuntimeBindAdapterResponse.bootstrap_credential`/`RuntimeCreateInstanceResponse.bootstrap_credential` 在 P05 生成（仅 externally_managed/新绑定一次），但 target 字段仍 not_implemented；P06 外部 Bridge 消费与端到端证据未完成，字段语义不翻。
- **P05b Runtime 实例委托**：Runtime 获取下游实例委托的生产来源未接线；P11 单链路仍分层证据。**需用户确认委托语义**后再排期。
- **P05 目标方法未翻**：实现+本机测试≠生产 target；翻转须逐方法登记 evidence 与真实环境，非 P05f 范围（本次不改契约）。
- **真实目标栈 live**：本机固定环境 46/46，不代表六服务/PG16.4 生产部署；Docker/Compose/远端 CI/两主机未验。
- **Host peer 生产证书轮换**：platform_peers 离线登记与本地 e2e 通过；生产签发/轮换/吊销未验。
- **跨主机**：网络分区、TLS/撤销/重连未演练（两主机延期）。

### 4. 门禁（HEAD `439fb9e`）

`check_implementation_status.py`、`target_api_test.rb`、`evidence_scopes_test.py`、`check_contracts.sh`（descriptor compatibility）全部 PASS；本次未改 proto/OpenAPI/contract_status，无新增契约漂移。

## P05b-2 Identity 实例主体父委托（B1，2026-11-03）

契约：`reports/analysis/p05b-identity-instance-delegation-contract.md`；wire 新增可选
`IdentityIssueDelegationRequest.instance_id`。Identity 签发支持 Control 管理员为已注册
plugin principal（RESOURCE_INSTANCE binding）签 audience=runtime、TTL≤56s、hops1 父委托；
管理员需 `identity.delegation.issue`、实例 `resource.manage` 与每个 whitelist 权限的 exact
assignment；scope/permission 白名单与跨实例拒绝；签发写 audit。Exchange/CheckAuthorization
识别 source credential/session=管理员、subject=实例的父子委托，用管理员 authority 做
origin/permission 在线校验并校验实例 binding；普通 delegation 不变。真 PG16.4+mTLS e2e
`p05_runtime_identity_e2e` 覆盖正例（父委托→Runtime 叶 Exchange hops2）与非管理员/无 binding/
错 permission/证书不匹配/跨实例/撤销/过期 ttl 断言。未完成：Runtime 获取/刷新与 Exchange
生产 provider（P05b-4/5）、真实 Model/Scheduler 链。

## P05b-6 sealed 实例父委托文件（2026-11-03）

交付：`scripts/deploy/instance-delegation-refresh` + `scripts/deploy/sealed_delegation.py` +
`deploy/instance-delegation-refresh.example.json`。管理员 access token（stdin/0600 file）经
Control 登录取得，工具用 Control mTLS 叶调用 Identity `IssueDelegation(instance_id=...)`，
原子写 0600 regular JSON（fsync+rename+dir fsync，拒绝符号链接/错误 owner/模式/未授权/错误
instance），支持 `--once`/`--interval<56s`。Runtime 读取侧 helper 校验 schema/instance/
installation/audience/白名单/TTL/未过期，文件不是签名信任根；P05b-4/5 provider 接入点。
测试：`p05b_sealed_delegation_file`（单元）与 `p05_runtime_identity_e2e`（真 PG16.4+mTLS
管理员触发→文件→reader；错误 instance、未授权、符号链接、幂等刷新）。未做：Runtime 自动
读取/刷新 provider、多主机内部通道、真实 Model/Scheduler 下游。

## P05b-7 model sealed 委托与 Identity 交叉积修正（2026-11-03）

- Identity 实例父委托新增 model-only 形状：subject 仍由 instance_id binding 解析；scope 恰好
  一个 RESOURCE_MODEL，permissions model.chat/model.call.inspect；管理员 exact assignment 按
  Model 资源，不要求 INSTANCE 交叉；task 形状不变；混合/跨模型/多 Model/非白名单拒绝。
- sealed 文件/工具按 kind 分组：task 默认旧文件名，model 为
  `instance-delegation-<instance>-model.json`；refresh 工具支持 `--scope-kind/--model-id/
  --permissions`；reader 增加 `expected_model` 并拒绝混合 envelope；TTL/0600/原子/审计不变。
- e2e：真 PG16.4+mTLS model 签发→Runtime 叶 Exchange audience=model hops2；sealed model 文件
  reader/幂等；错误 model scope、混合 scope、model.manage、撤销模型 assignment 负例。
- 解锁：P05b-4/C++ reader 可读取 model envelope 并 Exchange；UpdateModel/RotateProviderSecret
  当时缺少 scope_kinds。后续 `p14_native_single_host` 已验证两条 Model 管理 RPC 的精确模型授权，见[阶段退出证据矩阵](../../doc/27-phase-exit-evidence.md)。

## P05b-7 CheckAuthorization 下游解锁矩阵（2026-11-03）

真 PG16.4+mTLS e2e 覆盖：
- Scheduler owner 叶 CheckAuthorization(`kiarabot.v1.SchedulerService/CreateTask`, scope
  INSTANCE)：task child（INSTANCE+INSTALLATION 与 minimal INSTANCE-only）allowed=true；跨实例
  scope 返回 allowed=false/PERMISSION_DENIED；结构过期 child 返回 UNAUTHENTICATED。
- Model owner 叶 CheckAuthorization(`kiarabot.v1.ModelService/Chat`, scope RESOURCE_MODEL)：
  model child allowed=true；跨模型 scope 拒绝。
- 父委托仍由管理员 exact assignment 控制；撤销后 Exchange/Check 失败。该回归证明 Scheduler/
  Model 下游对 B1 hops2 子委托的在线授权链可用，P05b-5 可继续 CreateTask 真实链。若调用方
  使用 Runtime 叶调用 CheckAuthorization，Identity 会按 recipient owner 规则拒绝
  (UNAUTHENTICATED)，必须由实际 Scheduler/Model mTLS peer 发起。

## P05b-9 production ExchangeDelegation budget 根因（2026-11-03）

build-deploy P05b-4 的 production Runtime sealed model chain：ExchangeDelegation 返回
InvalidArgument/UNAUTHENTICATED，而测试侧长 deadline 直连成功。对照证据（`/tmp/p05b9-handoff`）：
wire、sealed envelope、Runtime 叶指纹（Identity 实测 peer_cert==request_cert）、context 均合法；
Identity audit 无 ExchangeDelegation 记录，说明在 application 前被拒。根因是
`RuntimeIdentityClient` 的 Exchange 复用了 CheckAuthorization 的 500ms/1s 热路径预算；Exchange
需要两次在线授权+签名，production 负载下第二次授权前 deadline 已耗尽。

修复：`RuntimeIdentityClientOptions` 增加 `delegation_budget{2000}`/
`delegation_maximum_budget{5000}`（仅 Exchange；Check 保持 500ms/1s），create 校验有界；
无 proto/RPC/权限/配置字段变更。`p05_runtime_identity_client` 增加 700ms 慢 Exchange 单测与
非法预算负例；build-deploy 已验证同设计下完整 model chain 正向矩阵与 fail-closed fixture 通过。
详见 `reports/modules/architecture-p05b9-exchange-delegation-budget.md`。

## P05b-10 真实 task PluginBridge 单链（2026-11-03）

新增 `tests/architecture/p05b_task_chain_e2e.py`：真实 Identity/Runtime/Scheduler + managed
SDK plugin；sealed task envelope 由 refresh 工具写入；SDK `task_create/get/cancel` 经真实
PluginBridge、Runtime ingress 到真实 Scheduler。验证 owner/installation/空 account、幂等回放、
取消窗口、COMPLETED、跨实例、Scheduler 离线、sealed 缺失、无 scheduler_endpoint fail-closed。
修复 plugin profile timer subscription 与 `task.read` 权限词汇表。未扩 proto；真实 QQ/平台
触发与重连矩阵未覆盖。

## P05b-13 安全加固（2026-11-03）

refresh 工具 token 生命周期（`--max-token-age`、过期/权限失败停止+重认证提示）、provider
per-key single-flight（并发仅 1 次 Exchange）、缓存 proof best-effort 清零、reader 可选
`expected_subject` 校验；测试 `p05b_refresh_tool`、`p05_instance_delegation_provider`。主机
core dump/ptrace 边界写入 doc/05/08。未改 proto/契约。

### P05 既有安装与实例的精确资源管理复验

`p05_runtime_identity_e2e` 通过 Control 测试证书向真实 Runtime 发起 RPC，由真实 Identity 在线授权，读写隔离的 PostgreSQL 16.4。fixture 只预置已有安装和外部管理的实例。GetInstallation、GetInstance、GetInstanceConfig 验证精确 UUID 的读取、同 UUID 错误资源种类的 INVALID_ARGUMENT、跨资源 PERMISSION_DENIED；配置尚未写入时返回 NOT_FOUND。

SetDesiredState 通过预期版本 CAS 更新运行意图，UpdateConfig 产生新期望配置版本。两者在相同请求身份重放时不重复写入；换新幂等键并提交旧版本时返回 CONFLICT。GetInstanceConfig 读回 PENDING、applied_version=0，未注册的实例没有运行 fence。上述验收不覆盖创建入口、注册后 execution/connection generation、插件 ACK 或 Host 实际执行。

两条写 RPC 的完整回执仍待收尾。已观察到变更 request_id、保留幂等键的重试可能缺原 operation；跨资源 operation 已在重试路径排除。同一实例内，若调用方把同一幂等键的 SetDesiredState 重试 request_id 改为随后 UpdateConfig 的 operation_id，隔离实测仍会返回该配置操作引用。资源匹配不足以恢复原始命令身份。UpdateConfig 返回的 state.version/desired 还可能与同次提交后的实例权威状态不符。仅凭此组测试不能将两条写 RPC 整体登记为完成。

## 2026-11-03 既有账号和适配器的目标 RPC 复验

`p05_runtime_exact_accounts` 用隔离 PG16.4、生产 Runtime 与 Identity、Control 客户端证书和真实在线授权执行下列用例。夹具预置一个已存在的 Active 账号和一份已有适配器注册记录；账号更新、绑定、解绑、禁用全部经生产 RPC 提交。

- `GetAccount` 读取指定账号及版本。拿相同委托读取另一个账号 ID 返回 PERMISSION_DENIED；`GetAdapter` 以独立的 ADAPTER 范围读取登记的资源 ID、实现名称、生命周期与版本，只有 ACCOUNT 范围的调用失败。
- `UpdateAccount` 校验 ACCOUNT 范围的管理权限、旧版本冲突、同键重试的原结果以及同键异参冲突；`GetAccount` 回读更新后的名称和版本。
- `BindAdapter` 成功后重放返回同一绑定及 operation；重放无秘密。未存在的绑定带非匹配预期版本、已解绑后用旧版本 1 尝试重绑，都返回 CONFLICT。用当前版本 2 重新绑定成功并递增到 3。只有读取权限的调用不能创建或撤销绑定。
- `UnbindAdapter` 拒绝陈旧版本。解绑成功和同键重放返回同一绑定版本和 operation。活动绑定阻止 `DisableAccount`；解绑后只有具备管理权限并提供当前账号版本才能禁用。禁用结果可重放，读取返回 Disabled，后续更新与绑定均被拒绝。

修复前的真实链路接受了对版本 2 的绑定提交 expected_version=1 的请求；修复后在写入及签发前检查显式条件。此项落实原有的单资源条件写约定，已同步 `doc/02-core.md`。未改变 proto/API 字段、错误码、授权范围、私有表或工具链。复验命令为 `ctest --test-dir build/full-locked -R '^p05_runtime_(exact_accounts|identity_e2e|resource_application)$' --output-on-failure`。

本项只覆盖既有对象。CreateAccount、ListAccounts、ListAdapters 的创建与目录授权另行裁定；GetAdapter 对绑定列表的投影及首次 bootstrap 凭据的交付不由本项登记。

### 按重试身份和资源种类核对登记

真实 PG/mTLS 复验中，GetAccount、GetAdapter 均以原有 UUID 配错误 kind 仍能读到记录。两者现返回 INVALID_ARGUMENT；两个成功响应均提供观察时间。UnbindAdapter 曾在正确账号与版本、错误适配器实例的情况下撤销实际绑定；现返回 CONFLICT，随后以原实例和未变版本仍可成功解绑。

目标方法本轮登记 GetAccount、GetAdapter 和 UpdateAccount。UpdateAccount 更换 request_id、trace_id 后以同一幂等键重放，返回原账号快照。GetAdapter 的绑定列表仍无投影，登记只覆盖实际验证的字段。BindAdapter、UnbindAdapter、DisableAccount 首次调用均有 operation，同键换 request_id 后重试，资源快照仍在，原 operation 却丢失。三项 RPC 保留 not_implemented，首次 bootstrap 明文字段也保持原登记。

### 尚待维护者确认的两项变更

1. 创建和目录授权。拟将创建账号、校验清单和创建安装分别绑定到所属资源族的离线登记目录 UUID，创建实例绑定到精确的父安装。创建成功不会自动授予新资源的读写权限。列表沿用 doc/21 的两层核验，每页验证目录授权，再逐项验证独立子资源授权。新的 Identity 接口、存储登记方式与跨服务事务需要单独审查。
2. 五项写操作的持久回执。拟在原业务事务中保存版本化的不可变结果，包含原 operation 引用；实例操作另保存提交时的版本、期望状态和观察时间。同键同参重试须在重新核验权限后返回原始业务结果，一次性 bootstrap 明文不得进入普通回执。旧回执无法由现有数据唯一重建时，升级预检应阻止切换。可停用受影响写入、保留旧版本的重试服务，等全部相关旧回执越过各自的 retain_until 后再切换（普通回执至少七天）。验收需覆盖更换 request_id、进程重启、其间资源已被后续命令修改、同键异参及旧格式升级预检。

两项均为待确认方案。当前 GetOperation 已在读取参与者前比对已授权的请求引用与存储的完整引用。真实 mTLS/PG 回归验证了已获授权的账号不能读取归属实例的 operation，引用归属不符返回 NOT_FOUND。

### 2026-11-04 审查材料与本地回归

创建和目录授权详见 [契约提案](../analysis/p05-catalog-authorization-proposal.md)，五项操作回执详见 [存储及升级提案](../analysis/p05-mutation-receipt-proposal.md)。两份均待维护者审查，尚未实施提案中的协议或存储变更。

Bridge 双连接隔离修复及 P06 容量重投集成后，主构建重新编译 Runtime 和资源应用测试，执行 `ctest --test-dir build/full-locked -R '^p06_(bridge_acceptance|client_e2e)$|^p05_runtime_resource_application$' --output-on-failure`，3/3 通过，86.41 秒。两次 Connect 不再共享响应 writer，旧 refresh 和延迟 sink 不能覆盖当前代次。容量满时真实客户端保持 SQLite 原事件，恢复后同身份重投且仅投递一次。编译仍报告既有警告，未宣称零警告。

前一轮阶段全量回归 163/163 通过，1126.98 秒，执行期间包含其他集成变更，不能作为本次固定提交完整门禁。本轮新代码尚未重建进现有 Compose 镜像。

文档影响限于上述两份待审方案、doc/02 中既有连接隔离规则与 P06 模块的背压验收报告。未改 wire、API schema、scope 或数据库，故修复无需变更 doc/01、doc/06、doc/22 的契约；已登记的兼容性检查保持原实测范围。方案本身通过 diff 和文档链接检查，实施前仍须按其列出的契约和迁移门禁验证。

### P05 目录授权实现（2026-11-04，原生单测阶段）

已按批准的 `reports/analysis/p05-catalog-authorization-proposal.md` 接线 Runtime 目录映射、三条 catalog 输入、父安装精确创建授权、四条 typed 列表的目录及 child 在线授权，并实现有界不可变十分钟快照。PG 快照使用独立 repeatable-read/read-only 事务，逐对象授权前释放数据库视图；ListHosts 仍缺 typed backing，新建资源的 Identity 授权供给流程仍未扩展。

在独立 `build/catalog` 构建 Runtime、Identity、bootstrap、PG initializer、resource application test 与 service config driver 成功。`ctest -R '^(p05_runtime_resource_application|p13_runtime_lifecycle|p02_service_config)$'` **3/3 passed**；新增覆盖错族/缺失映射、subject/scope/授权版本绑定、记录冻结和原 TTL、未签发候选位置、64 个主体快照与 4097 候选预算拒绝。契约 implementation-status、semantic rules、enum consistency 三个 Python gate 均通过。变更 C++ 使用 clang-format 16.0.6；构建保留原有未使用变量/函数及 libpqxx deprecation 警告。

真实 PG/Identity/Runtime e2e 尚未取得通过：一次离线 bootstrap TIMEOUT，后续在进入新增目录路径前，既有 `runtime_instance_management_e2e.py:76` SetDesiredState 重放遇 grpc DEADLINE_EXCEEDED。未降低断言、未扩大授权。目录真实联测将独立补证，target status 本提交不翻 implemented；receipt lane 集成后的全量复测由 Lead 继续。

文档影响：proto、README、doc/01-contracts 已由前置契约提交同步，本提交更新 doc/01-runtime-semantics、doc/02-core、doc/08-operations 和批准提案状态。API/OpenAPI 与 doc/06 由 Control lane 负责同能力同步；不修改冻结 doc/18，不涉及 Adapter/SDK/Panel 协议或其他 owner 行为。
