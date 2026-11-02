# P04-A Identity 主体/凭据管理 RPC + readiness 实施记录

## 范围与文档影响

目标：补齐 Identity `RegisterPrincipal`、`RotateCredential`、`RevokeCredential`、
`GetPrincipalStatus`、`GetCredentialStatus` 五个目标 RPC 的纯应用、Identity 私有 PG 接线、
gRPC 边界转换与生产组合根装配，并把 Identity readiness 从固定 false 改为真实判定。

允许范围：`core/{include,src}/application/target/`、`core/include/.../domain/target/`、
`core/include/.../ports/target/`、`core/src/runtime/platform/identity_rpc.cpp`、
`core/app/identity_composition.cpp`、`core/tests/{architecture,platform}/`、
`infrastructure/{postgres,identity_crypto}/`、`cmake/identity-security.cmake`、
`control/api/tests/architecture/identity_daemon_http_test.py`、直接契约/模块文档与本记录。
未修改 `proto/kiarabot/v1/` 线协议字段、`doc/18`（只读基线）、其他服务内部实现与部署配置。

文档影响：更新 `doc/01-contracts.md`（target 执行范围表与 RPC 边界）、`doc/02-core.md`
（Identity RPC 边界与凭据语义）、`doc/06-control-api.md`、`doc/08-operations.md`
（凭据运维语义与 schema v11）、`doc/05-infrastructure.md`（schema v11/受众关系/唯一索引）、
`doc/21-service-contract-design.md`；`proto/tests/contract_status.json` 与
`proto/tests/target_spec.json` 按 target scope 翻转五个 RPC 及其消息/字段。
API 的 HTTP 操作面（`/principals` 等）仍为 `not_implemented`，本次只完成 gRPC/Identity daemon 面。

## 实现摘要

### 1. 领域与纯应用

- `PendingCredentialMaterial` 增加 `audiences`：注册时持久化 `allowed_services`，尚未发放的
  Pending 凭据无 verifier/expiry；`valid_pending_audiences` 强制 ≤16、去重、禁 Control。
- 新增域规则 `valid_principal_binding`：platform_service→Principal 自绑定、adapter→Adapter、
  plugin→Instance；human 只允许可信离线 bootstrap，公开注册拒绝。
- 新增内部端口 `IdentityPrincipalFingerprintPort`：RegisterPrincipal 规范指纹 +
  RevokeCredential（域 + credential id + expected version）指纹；identity_crypto 用
  EVP SHA-256 与长度前缀编码实现，均在 UoW 外调用。
- 新增 `IdentityPrincipalApplication`（`core/{include,src}/application/target/identity_principal_application.*`）：
  - Register：同一 Identity UoW 创建 Disabled Principal 与 Pending WorkloadBootstrap 凭据，
    普通成功收据保存完整 `RegisteredPrincipalSnapshot`；同幂等键重放返回原快照，同键异内容
    CONFLICT。
  - Rotate：支持 Pending/Active WorkloadBootstrap（opaque token 24h 信任策略）与
    Pending/Active human Password（服务端一次性新密码 + `PasswordCryptoPort::derive`，365d
    信任策略）；CertificateBinding 明确 kind 级 `CAPABILITY_NOT_SUPPORTED`。首次成功轮换在
    同事务把 Disabled principal 激活为 Active（principal version+1 CAS），其余非 live 状态
    回滚并返回 CONFLICT/READ_CURRENT。交付密文收据 scope=稳定调用者+credential+幂等键，
    expected_version 进入 keyed request commitment；重放先校验当前授权与当前凭据/轮次。
  - Revoke：human 与 workload 凭据都支持；撤销保留历史版本与证据，撤销后 Login/Validate
    的当前凭据门禁必须失败；普通成功收据 + 常量域指纹（含 expected_version）保证同键异版本
    CONFLICT。
  - 状态查询：只返回当前无秘密投影；未授权与不存在统一 `NOT_FOUND`（register 的
    caller-provided binding 无目标存在性可泄露，保留 PERMISSION_DENIED）。
  - 所有 8 处 `final_check` 使用 `requires_online_authorization=true` 的 1 秒当前授权
    checkpoint；超窗返回 TIMEOUT 且不提交、不返回 secret。
- 新增纯测试 `identity_principal_application_test`：覆盖注册/重放/冲突、Disabled→Active
  生命周期、kind 级轮换、human 密码轮换/重放/撤销、撤销后认证 gate 失败、NOT_FOUND
  收口、一 binding 一 Principal、卡顿拒绝、Unknown commit、审计/收据失败回滚、读方法零 commit。
- 新增 `principal_fingerprint_test`：真实 OpenSSL SHA-256 golden 值与域隔离/排序等价/非法输入。

### 2. Identity 私有 PG 与 schema v11

- `credential_audiences` 增加 `material` 列并泛化 FK 到
  `credential_versions(id,version,material)`，使 Pending 与已发放 WorkloadBootstrap 都能
  精确保存/重载受众；`credential_audiences` 关系数不变。
- 新增 Identity 私有唯一索引 `principals_binding_unique`
  （partial unique `(binding_kind,binding_id)` WHERE both NOT NULL），强制 v1「一个精确
  binding 只对应一个 Principal」，与请求幂等键无关。
- schema 10→11，实测 PG16.4 空库（`kDescriptorQuery` 与 `pg_class` 计数）：
  `descriptor=2f1e003acaf28de3990f3dc8ebe49cf5`、`objects=114`；已写回
  `infrastructure/postgres/schema/manifest.hpp` 与 `schema/v1.hpp`，数据库测试版本断言同步。
- 该变更是 P04 计划内的 Identity 私有库 schema 细节（仅新空库初始化，不涉及跨服务存储模型、
  不新增服务/模块边界）。P04-B rebase 后将在此基线上再 bump 到 v12 / 117 relations。
- `identity_application_test.cpp` 增加真实 PG/TLS 链：register→Pending audiences 往返→
  rotate(workload)→replay→revoke→current-credential gate 失败；human 密码 rotate→新密码登录
  成功/旧密码失败→revoke→Login 失败；pending 收据重放不重复。

### 3. RPC、组合根与 readiness

- `identity_rpc.cpp` 在 `IdentityRpcOptions{.principal=&app}` 存在时适配五个方法：只做
  protobuf 转换、输入形状/预算校验与稳定 RpcFailure 映射；未注入时保持 typed
  CAPABILITY_NOT_SUPPORTED。新增 PrincipalRecord（含非 human kind/Disabled 状态）与
  CredentialStatus/CredentialDelivery 投影，非法应用结果保守 INTERNAL/Unknown。
- `identity_composition.cpp` 构造 `IdentityPrincipalApplication`（注入真实 crypto/PG 端口），
  并把 principal 能力注入 RPC；readiness 现为「业务可用 && DB writer 可用 && 密钥材料已装载
  && bootstrap 已 Consumed && 可信 clock」，`maintain()` 每秒重新观测 schema/bootstrap/clock；
  组合根不再 never-upgrade。
- `IdentityRpcOptions` 与组合根只追加自身字段，未重排其他能力。
- 扩展 RPC 测试：register/rotate/revoke/status 成功投影、失效输入、typed 失败、异常结果
  INTERNAL/Unknown 拒绝；真实 mTLS 双服务夹具（六方法服务无 principal 能力、第二服务带
  principal 能力）。
- daemon HTTP 集成测试就绪断言随行为更新（bootstrap 后 ready=true）。

## 实际运行检查（本机，固定 full-locked 环境）

所有命令在 worktree `codex/p04-cred` 执行，构建目录 `build/full-locked`：

- configure/build：
  `VCPKG_ROOT=... cmake --preset full-locked -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_INSTALLED_DIR=... -DPython3_EXECUTABLE=... -DKIARABOT_TARGET_PYTHON=... -DKIARABOT_TEST_OPENSSL=... -DKIARABOT_TEST_PG_BIN=...`
  然后 `cmake --build --preset full-locked --parallel 3`：成功。
- 真实 PG16.4/OpenSSL3.5 schema 测量（P03 无 Docker 临时集群）：Identity 空库
  `descriptor=2f1e003acaf28de3990f3dc8ebe49cf5`、`objects=114`。
- `ctest --preset full-locked -R 'p03_postgres|p04_identity' --output-on-failure`：
  **27/27 passed**（含 `p03_postgres_database`、`p04_identity_application`、
  `p04_identity_bootstrap`、`p04_identity_daemon_*`、`p04_identity_rpc` 等真实 PG/TLS 用例）。
- `KIARABOT_API_NODE_MODULES=/Users/betta/Workspace/KiaraBot/scripts/ci/node_modules ctest --preset full-locked --output-on-failure`：
  **96/96 passed**（总耗时约 343s）。`p01_target_api` 需要共享 `scripts/ci/node_modules`，
  通过环境变量指向主仓库已安装依赖后通过。
- `python3 proto/tests/check_implementation_status.py`：通过（target scope 新增 12 消息 +
  33 字段 override/evidence，历史 reference scope 仍 not_implemented）。
- `clang-format 16.0.6 --dry-run --Werror`（`build/format-venv/bin/clang-format`）：
  全部改动 C++ 文件通过；`git diff --check` 通过。
- 纯应用/指纹测试也在系统 clang（非固定环境）独立编译运行通过，用于快速回归：
  `identity_principal_application_test`、`principal_fingerprint_test` 均退出 0。

## 未运行 / 未完成项

- 未运行 clang-tidy/clang-analyzer（本任务未要求；固定环境 tidy 可用性未验证）。
- 未运行远端 CI、Docker/Compose、生产部署/备份恢复演练（T11b 归属其他任务）。
- API/Control HTTP 五个操作仍为 `not_implemented`（本次只交付 Identity gRPC/daemon 面，
  未修改 `api/openapi.yaml` 操作状态）；真实 QQ/模型无关。
- `p01_target_api` 在本 worktree 直接 `ctest --preset full-locked` 下会因缺少
  `scripts/ci/node_modules` 报环境前置错误（非代码失败），需显式给
  `KIARABOT_API_NODE_MODULES` 指向已安装依赖。
- Identity descriptor/relation_count 以 P04-A 分支实测值为准；P04-B rebase 时需按
  Lead 的 11→12 / 117 计划再次重算，本分支不预设这一数字。

## 风险与交接

- 注册成功后 principal 为 Disabled；首轮 Rotate 成功才激活。当前没有独立 enable/disable
  RPC，若后续产品需要“只注册不激活”或“停用”管理，应作为后续能力补齐。
- human 密码轮换交付服务端生成的一次性新密码（`CredentialDelivery.new_secret`），
  调用方负责用其更新登录密码；旧密码在版本递增后立即失效。若产品要求客户端提交新密码，
  需扩展请求字段（公共契约变更，另立任务）。
- Register 的权限拒绝仍为 PERMISSION_DENIED（无可泄露目标资源）；资源类读写拒绝均收敛
  NOT_FOUND，符合 8a 裁定。若 Control/API 层需要区分 403/404，由边界按契约再映射。
- 与 P04-B 共享的 `identity_rpc.cpp/.hpp`、`identity_composition.cpp`、
  `contract_status.json` 仅追加本能力的 handler/option/evidence；集成时按 Lead 顺序
  rebase，冲突由 Lead 协调。
