# KiaraBot Protocol Contracts

T06a 使用同一 features 协商字段启用 host.storage.v1，T06b 启用 host.model.v1，承载已定义的 HostRequest/HostResponse 操作；未协商不发送，其他未实现 Host operation 显式拒绝。未改变 Host 字段编号或类型，详细限额/TTL/错误见 doc/03-plugin-sdk.md 和 doc/05-infrastructure.md。

T04a 兼容新增插件功能协商（supported_features=3 / enabled_features=5）、PluginActionRequest.request_id=2、PluginToCore.action_query=6/action_cancel=7、CoreToPlugin.action_response=7。action.result.v1 经双向确认后才能使用；旧端缺省不启用。查询可按认证调用方作用域中的 Action ID 或幂等键进行。响应 oneof 区分结果与操作错误，未知传输结果不能生成 FAILED。详细语义见 doc/01-contracts.md；服务接线由 T04b 实现。

T03a 兼容新增 CoreToPlugin.error=6，承载注册后的终止错误。注册拒绝仍在 register_result.error；客户端不能把流关闭当作 ACK。插件订阅使用现有 capabilities 中的事件能力名，与 doc/03-plugin-sdk.md 中权限映射一致，不新增私有跨语言类型。

本目录是跨语言契约的唯一来源。C++ Core、Python NoneBot2 适配器、C++ 插件和其他客户端不得复制这些消息定义。

## 依赖

- `protoc` 25.x 或更高版本；
- `grpc_cpp_plugin` 来自与 gRPC runtime 相同的 release 系列，并通过实际代码生成验证兼容性；它不一定提供可解析的 `--version` 输出；
- `protobuf` 运行库 25.0+、`grpc` 运行库 1.60.0+，由 CMake 配置阶段检查。

## 生成 C++ 代码

```bash
cmake -S . -B build -DKIARABOT_GENERATE_PROTO=ON
cmake --build build --target kiarabot_proto
```

该目标会调用 `protoc` 和 `grpc_cpp_plugin`，输出到 `build/generated/cpp/kiarabot/v1/`。如果本机没有这两个可执行文件，配置阶段应失败，而不是生成不完整的代码。

协议兼容性检查器由 `kiarabot_descriptor_compatibility` 构建目标提供。它读取当前 descriptor 和上一版本 descriptor，按文件名、语法/包名、消息字段编号及完整字段元数据、oneof、嵌套类型、扩展/保留范围、枚举值和 gRPC 方法签名与选项进行语义比较；不比较 descriptor 二进制序列化结果。新增字段和枚举值允许，删除或改变已发布定义会失败。

## 生成 Python 代码

```bash
python -m grpc_tools.protoc \
  -I proto \
  --python_out=build/generated/python \
  --grpc_python_out=build/generated/python \
  proto/kiarabot/v1/*.proto
```

生成文件只允许出现在 `build/generated/`，不直接编辑生成文件。

## 兼容性规则

- `kiarabot.v1` 只做向后兼容的新增；
- 已发布字段编号不得复用；
- 删除字段先标记 deprecated，再在下一个主版本移除；
- 枚举新增值必须由旧客户端安全忽略；
- 每次协议变更必须运行 descriptor diff 和跨语言 round-trip 测试。

### reserved / deprecated 可执行规则

- 删除字段/枚举值前先在 `.proto` 标记 `[deprecated = true]`，并在 `proto/tests/contract_status.json` 登记 `deprecated`、在 `doc/01-contracts.md` 状态表出现；只改注释或只改文档会被门禁拒绝。
- 真正删除时字段号/枚举号必须进入 `reserved`，且不得与现存字段号/枚举值重叠；编号永不复用。
- 枚举禁止 `allow_alias`；v1 全部 10 个枚举（含嵌套枚举）的名称与数值冻结在 `proto/tests/enum_baseline.json`。新增值允许，但必须在同一变更更新基线；改名、删除或重编号失败。
- 未实现的声明项不得被文档暗示为可用；状态以运行接线和测试证据为准，清单见 `proto/tests/contract_status.json` 与 `doc/01-contracts.md` 的“契约实现状态”。

v1 的稳定枚举词汇也由 REST 和 WebSocket 契约复用：`ErrorCode`、`HealthState`、`PluginState` 和 `ActionStatus` 在 `api/openapi.yaml` 与 `api/asyncapi.yaml` 中必须逐项保持一致；其余六个枚举（`EventDeliveryMode`、`ConversationRef.Type`、`MessageSegment.Type`、`GroupMemberChanged.ChangeType`、`AdapterEventAck.Status`、`ScheduledTaskState`）只属于线协议，但同样受数值基线保护。

## 契约实现状态与门禁

`proto/tests/contract_status.json` 登记每个 Protobuf 字段/oneof 载荷、枚举值、AsyncAPI 频道/事件和 OpenAPI 操作的 `implemented` / `not_implemented` / `deprecated` 状态。未实现项在 `.proto` 内有 `CONTRACT_STATUS` 注释、在 `api/*.yaml` 的 `x-contract-status` 中列出，并在 `doc/01-contracts.md` 状态表中说明。

本机可运行的契约门禁（无需 protoc）：

```bash
python3 proto/tests/check_enum_consistency.py
python3 proto/tests/check_implementation_status.py
python3 proto/tests/check_semantic_rules.py
```

需要 protoc / 构建产物的完整门禁：

```bash
./proto/tests/check_contracts.sh
```

`check_contracts.sh` 还会在具备 `protoc`、descriptor 兼容工具与生成后 Python 包的 CI 环境中，执行 `descriptor_compatibility_test.sh` 的 6 个负例与 `action_contract_test.py` 的旧 reader 兼容检查。缺少依赖时脚本以退出码 2 明确失败，不能当作通过。

## 优雅停机（drain）

`common.proto` 定义 `ShutdownNotice`，两个 Bridge 的 oneof 预留 `shutdown` 帧（`CoreToAdapter.shutdown=5`、`CoreToPlugin.shutdown=8`）。v1 Core 不发送该帧，状态为 `not_implemented`；客户端必须把未收到帧的流关闭视为未知结果，重连后重新注册并按幂等键恢复，见 `doc/01-runtime-semantics.md`。

## 插件 Host API

T06c 协商 host.tasks.v1 后启用 task_create/get/cancel；timer 事件只投递给任务所有者，任务状态值使用既有 ScheduledTaskState。没有新增线协议字段，运行时语义见 doc/01-runtime-semantics.md。

模型、命名空间存储和一次性任务使用 `plugins.proto` 中的 HostRequest/HostResponse，通过已注册 PluginBridge 传递。鉴权、幂等、时区、状态及错误语义见 `doc/01-contracts.md` 和 `doc/01-runtime-semantics.md`。

## 桥接认证

Connect 使用单个 bearer metadata，错误详情携带序列化 Error；身份绑定、失败及尚未实现行为见 [契约说明](../doc/01-contracts.md#桥接连接认证t01b)。T01b 未改变字段编号或 RPC 签名。

## 适配器持久化确认（T02a）

CoreToAdapter 新增 oneof event_ack=4，类型 AdapterEventAck；原字段编号保持不变。PERSISTED/DUPLICATE 才允许适配器移除本地待确认事件；REJECTED 必须检查稳定 Error.code/retryable。未知字段兼容旧客户端，但旧客户端必须升级支持 event_ack 后才能可靠清理缓存。注册 protocol_version 和事件 schema_version 为 kiarabot.v1。

配置能力 `plugin.config.v1` 使用 PluginConfig.delivery_id/apply_timeout_ms 与 PluginToCore.config_ack=8（PluginConfigAck）。旧客户端不协商则不下发；确认、失败、重连语义见 doc/03-plugin-sdk.md。

## P01 统一目标（未实现）

`platform.proto` 为目标公共上下文、资源、操作/未知结果、交接及 attempt 租约；原 AdapterBridge/PluginBridge 统一升级，不提供旧流兼容入口。目标注册后每帧须携当前正代次与 Runtime epoch；插件安装绑定和执行代次必填。CoreToPlugin 的 event 必须伴随同帧 delivery_attempt；ACK/Renew 都引用完整 attempt。目标方法表见 `tests/target_spec.json`，字段状态见 `tests/contract_status.json`。这些字段没有生产者，不代表旧实现已经通过目标校验。

`identity.proto`、`runtime.proto`、`host.proto`、`scheduler.proto`、`model.proto` 是五个权威服务的类型化方法；`policy.proto` 封闭 Permission 与每个 RPC 的权限/幂等/默认及最大 deadline/在线授权 descriptor options。HostBridge 由 Host 主动建立，复用类型化命令/观察；逻辑 HostService 不要求另建 Runtime 主动远程执行通道。ClaimEvent/DeliverEvent 仅可信 Runtime dispatcher，不向插件公开领取能力。

目标查询补全：ModelService.GetOperation复用OperationRef/Operation，必须在model owner校验已存储resource和当前授权；GetModelConfiguration按精确Model scope敏感在线查询，返回配置版本/secret_configured/observed_at，绝不返回秘密。权限复用resource.read/model.manage，默认2秒/最大5秒；二者target均not_implemented。方法计数由target_spec/descriptor动态推导，不能把UNIMPLEMENTED或reference闭环当成查询生产者。

Runtime typed读取补全为五类Get/List（Account/Installation/Instance/Adapter/Host）及GetInstanceConfig。Get使用该方法固定kind的exact ResourceRef；List必须给已登记RESOURCE_CATALOG UUID，仅发现当前调用方另获独立child grant的资源，不继承目录权限。新列表输入PageRequest，输出SnapshotInfo，禁止client传server快照字段。账号/插件/适配器/配置读权限是封闭枚举；全部新增类型和方法尚未生产实现。

## Identity 当前投影与凭据状态（目标未实现）

Identity 的 ValidateSession/GetPrincipal 返回当前主体的逐条 AssignmentRecord、authorization_version 和权威 UTC checked_at。每条绑定独立计算权限与 exact scope，不能把显示 roles/permissions 与全部 scope 做笛卡尔积授权；PrincipalRecord.roles/display_permissions 仅是当前绑定的显示并集。固定角色只用于 human；显式权限使用 role=UNSPECIFIED 加非空、去重的封闭 Permission 列表，两种授予依据不能混用。message.read/action.inspect 等敏感权限不由默认角色或创建资源自动授予。空 scope 不代表全局。

SetAssignment 输入 AssignmentSpec：创建不带 assignment_id/expected_version，Identity 生成 UUID/version=1；更新二者同时提供且版本为正，主体不可更换，并核验旧、新 scope 并集上的当前授予权限。目标尚未发布，原开发快照的 AssignmentRecord 请求已统一替换为 Spec，不提供旧请求桥接。

SessionRecord.created_at 是 Identity 持久保存的首次创建时间，刷新/撤销不改写。会话状态以同响应 checked_at 判定：revoked 优先 REVOKED，其次 checked_at >= expires_at 为 EXPIRED，否则 ACTIVE。Login/Refresh 的剩余 access TTL 为 max(0,floor(access_expires_at-checked_at))；Control 不用本地时间、token iat 或占位时间补齐。缺失权威字段、非当前绑定或授权依赖不可用必须拒绝，不能伪造空权限/成功。

RegisterPrincipal 同事务创建无秘密的 PENDING CredentialStatus 引用（credential_id/principal_id/binding/version/created_at/checked_at）；尚未发放的 PENDING 不得伪造 expires_at。GetCredentialStatus 以 identity.principal.read 在线读取同一记录，默认2秒/最多5秒；ACTIVE/EXPIRED 必须有 expires_at，REVOKED 可保留原到期。状态查询不返回秘密、摘要或可重放材料。初次实际发放仍用 RotateCredential 单次 CredentialDelivery，未知结果不得从状态查询恢复秘密。当前未实现，不启用生产路由。

### 外部连接授权目标补全（未实现）

platform.proto三类ConnectionAuthorization/RefreshConnectionAuthorization/Result与两Bridge的登记、proof、refresh帧定义短期外部连接授权。名义最多56秒、总预算60秒、每秒检查失效，单飞原子轮换；丢响应须关闭旧流、新bootstrap与连接代次。完整规则见doc/01-contracts及运行语义；内部JWT委托和P02 mTLS owner监听不变，P05/P06真实接线前仍not_implemented。授权材料禁止日志/公开投影/持久秘密收据，高风险业务仍在线核验，刷新不续投递租约、不改历史业务事实。浏览器REST/WS不使用这些proof，OpenAPI/AsyncAPI因此无schema变更。
