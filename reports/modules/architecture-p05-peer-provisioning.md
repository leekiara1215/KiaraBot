# P05 follow-up：平台 peer 注册与 Runtime↔Identity 真实 mTLS/授权 e2e

## 范围与文档影响

目标：补齐 Runtime（及可复用 Scheduler/Model）作为 Identity 平台 peer 的离线注册/证书/assignment
供给路径，并用真实双进程 + 真实 PG16.4 + 真实 mTLS/委托跑通允许、撤销后拒绝、依赖故障
fail-closed 与 readiness。不新增 proto/API 字段；Runtime 方法仅补既有 `scope_kinds`
descriptor option（P04-B 已定义的 `policy.proto` field 51006）。

允许修改：`core/app`（bootstrap CLI、Runtime 组合根/服务边界）、`core/include|src/.../target`、
`core/tests/{architecture,platform}`、`infrastructure/postgres`（测试夹具）、`tests/architecture`、
`cmake`、`doc/02,08,21`、`proto/tests`（descriptor 兼容工具/夹具/target_spec 元数据镜像）、
`reports/modules`。未改 `proto/kiarabot/v1` 消息/字段/服务定义（仅方法 option 注解）、未改
`api/`、未改 `doc/18`、未引入生产部署自动化。

## 实现摘要

### 1. 离线平台 peer 供给（bootstrap CLI + admin application）

- `PlatformPeerEnrollment` 加入 `OfflineIdentityBootstrap`：service、principal、credential、
  certificate binding 与显式 assignments，随管理员/Control 在**同一 once-only 事务**写入
  principals、credential_versions、authorization watermark、exact_assignments 与审计。
- 校验：service 仅 Runtime/Scheduler/Model；principal/credential/assignment 与既有身份及彼此
  不重复；证书 subject==principal、platform_service==service、有效期覆盖当前时间；assignment
  必须显式 permissions（无 role）且 subject 匹配、集合合法。certificate 的 DER 链、CA、
  clientAuth EKU、实际 SAN/指纹/时间/服务绑定由成熟 `peer_certificate_crypto` 离线验证，
  pure application 只做形状/唯一性复核。
- Enrollment JSON 顶层新增可选 `platform_peers`（≤16，未知/重复键拒绝）；旧不含该键的
  enrollment 行为不变。错误稳定：形状/证书/assignment 非法 INVALID_ARGUMENT，重复引导
  CONFLICT；不打印秘密/路径正文。
- Runtime/Scheduler/Model 可复用同一 JSON 段；Runtime 需要
  `identity.authorization.check`（scope=自身 Principal）作为 CheckAuthorization 消费能力。

### 2. Runtime 方法 scope 元数据（既有 option，非新字段）

为无歧义 exact 资源方法补 `scope_kinds`：GetAccount/UpdateAccount/DisableAccount/
BindAdapter/UnbindAdapter→ACCOUNT；GetAdapter→ADAPTER；GetInstallation→INSTALLATION；
GetInstance/GetInstanceConfig/SetDesiredState/UpdateConfig→INSTANCE。Create/List/
GetResourceState/GetOperation 等仍不标注（空/动态 scope），保持 fail closed。`target_spec.json`
镜像 reviewed 标注，`target_contract_test.py` 校验 descriptor option 与 spec 一致。

### 3. descriptor 兼容工具修正

`kiarabot_descriptor_compatibility` 原先对“方法新增既有 custom option”误判为破坏性。现按
additive 规则比较：方法已知 option 字段须保持原值，新增已知字段允许；custom options（在
descriptor set 中表现为 unknown fields）按字段号做**值集合**比较——旧有 option 的值改变或
删除失败，旧不存在的 option 号新增成功。message/field/enum/service 变更规则不变。
`descriptor_compatibility_test.sh` 新增 `option_added` 正例与 `option_changed`/
`option_removed` 负例（共 2 正例 + 7 负例）。

### 4. 真实授权链修复（e2e 发现）

- **委托 claims 编码不一致（P0）**：Identity 签名器的 uint64 claims 使用规范十进制字符串
  （`identity_crypto/delegation_crypto.cpp`），Runtime `decode_delegation_claims` 却要求
  JSON integer，导致真实委托被 UNAUTHENTICATED。已改为按规范十进制字符串解析（拒绝数字、
  前导零、越界），并更新单测 fixture。
- **Assignment 收据保留期非法（P0）**：`kAssignmentReceiptRetention` 为 24h，但域规则要求
  普通命令收据最少 7 天；真实 PG 下 RevokeAssignment 收据写入被 `check_receipt_write`
  拒绝为 INVALID_ARGUMENT（fake 收据测试未覆盖）。改为 7 天，真实 PG e2e 覆盖。
- Identity 签发/撤销与 Runtime 消费现走同一真实格式；`CheckAuthorization` 依赖的
  `scope_kinds` 注解缺失（runtime.proto 全文件为 0）已按第 2 节补齐最小无歧义集合。

### 5. 真实双进程 e2e（p05_runtime_identity_e2e）

测试由 `run_database_test.py` 提供 owned PG16.4 集群；`runtime_identity_e2e_test.py` 复用
`identity_bootstrap_test` 的证书/库/配置夹具，并扩展：
1. 生成 Control 与 Runtime clientAuth 证书；enrollment 增加 Runtime `platform_peers`、
   管理员 Account 级 account.read/manage assignment、Control 的 assignment/principal 管理
   grant。
2. 初始化 Identity 与 Runtime 私有库，启动真实 Identity daemon（mTLS、identity_security
   材料、真实 PG），按真实 bootstrap CLI 提交。
3. 启动真实 Runtime daemon（mTLS、私有 PG、`identity_endpoint`），轮询 probe 至 ready=true。
4. Control 客户端登录→IssueDelegation（aud=runtime、Account scope、account.read）→
   Runtime.GetAccount 经真实 CheckAuthorization 允许并读到 seed account。
5. RevokeAssignment 后同一委托被拒绝（PERMISSION_DENIED/UNAUTHENTICATED），且无法再签发
   新委托；撤销管理员密码凭据后 Login 失败。
6. 停止 Identity：Runtime readiness 在窗口内变 false，进程存活且命令不伪造成功；随后停止
   Runtime，用无 `identity_endpoint` 的配置启动，assert ready=false 且 fail-closed authorizer
   拒绝业务命令。
测试挂 CTest `p05_runtime_identity_e2e`，labels `p05;p04;integration;security;fault`。

## 实际运行检查

- 构建：`full-locked` configure/build 成功（固定 vcpkg/PG16.4/OpenSSL3.5 依赖，未重装）。
- `ctest --preset full-locked -R 'p04_target_codec|p05_runtime_identity_e2e'`：通过。
- `p05_runtime_identity_e2e`：**100% tests passed out of 1**（owned PG + 双进程 + mTLS）。
- `p04_identity_bootstrap`、`p04_identity_admin_application`、`p05_runtime_identity_client`、
  `p01_target_roundtrip`（含 target_spec/scope_kinds 校验）：通过。
- descriptor 兼容：`descriptor_compatibility_test.sh`（2 正例+7 负例）通过；以分支基点
  e1cf4ef descriptor 为 baseline 运行 `kiarabot_descriptor_compatibility` 通过。
- clang-format 16.0.6 dry-run、`git diff --check`：通过。
- 未运行：远端 CI、Docker/T11b、P06 bootstrap_credential 消费、P14 部署自动化；本机未跑
  完整全量 ctest（按 Lead 指定 subset + 新 e2e 验证）。

## 未完成 / 风险

- CreateAccount/ListAccounts/GetResourceState/GetOperation 等仍无单一 exact scope 或应用传空
  scope，未标 `scope_kinds`，真实授权链下保持 fail closed；补齐需应用先传稳定 exact scope，
  属后续 Runtime 能力。
- P06 `bootstrap_credential` 消费端（Bridge）仍 deferred；本任务不使用该路径。
- e2e 依赖本机 owned PG/OpenSSL 与 test-only fixture；不宣称生产部署/镜像编排完成。
- P05 report 中“Identity 真实授权链路未接线/无真实 e2e”遗留项由本任务关闭；调用
  `scope_kinds` 剩余方法清单与 P06 消费项仍保留为后续任务。
