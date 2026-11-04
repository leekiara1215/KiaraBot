# Host 错误、独立宿主管理与 bootstrap 交付实施子计划

日期：2026-11-04。状态：**规划交付，未实施，接口待主代理冻结**。本报告不是公共契约，也不是新增能力的验收证据。

## 0. 授权、边界与实施入口

依据当前用户批准和[用户决策登记](user-decisions-2026-11-04.md)，三个范围分别跟踪为 HERR、HOST、BOOT，不再受 2026-11-03 的“保持 not_implemented、不扩契约”范围限制。历史决策和证据保留原日期；实现后逐项登记 target，不能一次翻整族 marker。主代理负责冻结、共享文件和集成，具体任务交给 `gpt-6.1-sol`、`high` 子代理。

本轮唯一写入文件是本报告。未修改产品、共享协议、状态登记、reference 或现有未提交重构；未构建、未运行产品测试。两主机/断网演练继续延期；本计划中的丢响应、进程崩溃、重连是各能力自身的本地故障测试，不扩展为双机项目。没有工期估算。

已阅读根 `AGENTS.md`、`doc/12-cpp-coding-standard.md`、`doc/00-boundaries.md`，以及 `doc/01-contracts.md`、`doc/02-core.md`、`doc/03-plugin-sdk.md`、`doc/06-control-api.md`、`doc/08-operations.md`、`doc/11-toolchain.md` 相关章节、`doc/27-phase-exit-evidence.md` 第 0 节和 2026-11-03/04 用户决策。`doc/18-architecture-baseline.md` 与保护清单只读，不为“同步文档”修改。

所有实现任务继承以下约束：不 revert/reset/stash，不提交他人差异；不编辑生成的 `.pb.*`；不从 Control 查写 Runtime/Host 的库；不开放任意 shell、请求指定的文件路径、包路径、环境值或 SSH；不把 bootstrap 与 Host mTLS、QQ 密码、Identity 管理员密码、实例业务委托混为一种秘密。

## 1. 当前事实与证据限制

本次使用 codebase-memory Verify 工作流。项目 `Users-betta-Workspace-KiaraBot`，generation `2026-11-04T05:19:50Z`，ready，17645 nodes / 63109 edges，partial 37、excluded directories 55。对 15 条证据路径和 3 个范围执行 coverage 核对，结果已完整返回，无分页剩余。符号检索 90 条分两页读完，reference 校验器双向 trace 分两页读完；trace 有通用名称误解析，调用边只作定位，关键结论以源码为准。

`runtime_rpc.cpp` partial 第 639 行已回读上下文；它是删除模板声明，不是 bootstrap 控制分支。`runtime_resources/*`、`owner_gateway/*`、目标 application 和部署 helper 为 excluded，均采用直接源码核对。不能以图里没有符号声称“生产没有能力”。

### HERR 事实

- `proto/tests/contract_status.json:1934,2048` 的 `semantic.host.unknown_operation_error` 仍 not_implemented，证据引用的是迁移前 `core/include/kiarabot/core/application/host_validation.hpp`。
- 实际旧文件在 `core/reference/include/kiarabot/core/application/host_validation.hpp:15`，`OPERATION_NOT_SET/default` 返回 `INVALID_ARGUMENT`；它不属于目标六服务的业务实现。`core/reference/manifest.json` 由 `scripts/quality/check-reference.py` 校验 blob，不能为了翻 marker 修改该文件或保护 manifest。
- 目标 `core/src/runtime/platform/runtime_bridge.cpp:1292,1963` 的 `storage_operation_from_wire` 与 `handle_host` 默认路径已经返回 `CAPABILITY_NOT_SUPPORTED`。空 operation 也会落到这里，现有缺口主要是**缺操作与未知 future operation 的区分、稳定形状测试和正确 target 证据登记**，不是简单替换旧函数的错误码。
- Protobuf future oneof 在旧 reader 中表现为 unknown fields + `OPERATION_NOT_SET`。不能仅凭 `operation_case()` 把缺失、未来操作和未知普通 metadata 全部分类成同一种输入。

### HOST 事实

- `api/openapi.yaml:1760` 的 operation owner 枚举及 `control/api/src/target/owner_gateway/operations.cpp:88` 只接 runtime/scheduler/model，host 当前为 400；`owner_clients.cpp:734` 也无 Host GetOperation 分支。
- Host 已有真实 mTLS HostBridge 主动连接 Runtime、受管进程管理和 SQLite command journal。`core/app/host_composition.cpp` 装配 journal/supervisor/bridge；Host 的 owner mTLS 监听在 `service_rpc.cpp:98` 目前注册 generated denying HostService stub。
- 原 HostService 的七个方法 `exposure=host_bridge`，是 HostBridge 命令目录，不能把它们改成可由 Control 随意调用的 spawn/stop 远程执行入口。
- `host_service.cpp:643` 的 drain receipt 只表示 accepted；循环中逐执行停止并上报 observation，最后 command Completed 不足以证明所有执行已停止。独立维护 operation 必须聚合真实子执行终态，不能拿 `persisted=true` 或这个 Completed 直接当 operation SUCCEEDED。
- Runtime 生命周期已有 Host 参与者，不替代用户批准的独立宿主管理。独立 Host operation 的业务资源是 `RESOURCE_HOST`；删除实例等 Runtime operation 的 owner 和语义保持原来归属。

### BOOT 事实

- `runtime_resources/common.cpp:199` 的 `issue_bootstrap_credential` 生成 32B CSPRNG 文本、持久 SHA-256，有 active credential 时不再返回明文。`account_adapter.cpp:255` 与 `instances.cpp:193` 的局部明文只在首次事务执行路径产生；普通幂等重放不返回秘密。
- `runtime_rpc.cpp:1178,1373` 将首次明文写入 BindAdapter/CreateInstance RPC。当前 managed instance（有 host）也生成明文，和 `runtime.proto`、doc/01 某些“仅 externally_managed”历史说明不一致，必须先冻结受管/外管消费矩阵，不能宣称文档已一致。
- Control `owner_gateway/accounts.cpp:50`、`instances.cpp:113` 剔除 `bootstrap_credential`。普通 REST、WS、operation、日志、审计继续不回显秘密；新增交付流程不能删掉这两处保护作为“修复”。
- `scripts/deploy/target_host_credential.py` 已有 stdin/0600 响应输入、同目录临时文件/fsync/rename、0600、非 symlink、显式 replace、verify/prune；它只消费管理员已取得的一次性 RPC 响应，没有 Control 到部署目标的生产闭环。
- `BridgeCredentialRecord` 只有 id/resource_id/digest/created_at/revoked；连接授权 `credential_version` 当前由常量 `kBridgeCredentialVersion=1` 初始化。安全重新发放需要真实持久单调版本与连接 gate 核对，不能只 revoke/insert 摘要后让旧 proof 持续可用。

## 2. 建议由主代理冻结的最小设计

以下是**建议的新设计**，尚未写入 proto/API，主代理可按现有授权选定并冻结，不再要求用户重复批准。

### 2.1 HERR 分类与传输

在当前认证流、当前 fence/proof 已验证之后做操作分类：

1. operation 缺失且无 future-operation 字段：`INVALID_ARGUMENT`，retryable=false，无下游调用、无幂等写入。
2. 已知 oneof、形状合法但服务未启用/能力未协商：`CAPABILITY_NOT_SUPPORTED`，retryable=false；已知操作自己的输入校验、权限拒绝和依赖故障继续保留相应稳定码。
3. future oneof：`CAPABILITY_NOT_SUPPORTED`，retryable=false，不断言该操作具体业务形状有效，也不产生副作用。
4. malformed protobuf、不能形成合法关联响应的 request_id/trace_id、歧义载荷：按冻结的 transport-invalid 规则稳定拒绝；不伪造 request/trace 以凑响应。

建议冻结一段未来 operation tag 编号区间（例如 10–127；已占 10–17），规定该区间不承载普通 metadata。这是编号分配规则，**不要在 proto 中 reserved 整段**，否则 future oneof 无法追加。旧 reader 只有在区间内出现恰一个 length-delimited unknown 字段、且无已知 operation 时才分类为 future operation；未知区间外 metadata 按 Protobuf 兼容规则忽略，不让它把真正缺操作变成 capability 错误。已知+未知 operation、多个候选、错误 wire type 作为歧义/非法输入；完整规则在 HERR-01 用 descriptor/旧 reader 样例冻结，不能凭 `unknown_fields` 非空粗判。

有效 envelope 的未知/缺失请求使用关联 `HostResponse.error`，result unset；真正 transport-invalid 可终止流，需在契约写清。优先补缺失/未知分支，不顺便改所有正常 Task/Storage/Model 校验顺序。Model streaming 的新 oneof/tag 由模型工作流统一分配；不得在 HERR 中另造 streaming 特例。

### 2.2 独立 Host owner：维护、排空、恢复接纳

最小可用动作是 `BeginMaintenance(drain=true, grace_ms)` 和 `ResumeAdmission`，并提供独立 `GetOperation`、受限 `CancelOperation` 和统一 operation changes。它管理 Host 上部署白名单内的受管插件，不停止 Host daemon、不开放任意 restart/shell、不管理外部自管插件。

- 新 `HostManagementService` 注册在现有 Host owner mTLS `grpc_listen`；不改变原七个 `HostService` host_bridge 方法的 exposure，不增加平台进程。Control 依据离线 Host registry 中 `host_id → owner endpoint` 路由；现有单 `host_endpoint` 可作为单机一条 registry 的配置兼容，不允许浏览器自报 endpoint。
- Host owns SQLite operation、维护模式、接纳水位、审计和本机执行事实。Runtime owns Host 禁派 gate、desired-state/outbox/生命周期和投影；两者通过新 typed 公共端口协调，不跨库事务、不复制同一个权威 operation。
- `BeginMaintenance` 输入 request context、精确 host_id、expected maintenance version、Idempotency-Key、grace（沿用既有上限 10s）；输出 `OperationRef(owner_service=host, resource=RESOURCE_HOST)` 与当前维护版本。`ResumeAdmission` 同样带 expected version 和幂等身份。
- Control 每请求申请 audience=host 的短委托；写动作复用 `host.execute`，读及 changes 复用 `host.read`，均精确 Host scope。Host 依既有 mTLS/Identity authorizer 做当前在线授权；无注册、跨 host、错 audience、过期/撤销委托 fail-closed。隐藏操作返回 NOT_FOUND，不泄露存在性。
- 新 Host→Runtime typed `SetHostAdmissionGate`（名称冻结前可调整）只能由登记证书对应的 Host 请求自身 gate；不是扩大普通 host.execute 为全 Runtime 权限。认证当前 mTLS 证书、Host registration、当前 fence；其调用授权、expected version、operation intent/hash 绑定须逐项登记 target_spec，不能临时放开 Runtime 平台 peer 白名单。

维护时序：Host 先持久受理 operation；worker 在本地事务关闭新 spawn 接纳并写 Running，再请求 Runtime 持久禁派确认（同 operation identity、单调 gate version、重复同内容返回同结果）。Runtime 禁止新派发/迁入，但已有 in-flight apply 仍必须由 Host 本地 gate 拒绝。获得禁派确认后冻结本机待排空执行集合，逐执行按 marker/generation 确认 TERM/KILL 结果。仅 Runtime gate 已确认、全部执行已知停止、所有必要 observations 已持久记录时 SUCCEEDED；上报网络不可用可记参与者 BLOCKED，不能用 heartbeat offline 推断停止。执行 UNKNOWN、SQLite 未知提交、Runtime gate ACK 丢失、Runtime/Identity 不可达均保留原 operation identity 并 BLOCKED/UNKNOWN 恢复。

恢复接纳时序：先核对没有未处置 UNKNOWN/冲突维护意图、当前资源未删除、Runtime gate 对应当前维护代次；以新 Resume operation 明确开放 Runtime gate，确认后持久本地 Admission Open。中途失败本地仍关闭，Runtime 重放 apply 也不能绕过本地 gate。恢复不重建已取消任务、不复活旧 credential/lease、不把旧 PID 或旧 execution generation 当新执行；恢复只允许正常 reconciler 在现有 desired/generation 规则下继续工作。

取消：worker 修改 admission gate 前允许 Cancel，原子转 CANCELLED+NOT_STARTED。worker 已关闭本地 gate或发出 Runtime gate 请求后返回 CONFLICT；不能把不可逆的已停止进程“取消恢复”。恢复接纳必须显式 Resume。请求取消/超时不撤销已持久受理 operation；继续用原 ref 查询。FAILED 仅用于已知拒绝/已知失败，UNKNOWN 不能塞进 KNOWN_FAILED。终态保留至少既有 Host resolved 7d；未决与恢复资料至少 30d，超期进入 needs-attention，不删除造成未知的执行证据。

Host operation change 必须复用 WS 工作流冻结的 `ListOperationChanges(scope,cursor)` 类型/规则，SQLite 保存 Host owner 单调变更序列并与 operation 变更原子提交；游标绑定 owner、scope、subject/authorization version 和保留窗口。不要新造另一套 Host stream cursor。Runtime 引用 Host operation 为参与者，不能重发成 Runtime-owned operation 以消除 host owner 范围。部署多 Host 的游标源标识和 Control 复合 cursor 由 WS 方案统一冻结。

### 2.3 bootstrap：固定 recipient 文件交付 + 显式重新发放

**首版推荐**复用既有 0600 文件机制，增加 Control application 的 `SecretDeliveryPort`；外层实现放在 Control target 适配层，通过受控 helper/固定文件根投递，不引入新秘密数据库或自研加密协议。部署可将该根目录作为只读 secret 挂载给 Host/Adapter/外管插件。外部 secret manager 作为同一端口的后续实现，不是首版验收依赖。

`recipient_id` 来自离线登记，绑定固定部署目标、资源类别、Host/adapter/plugin 身份、owner UID/GID、固定根目录、允许 credential 环境名称。浏览器只能选已有 recipient ID；不能输入 path、owner、env value、URL 或命令。Control 可写投递区，消费者只读挂载；不能给 Control 任意 Host 包目录/系统目录写权限。目标只有本机受控部署根，本次不建设远程部署 agent/第二身份体系。

消费矩阵：

- Adapter：BindAdapter → Runtime digest/version → Control sink → Adapter 独立部署秘密文件 → 外部 AdapterBridge bearer。
- managed plugin：CreateInstance(host_id) → Runtime digest/version → Control sink → 指定 Host 的 `<instance_id>.bootstrap` 0600 → 白名单 credential_environment → SDK PluginBridge bearer。
- externally_managed plugin：CreateInstance(externally_managed=true) → Runtime digest/version → Control sink → 外管部署自己的只读秘密文件/环境 → SDK bearer；不得写 Host 凭据目录或假称由 Host 管理。
- Host 平台身份始终部署 mTLS 证书，不使用上述 bootstrap；QQ/NapCat 密码和 B1 业务父委托不进入本端口。

协议增量建议：现有创建/绑定请求增加受控 `recipient_id` 或明确交付选项；响应保留内部 RPC 的一次性 `bootstrap_credential`，增加无秘密 `delivery_id/credential_version/delivery_operation`。普通 REST 只返回业务资源、operation 和脱敏 delivery metadata，绝无明文。为恢复新增 Runtime typed `GetBootstrapDelivery`、`ReportBootstrapDelivery` 与 `ReissueBootstrap`（最终名字/字段号由 BOOT-01 冻结）。交付 operation owner=runtime；Control 不拥有权威业务库，sink 只保存文件及无秘密投递收据。

签发与恢复规则：

1. Runtime 在原本地 UoW 原子保存业务变更、摘要、真实单调 credential version、recipient binding、无秘密 delivery intent、幂等收据及 runtime operation。事务内不调用 sink。现有 Create/Bind 的首次 RPC 明文仅存在短时内存。
2. Control 收到后直接调用 `SecretDeliveryPort.store(delivery_id, resource, credential_version, recipient_id, secret)`。返回无秘密 receipt；再向 Runtime 报告精确 intent/version/recipient 的交付证据。报告只能来自已注册 Control 平台 peer，仍核对调用者当前管理授权/操作绑定，不能由普通插件自行标记 DELIVERED。
3. sink 写入同目录 0600 临时文件、fsync/rename/目录 fsync；receipt 绑定 delivery_id/version/resource/recipient，禁止返回秘密。文件写入与 receipt 不是跨文件原子事务，必须有可恢复 intent + 阶段记录：崩溃后通过固定目标文件匹配受限摘要和版本补全 receipt，不能仅因 receipt 缺失声称未写。摘要不向公开 API 暴露。
4. 同 delivery_id+同版本+同材料重复 store 为同 receipt；同 identity 异内容 CONFLICT。不同版本替换须当前 expected credential version 和受控 rotation 流程，不用 `--replace` 盲覆写新版本。旧 response/delayed worker 到达不得覆盖更高版本文件；sink 对版本做 CAS。
5. 若 Control 在 sink 成功后丢 ACK/崩溃，按原 delivery_id/version 查询无秘密 receipt，重新报告 Runtime；同一已知交付无需取回旧明文。sink 不可读/结果无法确认保持 UNKNOWN/BLOCKED，不能宣布失败后继续副作用。
6. 若 RPC 响应在到达 Control 前丢失，Runtime 摘要无法恢复旧明文。用原创建/绑定幂等身份查询原业务与 delivery operation，不再次创建资源。若 sink 没有可确认的交付，显式受权 `ReissueBootstrap(expected_credential_version, new Idempotency-Key, same fixed recipient)`：本地事务撤销旧 bootstrap、提升版本、登记新 digest/intent；同键重放只回原 operation/metadata，不重放秘密。重新发放响应再丢失仍不猜测恢复；保持 UNKNOWN，用下一次明确重新发放收敛。
7. 重新发放不得让旧 proof/refresh/fence 继续通过 gate。持久 version 是权威；首连、逐帧/周期失效检查、refresh 均核对当前版本和撤销状态，按既有最长 1 秒门禁使旧流失效，并在新注册时提升 connection_generation。已确认业务历史不回滚，不复活失效授权。
8. 轮换 managed plugin 前必须处于已确认停止/维护 gate；Adapter/外管插件要求部署明确停用并通过 Runtime gate 关闭旧连接。不能把离线等价于进程已停。权限复用精确 `account.manage`（绑定）或 `plugin.manage`（实例），固定部署 recipient allowlist 再限制交付能力；不为方便给 Control 任意 subject 或任意文件权限。
9. cancellation 只允许“尚未签发/交付”的意图转 CANCELLED。签发后取消意味着显式撤销当前 credential，不是恢复旧明文；已经交付后必须完成撤销及部署清理确认，不将 secrets 写入可重放业务收据。资源删除/解绑使 pending delivery 作废，迟到 store/report 被版本及 lifecycle gate 拒绝；Restore 不恢复旧 secret。

可选替代：将 recipient 加密封装并持久 ciphertext 可恢复 RPC 明文丢失，但会新增 recipient key registry、密钥轮换、sealed payload/replay/撤销规则。当前 `RuntimeSecretPort` 只有 CSPRNG/SHA-256，无 recipient envelope 端口，Identity 的加密 secret receipt 也不是 Runtime 凭据交付授权。若后续采用，必须复用已锁 libsodium 1.0.20 的成熟算法与单独外层端口并先审契约；本期不伪称已有该能力，也不引入它作为首版依赖。

## 3. 任务分解与精确文件所有权

下面的文件是**后续实现允许集**，不是本轮已修改清单。`[新]` 表示拟新增；未列出的文件不在子任务授权中，需要主代理先调整所有权。共享契约、CMake、doc/01/02/03/06/08 和 doc/27 只由主代理串行收口；实施者提交补丁建议，不相互覆写。

### HERR-01：冻结 target 分类与证据位置

- 模块：proto/API 共享契约文档；依赖：无，需和模型 streaming tag 分配核对。
- 允许文件（主代理）：`proto/kiarabot/v1/plugins.proto`、`proto/README.md`、`proto/tests/contract_status.json`、`proto/tests/target_spec.json`、`proto/tests/check_semantic_rules.py`、`proto/tests/evidence_scopes_test.py`、`doc/01-contracts.md`、`doc/03-plugin-sdk.md`。
- 输入输出：当前 HostRequest descriptor + §2.1 → 缺失/未来/合法不支持错误矩阵、operation tag 分配规则、target 证据路径。categories reference 历史状态不改写为 target；只修正允许的证据路径与新增独立 target override。
- 验收：future raw-wire 样例可由旧 reader 解析且 retained unknown field；空 oneof与未知普通 metadata 分开；没有改 reference blob/manifest。契约检查见 §4 C1，完成 HERR-02/03 前 marker 保持未实现。
- 文档影响：doc/01、proto README、SDK 错误说明需同步；HTTP 无变更，不改 OpenAPI/AsyncAPI。

### HERR-02：目标拒绝实现与相关性测试

- 模块：Runtime PluginBridge wire 边界；依赖：HERR-01。
- 允许文件：`core/src/runtime/platform/runtime_bridge.cpp`、`core/tests/platform/model_bridge_test.cpp`、`tests/architecture/p06_bridge_acceptance.py`；必要新测 `core/tests/platform/host_request_shape_test.cpp`。新测试登记只由主代理修改 `cmake/platform.cmake`。
- 输入输出/端口：已认证流+HostRequest → 关联 HostResponse.error，无 result，无 Task/Storage/Model 端口调用。只新增缺/未知操作分支；不要把 protobuf 反射带入 Core domain/application/ports。
- 验收：空、future tag、未知metadata、错误wire type、多个候选、已知+future、合法未启用、请求过期、无效关联字段、当前/旧fence、feature未协商全覆盖；有效流未知操作拒绝后仍可执行合法请求。拒绝下游调用计数为零，错误 retryable 确定。
- 验证：§4 B1 构建 Runtime 及测试，C1、T1、Q1。reference 测试不需改；允许把它们作为保护回归运行。

### HERR-03：证据登记与回归收口

- 模块：验收；依赖 HERR-02 实测。
- 允许文件（主代理）：`proto/tests/contract_status.json`、`doc/01-contracts.md`、`doc/27-phase-exit-evidence.md`、`reports/modules/architecture-host-error-alignment.md` `[新]`。
- 输入输出：精确源码/日志/命令/退出码 → 仅本语义 target implemented；保留 reference 缺失操作行为及 blob。不把 host operation/BOOT 顺带翻 implemented。
- 验收：C1、T1、`python3 scripts/quality/check-reference.py`；失败路径不能用宽化断言隐藏。

### HOST-01：维护管理契约与统一变化源冻结

- 模块：Host 管理 public contract；依赖：WS 统一 OperationChanges 接口冻结。
- 允许共享文件（主代理）：`proto/kiarabot/v1/host.proto`、`proto/kiarabot/v1/runtime.proto`、`proto/kiarabot/v1/platform.proto`（只复用 WS 定义的 changes 类型）、`proto/tests/target_spec.json`、`proto/tests/contract_status.json`、`proto/tests/enum_baseline.json`（仅确需新增枚举）、`proto/README.md`、`api/openapi.yaml`、`doc/01-contracts.md`、`doc/01-runtime-semantics.md`、`doc/02-core.md`、`doc/06-control-api.md`、`doc/08-operations.md`。
- 输入输出：§2.2 → HostManagementService、Runtime gate方法、REST `POST /hosts/{hostId}/maintenance`、`POST /hosts/{hostId}/resume`、owner=host operation查询/取消的精确契约。建议 cancellation路径 `/operations/host/{operationId}/cancel`；最终路径唯一来源是OpenAPI。无额外Host WS频道。
- 验收：HOST CRUD/动作权限、deadline（读默认2s/max5s；写受理默认5s/max30s，后台排空不占RPC）、idempotency、expected version、UNKNOWN矩阵、跨库顺序写完整；原HostService exposure无变化。C1通过，状态初始not_implemented。

### HOST-02：Host 本地 operation 与持久维护状态

- 模块：Host 纯应用+SQLite 实现；依赖 HOST-01。
- 允许文件：`core/include/kiarabot/core/domain/target/host_records.hpp`、`core/include/kiarabot/core/ports/target/host_ports.hpp`、`core/include/kiarabot/core/application/target/host_service.hpp`、`core/src/application/target/host_service.cpp`、`core/include/kiarabot/core/ports/target/host_management.hpp` `[新]`、`core/include/kiarabot/core/application/target/host_management.hpp` `[新]`、`core/src/application/target/host_management.cpp` `[新]`、`infrastructure/include/kiarabot/infrastructure/host/host_journal.hpp`、`infrastructure/src/host_journal.cpp`、`infrastructure/tests/host_journal_test.cpp`、`core/tests/platform/host_service_test.cpp`、`core/tests/architecture/host_management_test.cpp` `[新]`。
- 输入输出：typed维护意图/真实执行观察 → SQLite受理、operation/participants/变化序列、维护接纳gate。端口只用Core值类型；SQLite与ProcessSupervisor是外层实现；错误使用现有Failure，不新增私有Result族。
- 验收：同键同内容同ref、异内容CONFLICT；受理未知提交按同键查询；persist先于spawn/stop；真实已停止才成功；单个stop失败/UNKNOWN/重启/PID复用/SQLite错误/取消竞争均不伪成功；operation及change原子提交。恢复不重复spawn、不复活旧代次；保留期测试使用注入clock。
- 构建/测试：B1 的host库/应用和journal，T2、Q1；由主代理登记新业务库INNER及测试，不能避开reference传递链接门禁。

### HOST-03：Runtime 禁派 gate 与 Host 协调适配

- 模块：Runtime application/ports、PG gate；依赖 HOST-01，可与HOST-02按冻结端口并行。
- 允许文件：`core/include/kiarabot/core/ports/target/host_registration_ports.hpp`、`core/include/kiarabot/core/application/target/runtime_host_application.hpp`、`core/include/kiarabot/core/ports/target/unit_of_work.hpp`、`core/src/application/target/runtime_resources/host_commands.cpp`、`core/src/application/target/runtime_resources/host_command_snapshot.cpp`、`core/src/application/target/runtime_resources/lifecycle.cpp`、`core/src/runtime/platform/runtime_rpc.cpp`、`core/include/kiarabot/core/runtime/platform/runtime_rpc.hpp`、`core/app/runtime_composition.cpp`、`infrastructure/postgres/src/host_registration.cpp`、`infrastructure/postgres/src/host_registration.hpp`、`infrastructure/postgres/schema/runtime_host_connections.hpp`、`infrastructure/postgres/src/factories.cpp`、`infrastructure/postgres/tests/host_registration_test.cpp`、`core/tests/architecture/runtime_host_application_test.cpp`。schema版本入口改动由主代理另加允许集。
- 输入输出：当前受信Host的维护identity/version → 持久禁派或明确开放确认；Host pending apply → 保持pending/blocked或稳定拒绝，不修改desired和历史operation owner。
- 验收：旧Host连接、错host、旧gate版本、同id改payload、ACK丢失、Runtime重启、新apply竞态、Resume与Delete并发；Host离线不推断已停，未确认gate不宣告维护成功。事务内无远端调用。
- 构建/测试：B1 Runtime/PG，T2中Host registration/bridge/lifecycle，Q1；既有P13真实删除生产链不能回退。

### HOST-04：Host owner RPC、Control 路由与授权

- 模块：Host transport/composition、Control boundary；依赖 HOST-02/03。
- 允许Host文件：`core/app/host_composition.cpp`、`core/include/kiarabot/core/runtime/platform/host_management_rpc.hpp` `[新]`、`core/src/runtime/platform/host_management_rpc.cpp` `[新]`、`core/include/kiarabot/core/runtime/platform/host_runtime_client.hpp` `[新]`、`core/src/runtime/platform/host_runtime_client.cpp` `[新]`、`core/include/kiarabot/core/runtime/platform/service_config.hpp`、`core/src/runtime/platform/service_config.cpp`、`core/tests/platform/host_management_rpc_test.cpp` `[新]`。
- 允许Control文件：`control/api/include/kiarabot/control/target/owner_gateway.hpp`、`control/api/include/kiarabot/control/target/owner_clients.hpp`、`control/api/src/target/owner_clients.cpp`、`control/api/src/target/owner_gateway/operations.cpp`、`control/api/src/target/owner_gateway/internal.hpp`、`control/api/src/target/owner_gateway/hosts.cpp` `[新]`、`control/api/src/target/owner_gateway.cpp`（只登记handler）、`control/api/tests/owner_gateway_test.cpp`、`control/api/tests/architecture/owner_fixture.cpp`、`control/api/app/main.cpp`（仅装配）以及 `deploy/control/control.example.json`。
- 输入输出：管理员会话精确host scope → Identity短委托 → HostManagementService；owner=host get按已登记host目标路由、owner校验stored resource。无Host端点503，不把host当Runtime alias；未知owner仍400，跨host/不可见404。
- 验收：401/403/404/409/503/504、真实mTLS错叶证书/错audience/撤销委托、HEAD/GET与scope、旧三owner兼容、hostOperationChanges只读当前授权子集；Control不造progress。C1、B1、T2、Q1。

### HOST-05：独立宿主管理生产链验收

- 模块：集成；依赖 HOST-04。
- 允许文件：`tests/architecture/host_management_e2e.py` `[新]`、`tests/architecture/p08_managed_host_e2e.py`（只复用公共夹具/补相关断言）、`reports/modules/architecture-host-management.md` `[新]`；测试登记/marker/doc更新由主代理收口。
- 输入输出：真实Control/Identity/Runtime/Host、私有PG、SQLite、真实SDK子进程 → 维护accepted、禁派、TERM/KILL、已知终态、owner=host轮询、恢复接纳、WS统一changes。
- 验收：正常、未授权、host断连/重启、journal提交未知、gate ACK丢失、忽略TERM、PID复用、重复/冲突、取消窗口、某执行UNKNOWN、迟到旧apply、Resume/Delete竞争、变更cursor过期resync；保留真实本机故障，不要求两主机。T2新测必须注册且实际运行，不用show-only当通过。

### BOOT-01：交付/重发契约和消费矩阵冻结

- 模块：Runtime/Control/deploy公共契约；依赖：主代理选定§2.3最小文件sink，WS共享operation types。
- 允许共享文件（主代理）：`proto/kiarabot/v1/runtime.proto`、`proto/kiarabot/v1/platform.proto`（必要类型）、`proto/tests/target_spec.json`、`proto/tests/contract_status.json`、`proto/tests/enum_baseline.json`（必要时）、`proto/README.md`、`api/openapi.yaml`、`doc/01-contracts.md`、`doc/01-runtime-semantics.md`、`doc/02-core.md`、`doc/03-plugin-sdk.md`、`doc/06-control-api.md`、`doc/08-operations.md`、`deploy/target/README.md`。
- 输入输出：现有首次RPC字段+§2.3 → exact recipient/version/delivery/rotation状态、恢复方法和REST脱敏schema。managed与externally_managed的字段范围先明确，修正文档历史冲突，不以现有实现自动扩大正式契约。
- 验收：无普通REST秘密、无Runtime明文持久化、无新自研crypto；原create/bind幂等业务结果兼容；错误/acceptance/recovery映射、权限、取消、生命周期门禁冻结。已有绑定无明文时必须显式reissue，不尝试从SHA恢复。C1。

### BOOT-02：Runtime 签发版本、交付状态与安全重发

- 模块：Runtime application/ports、PG；依赖 BOOT-01。
- 允许文件：`core/include/kiarabot/core/ports/target/unit_of_work.hpp`、`core/include/kiarabot/core/ports/target/runtime_secrets.hpp`（仅文档/职责必要调整）、`core/include/kiarabot/core/ports/target/runtime_connections.hpp` `[新，如需]`、`core/include/kiarabot/core/application/target/runtime_resources/types.hpp`、`core/src/application/target/runtime_resources/common.hpp`、`core/src/application/target/runtime_resources/common.cpp`、`core/src/application/target/runtime_resources/account_adapter.cpp`、`core/src/application/target/runtime_resources/instances.cpp`、`core/include/kiarabot/core/application/target/runtime_resources/bootstrap.hpp` `[新]`、`core/src/application/target/runtime_resources/bootstrap.cpp` `[新]`、`core/src/application/target/runtime_bridge_application.cpp`、`core/include/kiarabot/core/application/target/runtime_bridge_application.hpp`、`core/include/kiarabot/core/domain/target/bridge_rules.hpp`、`core/src/runtime/platform/runtime_rpc.cpp`、`core/include/kiarabot/core/runtime/platform/runtime_rpc.hpp`、`core/app/runtime_composition.cpp`、`infrastructure/postgres/src/common_work.cpp`、`infrastructure/postgres/src/record_access.hpp`、`infrastructure/postgres/src/factories.cpp`、`infrastructure/postgres/schema/services.hpp`、`infrastructure/postgres/schema/runtime_bootstrap_deliveries.hpp` `[新]`、`core/tests/architecture/runtime_resource_application_test.cpp`、`core/tests/architecture/runtime_bootstrap_delivery_test.cpp` `[新]`、`infrastructure/postgres/tests/bootstrap_delivery_test.cpp` `[新]`。schema版本入口由主代理确认后串行修改。
- 输入输出：typed签发/报告/查询/重发 → 同UoW摘要+version+无秘密intent/receipt/operation；首响应明文一次；query/replay无明文。变更源复用Runtime的WS权威operation changes。
- 验收：CSPRNG/hash失败全回滚；同键同结果/异payload冲突；commit Unknown按原identity查询；并发reissue只有一方版本成功；旧bootstrap/proof/refresh拒绝；禁用/删除/解绑与迟到report竞争fail-closed；不把报告受理当SDK已注册。BO1、T3、Q1。

### BOOT-03：Control SecretDeliveryPort 与固定文件 sink

- 模块：Control application与外层文件适配；依赖 BOOT-01，真实接线依赖BOOT-02。
- 允许文件：`control/api/include/kiarabot/control/application/target/secret_delivery.hpp` `[新]`、`control/api/src/application/target/secret_delivery.cpp` `[新]`、`control/api/include/kiarabot/control/target/secret_file_delivery.hpp` `[新]`、`control/api/src/target/secret_file_delivery.cpp` `[新]`、`control/api/include/kiarabot/control/target/owner_gateway.hpp`、`control/api/src/target/owner_gateway/accounts.cpp`、`control/api/src/target/owner_gateway/instances.cpp`、`control/api/src/target/owner_gateway/bootstrap.cpp` `[新]`、`control/api/src/target/owner_gateway/internal.hpp`、`control/api/src/target/owner_clients.cpp`、`control/api/app/main.cpp`、`control/api/tests/secret_delivery_test.cpp` `[新]`、`control/api/tests/owner_gateway_test.cpp`、`deploy/control/control.example.json`。若实际Control application构建目录不同，由主代理先核准，不自行横跨infrastructure。
- 输入输出/端口：一次RPC明文+离线recipient → SecretDeliveryPort无秘密receipt → Runtime report；普通REST仍erase明文。内层值模型不含protobuf/JSON/filesystem，外层文件实现不决定管理权限。
- 验收：路径穿越/symlink/硬链接/错误owner/权限/disk满/rename或fsync失败/Crash每阶段/receipt丢失/跨recipient/旧版本迟到/当前授权撤销；拒绝时无越界文件副作用。无secret argv/log/stdout/HTTP/WS，sink元数据也不公开digest。严格使用固定路径和原子写入，不从请求拼shell。B1 Control、T3、Q1。

### BOOT-04：部署 helper、受管与外管消费者接线

- 模块：deploy、Host、Adapter/SDK部署读取；依赖 BOOT-03。
- 允许文件：`scripts/deploy/target_host_credential.py`、`scripts/deploy/tests/host_credential_test.py`、`deploy/target/README.md`、`deploy/control/control.example.json`、`deploy/compose.target.yml`（仅受控secret挂载）、`deploy/secrets.manifest.example`、`core/src/application/target/host_service.cpp`（若需版本读取/验证）、`sdk/python/src/kiarabot_sdk/runner.py`（仅实际需要的文件入口；实施前核对现有读取模块位置）、`sdk/python/tests/test_host_managed.py`。Adapter入口具体文件在第一批源码核定后由主代理显式追加，当前不授权修改未知路径。
- 输入输出：recipient固定文件 → Host白名单env/SDK或Adapter配置入口；保留既有外管部署独立目录。优先无需SDK/Adapter代码变更：利用现有env/file启动配置验证，确需新增读取才开允许集。
- 验收：0600/read-only挂载、root/owner验证、不同版本必须显式rotation、替换后旧进程与proof失效、prune不删active、资源删除清理、外管不冒充Host受管；secret不得出现在Compose展开输出/进程argv/诊断中。B1受影响目标、T3 helper/SDK测试、Q1。

### BOOT-05：本地真实交付与恢复链验收/字段登记

- 模块：集成；依赖 BOOT-02/03/04。
- 允许文件：`tests/architecture/bootstrap_delivery_e2e.py` `[新]`、`tests/architecture/p08_managed_host_e2e.py`、`reports/modules/architecture-bootstrap-delivery.md` `[新]`。字段target/文档收口由主代理处理，不能拿现有fake fixture生成明文翻marker。
- 输入输出：真实Control HTTPS/Identity/Runtime/私有PG/Host SQLite、文件sink、真实SDK与本地Adapter fixture → 一次秘密交付、注册成功、普通REST脱敏、幂等恢复。
- 验收：分别覆盖Adapter/managed/externally_managed；RPC提交后响应丢失、sink写后ACK丢失、Control崩溃重启、旧worker晚到、Runtime重启、重发响应再丢失、重复与并发、权限撤销/recipient错配、删除后迟到、取消与恢复；资源数不增长、旧材料无效、无secret泄漏。不能将secret delivered与plugin registered/process running/config applied混成同状态。

## 4. 构建、测试、格式与验收命令

本节命令是**后续执行要求**，本轮未执行。固定依赖沿用doc/11：C++20，PG16.4、Protobuf5.29.3/gRPC1.71.0、OpenSSL3.5.0、libsodium1.0.20、SQLite3.49.1、Python3.11.18；不升级锁。

### C1：协议/API门禁（共享文件修改后串行）

```bash
python3 proto/tests/check_enum_consistency.py
python3 proto/tests/check_implementation_status.py
python3 proto/tests/check_semantic_rules.py
python3 proto/tests/evidence_scopes_test.py
./proto/tests/check_contracts.sh
python3 scripts/quality/check-reference.py
```

完整check_contracts需要锁定protoc/descriptor工具与生成Python包；使用隔离build产出的 `KIARABOT_DESCRIPTOR_COMPATIBILITY_TOOL`，缺依赖exit2必须报告，不当通过。REST/WS schema还须跑仓库现有API解析/生成门禁；HOST/BOOT与WS共享schema合并后运行，不能只做字符串搜索。

### B1：隔离构建

子代理不得并发写主树 `build/full-locked`。建议每流一份包含**当前未提交重构快照**的隔离工作区和自己的binaryDir，依赖安装只读共享；只从HEAD创建空worktree会丢失当前重构，不能直接这样验收。主代理准备统一快照清单并分配工作区，禁止用reset/stash搬运。

```bash
cmake --preset full-locked -B build/host-bootstrap-plan-exec -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/host-bootstrap-plan-exec --parallel 2
ctest --test-dir build/host-bootstrap-plan-exec --show-only=json-v1
```

`host-bootstrap-plan-exec` 仅示例隔离binaryDir；实际每worker单独命名，确认vcpkg安装根不可并发变更。show-only只核注册，不算实测。新增目标和测试名由主代理先登记后执行；没有登记的正则不得声称覆盖。

### T1：HERR相关回归

```bash
ctest --test-dir build/host-bootstrap-plan-exec -R '^(p06_bridge_acceptance|p06_runtime_plugin_capability|p11_model_bridge|p09a_runtime_storage_application|p06_client_e2e)$' --no-tests=error --output-on-failure
```

加上HERR-02新shape测试的精确注册名。raw-wire未知字段与无下游副作用断言必须实际执行，不能只编译。

### T2：HOST相关回归

```bash
ctest --test-dir build/host-bootstrap-plan-exec -R '^(p08_host_service|p08_host_journal|p08_host_bridge|p08_host_bridge_server|p08_postgres_host_registration|p08_host_bridge_e2e|p08_managed_host_e2e|p13h3_joint_delete_production_e2e|p12_control_owner_gateway)$' --no-tests=error --output-on-failure
```

加上Host management unit/RPC/e2e精确新名与WS统一changes测试。Runtime生命周期仍必须等真实Host与Scheduler完成才能DELETED。

### T3：BOOT相关回归

```bash
python3 scripts/deploy/tests/host_credential_test.py
ctest --test-dir build/host-bootstrap-plan-exec -R '^(p05_runtime_resource_application|p05_runtime_catalog_e2e|p06_bridge_acceptance|p06_client_e2e|p08_managed_host_e2e|p08_config_conflict|p12_control_owner_gateway)$' --no-tests=error --output-on-failure
```

加上bootstrap应用、PG、sink、真实e2e的新精确名；如修改SDK/Adapter读取，分别执行其锁定环境pytest。对测试日志/HTTP/WS/SQLite/PG/outbox/receipt做随机canary秘密泄漏扫描，只报告匹配数量/位置，不能打印值。文件sink的目标0600秘密文件属于设计内唯一明文落点，不将它误报成日志泄漏。

### Q1：C++与边界门禁

```bash
python3 scripts/quality/check-boundaries.py
python3 scripts/quality/check-architecture.py --inner
python3 scripts/quality/check-architecture.py --links build/host-bootstrap-plan-exec/architecture-targets.json
python3 scripts/quality/check-architecture.py --database build/host-bootstrap-plan-exec/compile_commands.json
python3 scripts/quality/tests/architecture_test.py
./scripts/quality/check-format.sh
KIARABOT_TIDY_BUILD_DIR=build/host-bootstrap-plan-exec ./scripts/quality/check-tidy.sh
git diff --check
```

子任务先对所改C++运行锁定clang-format和改动TU analyzer，再由主代理最终串行运行全仓门禁/全量CTest及必要 `scripts/check-toolchain.sh`。doc/27第0节已有未改文件format/tidy失败，必须保留基线、单独报告新增诊断，不能写“全部质量全绿”或顺手改无关文件。

## 5. 文档影响、冲突与首批下发

文档影响判断：这三项改变客户端错误、Host owner/API/运维状态、bootstrap配置和交付/恢复行为，**均有文档影响**。必须更新的路径已在HERR-01/HOST-01/BOOT-01列出；新增公共API先改唯一来源，再实现。`doc/05-infrastructure.md` 在Host SQLite schema/PG端口和固定文件sink职责落定时补最小章节；`doc/11-toolchain.md` 仅新增目标/命令确有变化时更新；`doc/12-ci-cd.md` 仅CI/部署流程实际改变才更新；`api/asyncapi.yaml` 的操作事件/游标由WS工作流统一更新，不由Host重复编辑。

无需更新：`doc/04-nonebot-adapter.md`、`doc/07-control-panel.md` 若只复用现有秘密注入/未做面板交互，不新增无关说明；若实现新增Adapter配置或Panel动作，必须纳入允许集并同步。`doc/18-architecture-baseline.md` 不更新，冻结基线若与新独立Host管理/交付责任有差异，在最终报告列差异供人类维护者处理。纯内部helper拆分不另重写架构文档。`reports/analysis/user-decisions-2026-11-03.md` 保持历史，不覆盖旧用户决定。

共享文件冲突集中于：`proto/kiarabot/v1/{plugins,platform,runtime,host}.proto`、`proto/tests/{target_spec,contract_status,enum_baseline}.json`、`api/{openapi,asyncapi}.yaml`、`core/src/runtime/platform/runtime_bridge.cpp`（HERR/streaming/BOOT gate）、`runtime_rpc.cpp`（HOST/BOOT）、`runtime_composition.cpp`（HOST/BOOT）、`unit_of_work.hpp`、`infrastructure/postgres/src/factories.cpp`、`host_service.cpp`、Control `owner_gateway.hpp/owner_clients.cpp/internal.hpp/main.cpp`、三处CMake和doc/01/02/03/06/08/27。主代理分配单owner或串行patch；不要让各worker独立重生成公共输出/改整文件格式。

推荐首批：

1. 主代理串行冻结HERR-01、HOST-01、BOOT-01；先与WS统一OperationChanges和模型streaming分配tag/types。冻结的是可实施设计，不是implemented状态。
2. HERR-02可先下发，严格避开streaming共同编辑的handle_model区域，目标是缺操作/未知分类与测试。
3. HOST-02在Host/SQLite独占文件推进；HOST-03单独负责Runtime/PG gate，`unit_of_work.hpp/factories.cpp`由主代理保留串行所有权。二者接同一冻结端口。
4. BOOT-03可先做纯application/sink与崩溃恢复测试；BOOT-02接Runtime持久版本和撤销gate，触及runtime_bridge的patch由主代理在HERR/streaming后集成。
5. 实测后再下发HOST-04/05、BOOT-04/05和HERR-03登记；统一快照和最终门禁以后，才在doc/27注明各能力实际完成范围。

本轮实际检查：完成源码/文档/图coverage只读核对；报告仅做Markdown路径与diff检查。未运行产品构建、CTest、clang-format、clang-tidy或契约解析，因为本轮只允许计划文件且不修改契约/代码，避免共享构建干扰。后续实际文档更新和命令结果由相应任务交付逐项回填，不能引用本报告当通过证据。
