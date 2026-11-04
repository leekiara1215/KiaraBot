# 五项扩展的契约冻结记录

日期：2026-11-04。状态：**契约冻结通过，已放行依赖明确的实现切片；新增能力尚未完成运行验收**。
授权和范围见 [用户决策](plans/user-decisions-2026-11-04.md)，任务见
[主计划](29-approved-capability-expansion-plan.md) 和[执行台账](plans/2026-11-04-capability-execution-ledger.md)。
本页记录跨工作流的一致取舍、唯一来源和实测门禁；不以说明文件代替 wire/API 定义。

## 1. 唯一来源与本轮增量

- owner 变化查询：[observations.proto](../proto/kiarabot/v1/observations.proto)，
  `OperationChangesRequest/OperationChange/OperationChangesResponse`。
  Runtime、Scheduler、Model、HostManagement 各自追加 `ListOperationChanges`，500ms 有界 unary 查询。
- 流式模型：[model_stream.proto](../proto/kiarabot/v1/model_stream.proto)、
  [model.proto](../proto/kiarabot/v1/model.proto)、[plugins.proto](../proto/kiarabot/v1/plugins.proto)。
  新 `ModelService.ChatStream`、HostRequest start/cancel、CoreToPlugin event 和 cancel receipt；
  原 Chat、model_chat、字段编号、全局错误/生命周期枚举保持。
- 独立宿主管理：[host.proto](../proto/kiarabot/v1/host.proto) 的 `HostManagementService`，
  GetMaintenance、BeginMaintenance、ResumeAdmission、GetOperation、CancelOperation、ListOperationChanges。
  原 HostBridge/七个逻辑 HostService 方法的 exposure 不变，不增加平台进程。
- Runtime 协调与凭据：[runtime.proto](../proto/kiarabot/v1/runtime.proto)，新增自身 Host admission gate，
  无秘密 delivery 查询、Adapter/Plugin 各自重发与交付报告。原创建/绑定只增加可选 opt-in recipient。
- 管理 API：[OpenAPI](../api/openapi.yaml)；WS 首帧、envelope、恢复和限额：[AsyncAPI](../api/asyncapi.yaml)。
  API 不普通回显 bootstrap 明文；Owner、权限和资源 scope 各自显式登记。

合计新增 16 个 owner RPC（含 1 个 server-streaming），8 个 REST operation；
新字段/类型/方法/事件保持 `not_implemented`，既有实现状态没有翻转。

## 2. 已核定的跨模块语义

### 授权与操作变化

通用 operation 查询按 owner 分别要求 runtime/model=`resource.read`、scheduler=`task.read`、
host=`host.read`，没有四者并集或额外隐含 `resource.read` 门槛。
Host 写动作要求精确 Host 上的 `host.execute`；自身 admission 协调还核验当前 Host 注册、
实际 mTLS 身份、Fence、维护意图和单调版本，不能扩大其他 Runtime 平台 peer 白名单。

每次变化读取重核当前精确 scope；cursor 不构成授权。owner journal 与 operation/participant
更新同一 UoW，提交序、low-water 与持久 dataset epoch 由 owner 管理。
普通 owner 重启保留 epoch；恢复旧数据/重建 journal 更换 epoch。Control 重启使其流 session 失效。
owner 不可恢复的 cursor 返回结构化 CONFLICT + NOT_ACCEPTED/RESYNC_SNAPSHOT，
Control 保守要求重查快照，不解析自由文本错误来猜测缺口原因。

WS 先按单 owner/明确 scope 建立边界，再读取 REST 快照；恢复不重做管理写。
首批操作/健康/Adapter/Plugin、后续 task/log/message 和 Host 集成各有真实来源与独立验收，
不能以空流、Control 临时 ref 集合、reference 实现或状态简化冒充目标能力。

### 模型 streaming

started=0、delta=1..N、terminal=N+1；受理前拒绝只有 terminal=0。
同 owner/logical_call_id 重放不重调 Provider，不重放正文；cancel receipt 不是上游完成或未计费证据。
完整成功需要合法结束边界及已确认持久结果，partial、UNKNOWN、usage 来源和可能计费分别表达。
在途调用保守表达 `may_have_billed=true`，有确证后才清除；若共用旧 Chat 路径需同时补非流式回归。
只协商端到端可用的 `host.model.stream.v1`，控制帧不能被同步流处理堵塞，队列和并发均有界。

Provider 复用锁定 libcurl，仅计划私有引入固定版本的 eventsource-c C parser，
不启用其 transport/client/重连，不自动重发 POST。依赖精确 hash、标准 data/event 边界、
UTF-8、CR/LF/BOM、空/多 data、容量和 sticky overflow 由实现任务继续验证并登记。
QQ 仍默认聚合后一次 Action；本契约不自行开放网页模型正文或真实 QQ 主动消息。

### Host 与 bootstrap

Host 先持久受理、关闭本地接纳，再与 Runtime 禁派协调并排空有限执行集合；
UNKNOWN/离线不能当停止完成。恢复接纳是新的明确操作，不复活任务、凭据或旧代次。
独立 Host operation 事实归 Host SQLite，Runtime 只保存自身 gate 与参与者投影，Control 不跨库。

recipient 非空才启动新增 bootstrap 自动交付；省略保持既有创建/绑定、版本、幂等和受限部署行为。
recipient 由离线固定目标映射，客户端不能指定路径/URL/命令。Runtime 仅存摘要和真实凭据版本；
Control 经 SecretDeliveryPort 直接交付 0600 文件。普通 REST/WS/日志/收据没有明文。
响应丢失先查原 intent/receipt；旧明文不可从摘要恢复，必要时显式授权重新发放并撤销旧版本。
文件写入与 receipt 需要恢复协议，不伪称跨文件原子；迟到 worker 不能覆盖新版本。

### HostRequest 未知操作

空操作为 INVALID_ARGUMENT，单个 future operation 候选为 CAPABILITY_NOT_SUPPORTED；
已知不支持也返回能力拒绝。已知+未知候选、多未知候选、错 wire type 拒绝且无下游副作用。
已知 oneof 使用标准 Protobuf last-one-wins；不承诺在 typed gRPC 解析后识别已经丢失的重复字段。
future 候选只来自约定 operation 编号区；普通 unknown metadata 不被误认为操作。
reference 校验器和 blob manifest 保持原样，target 证据单独登记。

## 3. 门禁与实施放行

基线 descriptor：`build/logs/capability-expansion/baseline-descriptor.pb`，
来源为开工时 `0496b90` 加已有重构/计划的实际 proto，不使用新 descriptor 自比。

已实际通过：

- 锁定环境全部 proto descriptor 生成；与起始 descriptor 的兼容工具检查，exit 0。
- `cmake --preset full-locked -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`，exit 0。
- `kiarabot_proto` 的 C++/gRPC 生成和编译、`kiarabot_target_roundtrip` 重链，exit 0。
- Node 22.14.0 下 AsyncAPI、OpenAPI、target API schema 门禁，exit 0；
  包括 67 条 HTTP operation、WS 36 项与 Host/bootstrap 38 项边界检查。
- 原 registry reference/target 已有状态逐项比较：改变 0；所有新声明仍未实现。
- reference blob 检查和边界/架构自测 7/11 项通过；它们不替代新增功能运行验收。

最终门禁已收口：118 个 typed RPC 描述/权限、376 条 Python → C++ → Python 消息、25 组 wire golden、
26 项模型流语义正负例通过；`check_contracts.sh` 含 legacy reader 与真实起始 descriptor 兼容，exit 0。
新增 target feature 声明/禁止提前发布的 5 项拒绝测试通过；schema/API/enum/status/semantic/evidence
门禁均通过。初轮 API 检查曾失败，修正 owner 权限映射及边界后最终固定 Node22 复跑通过，
首轮失败记录保留，不将其改写成首轮全绿。

实际环境、命令、退出码、日志和源码 SHA 记录在
`build/logs/capability-expansion/freeze-final-summary.json`；跨语言往返命令单列
`freeze-roundtrip-result.json`（exit 0，82.062s）。本机日志/生成物不提交。
主代理已放行 WS-02 owner journal、STR-02/03/05 Provider/纯端口、HERR-02 和 BOOT-03 交付模块；
后续 RPC/Bridge/SDK/Panel 和完整部署链须另行实现验收，冻结不等于生产功能完成。

## 4. 文档影响

已更新协议 README、契约/运行语义、Core/SDK、Control/Panel 中直接描述新接口的章节，
新方法与字段状态登记、schema 测试和协议生成清单同步。
后续 Provider、Host SQLite、PG journal、运行配置、秘密交付与部署行为落地时，
继续更新所属 `doc/05`、`doc/08`、`doc/11` 及实际受影响的模块文档/运维验证。
与本次新增行为无关的模块不重写；`doc/18` 和受保护清单只读，基线差异只报告。

## 5. v1.1 Host gate 身份契约补充（2026-11-04，冻结通过）

实际身份链核对发现：既有平台 enrollment 只覆盖 Runtime/Scheduler/Model，
`CheckAuthorization` 需要受限委派 proof，不能拿 Host mTLS 冒充此 proof。
因此追加窄范围 `IdentityService.CheckHostGateAuthorization` 及两种消息；
唯一来源仍为 `identity.proto`，新方法状态保持 `not_implemented`，生产实现另行验收。

调用者仅为当前已注册 Runtime 的实际 mTLS 身份，且需其自身 Principal 上显式的
`identity.authorization.check`。请求只有 context、精确 Host ResourceRef 和实际入站 Host TLS
叶指纹，不能传任意 principal、permission、method 或代替原有 subject proof。
Identity 只判定固定 `RuntimeService.SetHostAdmissionGate`，按当前 Host 注册/证书、
显式 `RESOURCE_HOST(host_id)` binding 和该 Host 的 `host.execute` 授权检查；
响应仅含 decision 与当前 Host principal/credential 版本。Runtime 仍需核验真实 TLS、
当前 HostBridge 注册/Fence、维护意图和 gate 版本，不能缓存决定跨越撤权。
HostId 与 PrincipalId 分别传入，复用现有 typed binding，不新增 DDL。
原有 CheckAuthorization 委派语义与 Scheduler 白名单保持。

proto、registry/spec、两组 wire golden、专项 shape/policy oracle、协议 README、
doc/01/运行语义及直接模块文档已同步。实际验证：独立 Python/descriptor 生成、
对冻结 v1 和原始开工 descriptor 的兼容检查、完整 contracts 含 legacy reader 均 exit 0；
共享 C++/gRPC 生成和 roundtrip 目标重链成功后，主代理实际跨语言验证 **119 RPC、
378 消息、27 golden**，连同原 26 项流语义及 Host gate oracle 全部通过。
新增 HTTP/WS 契约未变化，无需据此重复 Node schema 验证。

日志：`build/logs/capability-expansion/identity-v11-contracts-result.json`、
`identity-v11-contracts.log`、`identity-v11-original-baseline-compat.log`、
`identity-v11-roundtrip.log`；精确 proto SHA-256 为
`4aa85a1897eeaf5365863635c70638c0d3e4c75cd1c97bff28a5ac848c9a651b`。
上述通过仅放行生产身份链实现，不是 Host gate 已可用或维护全链完成证明。
