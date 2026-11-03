# P05 Runtime 资源、绑定与当前代次（第一阶段）

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

两条写 RPC 的完整回执仍待收尾。已观察到变更 request_id、保留幂等键的重试可能缺原 operation；重试不得披露不同资源的 operation。UpdateConfig 返回的 state.version/desired 还可能与同次提交后的实例权威状态不符。仅凭此组测试不能将两条写 RPC 整体登记为完成。
