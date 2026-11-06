# Identity 身份与访问模块设计

本文描述 `kiarabot-identity` 身份与访问服务：人类会话、主体与凭据、固定角色与精确资源授权、短期委托、在线授权判定、失效流、验签材料以及审计/日志投影。跨进程契约唯一来源是 `proto/kiarabot/v1/identity.proto` 与 `proto/kiarabot/v1/policy.proto`；实现位于 `core/src/runtime/platform/identity_rpc.cpp`、`core/src/application/target/identity_*.cpp`、`core/include/kiarabot/core/{domain,application,ports}/target/`，PostgreSQL 适配在 `infrastructure/postgres/`，密码/令牌/委托密码学在 `infrastructure/identity_crypto/`。模块在六服务拓扑中的位置与信任链见[架构设计](../architecture.md) §3、§6。

## 1 模块定位与职责

Identity 是身份的**唯一权威**，负责以下事实及其本地事务：

- 人类主体与服务/适配器/插件实例主体的登记、状态和版本；密码、WorkloadBootstrap、证书绑定三类凭据版本与撤销；
- 浏览器登录会话的创建、刷新轮换、当前校验与撤销；会话是浏览器唯一凭据，不转发给其他服务；
- 固定角色（`admin`/`operator`/`viewer`）与**精确资源** assignment 的组合授权；授权版本（authorization_version）与失效记录；
- Ed25519 短期委托的签发（`IssueDelegation`）与交换（`ExchangeDelegation`），绑定实际调用服务、audience、操作/资源子集与父委托链；
- 在线授权判定 `CheckAuthorization`，以及只允许 Runtime 在固定方法上使用的窄检查 `CheckHostGateAuthorization`；
- 有界失效流 `WatchInvalidations`、授权快照 `GetAuthorizationSnapshot` 与公开验签材料 `GetVerificationKeys`；
- 只读审计/日志投影 `ListAudit`、`ListLogs`、`ListLogChanges`，以及人类主体目录 `ListPrincipals`；
- 离线一次性引导（初始管理员、Control 与平台 peer 登记）由独立命令 `kiarabot-identity-bootstrap` 提交，不是 RPC。

Identity **不**决定群动作预算、业务可执行性、插件调度或平台副作用；不保存其他服务的业务表；不提供自定义角色、完整用户管理、联合登录或外部 IAM；不保存平台私钥与跨服务秘密。

## 2 边界与依赖

| 调用方 | 允许用途 | 约束 |
| --- | --- | --- |
| Control | 浏览器 Login/Refresh/Validate/GetPrincipal/ListSessions/RevokeSession；管理员主体、凭据、assignment、主体目录、审计/日志 | 每次 REST 在线校验当前会话；服务身份不替代最终用户授权 |
| Runtime / Scheduler / Model / Host | `CheckAuthorization`、委托交换、失效订阅、验签材料、Host gate 窄检查 | 必须是当前 mTLS 平台 peer 且只检查自身方法/audience，不接受跨接收方管理员证明 |
| 部署/运维 | `kiarabot-identity-bootstrap` 离线引导 | 一次性，写入持久 marker 后禁用，不绑定 RPC |

依赖：私有 PostgreSQL 逻辑库（独立运行角色与迁移角色）；部署注入的密钥文件（token/request/receipt 对称密钥、Ed25519 委托公私钥、验签 catalog 声明）；受信 TLS 证书链与本机时钟声明（`trusted`、`maximum_known_skew_ms`）。Identity 不调用其他服务的业务接口，其他服务也不读 Identity 数据库；跨服务只通过 `proto/` 交换信息。

## 3 进程/组件结构

- 进程入口 `core/app/identity_main.cpp` → `platform::run_service()`（`core/src/runtime/platform/service_process.cpp`）：屏蔽停止信号、读配置、装配、监听、优雅停机；`--help`/`--config`/证书错误分别按 0/2/1 退出。
- 组合根 `core/app/identity_composition.cpp`：加载密钥与 TLS 期限、打开 PG、构造应用、注册 `IdentityService` 到内部 mTLS owner 监听；每秒探测数据库/引导 marker/时钟，并做有界失效记录清理。
- 传输 `core/src/runtime/platform/identity_rpc.cpp`：实现 `IdentityService` 全部 25 个 RPC，负责 peer 证书认证、权限/期限检查、请求形状校验与稳定错误映射；`authorization_method_policy.cpp` 解析 method options 为受信策略。
- 应用层（均为纯内部类型，不依赖 protobuf/SQL）：`identity_security_application`（会话）、`identity_principal_application`（主体/凭据）、`identity_admin_application`（引导与 peer 注册）、`identity_delegation_application`、`identity_exchange_application`、`identity_assignment_application`、`identity_authorization_application`、`identity_invalidation_application`、`identity_verification_keys_application`、`identity_audit_application`、`identity_log_feed_application`、`identity_principal_directory_application`。
- 边界组件：`core/app/identity_security_clock.cpp`（UTC+单调受信时钟与自身 TLS 调用期限）、`core/src/runtime/platform/peer_certificate.cpp`（DER 叶证书/链提取）、`core/src/runtime/platform/service_config.cpp`（严格 JSON schema v1）。
- 基础设施：`infrastructure/postgres/src/identity_security.cpp`、`identity_sessions.cpp`、`identity_assignments.cpp`、`identity_invalidations.cpp`、`identity_receipts.cpp`、`identity_access.hpp`；`infrastructure/identity_crypto/src/{password_crypto,token_crypto,receipt_crypto,delegation_crypto,peer_certificate,verification_keys,key_files}.cpp`。

## 4 数据模型与持久化

Identity 私有 PostgreSQL 库由 `kiarabot-pg-initialize` 以空库初始化；常驻进程只校验 schema 版本，不做 DDL/自动迁移。schema 版本见 `infrastructure/postgres/schema/v1.hpp`（Identity=18），关系/约束/索引/descriptor 数量由 `schema/manifest.hpp` 固定（141 个关系）。

| 关系组 | 文件 | 内容 |
| --- | --- | --- |
| 主体/会话/授权 | `schema/services.hpp` | `principals`（类型、binding、版本、生命周期）、`sessions`、`grants`、`grant_scopes`、`authorizations`（授权版本 + invalidation_dirty） |
| 凭据安全 | `schema/identity_security.hpp` | `credential_heads`/`credential_versions`/`credential_pending`/`credential_passwords`/`credential_bootstraps`/`credential_certificates`/`credential_audiences`、`login_identities`、`session_security`、`consumed_refresh`、`secret_receipts`、`administrator_bootstrap` |
| assignment | `schema/identity_security.hpp` | `exact_assignments`（state/version/撤销时间）、`exact_roles`、`exact_permissions`（封闭 Permission 1–50） |
| 失效流 | `schema/identity_security.hpp` | `identity_invalidation_state`（epoch+next_sequence）、`identity_invalidations`、`identity_invalidation_scopes` |
| 命令/审计/交接 | `schema/common.hpp` | `command_receipts`、`audit_entries`、`operations`、`operation_participants`、`outbox`、`inbox`、`replay_results` 与 `result_*` 快照 |
| 观测 journal | `schema/observation_feed.hpp` | `observation_feed_head`、`observation_feed_changes`（logs 家族，600 秒/16384 条有界） |

持久化原则：只保存验证材料或密文引用，不保存明文密码、token 或可重放秘密；凭据状态为 `Pending/Active/Revoked`，已签发凭据必须有有限 `expires_at`，`Pending` 不伪造到期时间；令牌以 HMAC-SHA256 keyed verifier（`TokenVerifier`）保存，密码用 libsodium Argon2id（2 次、64 MiB）保存编码材料；证书绑定保存 32 字节指纹与登记的 SAN。写操作使用资源版本 CAS/唯一约束，登录/刷新/凭据交付的幂等重放依赖加密 `secret_receipts`（保留 24 小时）。主体/凭据/会话 ID 均为规范小写 UUIDv4 文本。

## 5 关键流程/状态机

- **Login**：校验登录名映射与密码 verifier；未知用户名也做同代价假验算；成功后在单一事务写会话、access/refresh verifier、审计与回执，提交后才返回 token 对。access 15 分钟、refresh 30 天；同幂等键重放走加密收据。
- **RefreshSession / ValidateSession / GetPrincipal**：刷新在 `session_id` 内轮换次数与 verifier，旧 refresh 进入 `consumed_refresh` 防重放；校验每次都读当前权威状态，撤销/过期/凭据失效返回 `UNAUTHENTICATED`，不允许用缓存或旧收据伪装成功。
- **主体与凭据生命周期**：`RegisterPrincipal` 同事务创建（通常 Disabled 的）主体与 PENDING 凭据引用并记录 allowed_services；首次成功 `RotateCredential` 激活主体并交付一次性秘密（WorkloadBootstrap 为 opaque token，有效期 24 小时；Password 由服务端生成并重新派生 verifier）；`RevokeCredential` 保留历史版本，撤销后该凭据的登录/校验门禁必须失败。`GetCredentialStatus` 只返回状态/版本，不返回秘密。
- **委托签发/交换**：`IssueDelegation` 在线校验来源会话、主体与精确权限后签发 Ed25519 JWT（正文期限 ≤56 秒），绑定 audience、主体、资源/权限子集、actor 链与客户端证书；`ExchangeDelegation` 从父证明派生收窄的子证明，业务转交最多 2 跳；`instance_id` 形式的父委托只允许管理员会话对已登记插件实例签发。两者无幂等键，重试会重新授权并可能得到新证明。
- **在线授权检查**：`CheckAuthorization` 在 ≤1 秒窗口内读取两次当前权威状态（数据库 UTC + 可信单调时钟），窗口内主体/授权版本/判定变化即 `UNAUTHENTICATED`；返回值包含 allowed、authorization_version、checked_at，拒绝以 `PERMISSION_DENIED` 表达。该用例无写入、无缓存、无秘密收据。
- **Assignment 与失效**：`SetAssignment`/`RevokeAssignment` 要求预期版本，角色与显式权限二选一，资源 scope 为精确集合；同一物理事务推进主体授权版本、写失效记录与 scope 行、审计与回执。撤销保留分配 ID 作为永久墓碑，不能凭新幂等键复活。
- **失效流**：`WatchInvalidations` 从游标 `sequence+1` 顺序续发；空游标/epoch 不一致/超出重放窗返回 `resync_required=true` 与当前 watermark，客户端必须取 `GetAuthorizationSnapshot` 后只应用快照之后的记录；重放窗为最近 10 分钟且最多 4096 条，清理只影响续发能力。
- **Host gate 窄检查**：`CheckHostGateAuthorization` 只接受当前 Runtime 平台 peer，按实际入站 Host 叶证书指纹解析 Active Host principal，要求其 `RESOURCE_HOST` binding 与请求一致且持有显式 `host.execute`；返回一次性判定与 host credential/principal 版本，允许结果不可缓存。

## 6 接口与契约

契约源：`proto/kiarabot/v1/identity.proto`（`IdentityService` 25 个 RPC）、`proto/kiarabot/v1/policy.proto`（封闭 `Permission` 1–50 与 `required_permission`/`requires_idempotency`/`default_deadline_ms`/`max_deadline_ms`/`requires_online_authorization`/`scope_kinds` 方法选项）、`proto/kiarabot/v1/platform.proto`（`RequestContext`/`ResourceRef`/`StreamCursor`/`Operation`/`FailureContext`）与 `proto/kiarabot/v1/common.proto`（`Error`/`ErrorCode`）。target 可用性以 `proto/tests/target_spec.json` 与 `proto/tests/contract_status.json` 的 `execution_scopes.target` 为准；IdentityService 的 25 个方法当前均登记 `implemented`，但 `.proto` 内联 `CONTRACT_STATUS` 仍保留 reference 历史注释，引用状态时必须区分两者。

| 方法族 | 权限 | 期限/幂等 |
| --- | --- | --- |
| Login、RefreshSession | `identity.session.create` | 5s/30s，必需幂等 |
| ValidateSession、GetPrincipal、ListSessions | `identity.session.read` | 2s/5s，无幂等 |
| RevokeSession | `identity.session.revoke` | 5s/30s，必需幂等 |
| RegisterPrincipal、RotateCredential、RevokeCredential | `identity.principal.manage` | 5s/30s，必需幂等 |
| GetPrincipalStatus、GetCredentialStatus、ListPrincipals | `identity.principal.read` | 2s/5s，无幂等 |
| IssueDelegation、ExchangeDelegation | `identity.delegation.issue` | 5s/30s，无幂等（重试重新授权） |
| CheckAuthorization、CheckHostGateAuthorization | `identity.authorization.check` | 500ms/1s，在线 |
| ListAssignments | `identity.assignment.read` | 2s/5s |
| SetAssignment、RevokeAssignment | `identity.assignment.manage` | 5s/30s，必需幂等 |
| WatchInvalidations、GetAuthorizationSnapshot | `identity.invalidation.read` | 2s/5s，服务端流 |
| GetVerificationKeys | `identity.keys.read` | 2s/5s |
| ListAudit、ListLogs、ListLogChanges | `audit.read` / `log.read` | 2s/5s（变更流 500ms/500ms），scope 只允许 `RESOURCE_CATALOG` |

## 7 权限与安全

- **通道身份**：生产服务间使用 mTLS；peer 认证从实际传输证书提取并校验登记指纹/SAN/链，不用 hostname、IP 或请求字段确定身份。请求正文中的 actor/role 永不作为可信声明。
- **方法授权**：每个 RPC 的必需权限来自 `policy.proto` 方法选项；缺失/未知策略 fail-closed。固定角色只按显式 allowlist 生效（`core/include/kiarabot/core/domain/target/identity_policy.hpp`），`message.read`、`action.inspect`、`log.read`、`audit.read`、`health.read` 等敏感权限只能显式授予；空 scope 不代表全局，未知权限名直接拒绝。
- **委托**：固定 Ed25519/JWS，不自动降级算法，不接受 `none` 或 token 指定键地址；检查 iss/aud/sub/exp/iat/nbf、主体与资源范围、actor 链与 mTLS 证书绑定。证明正文 ≤56 秒，加节点时钟差与验签容差总预算 ≤60 秒；超过已知误差界视为不可信。
- **秘密**：密钥只从 owner-only 文件或部署注入读取，不写入 JSON/argv/数据库明文/日志/outbox；响应秘密只在提交后返回一次（`CredentialDelivery`），丢失按同键加密收据恢复，不通过状态查询找回秘密。
- **撤销语义**：撤销不依赖后续清理行即生效；REST 每次访问由 Control 在线校验，WS 在既有 1 秒预算内关闭；失效流只是有界加速器，撤销与已发生副作用不是全局原子。

## 8 错误/失败/恢复语义

- 稳定错误码：`INVALID_ARGUMENT`、`UNAUTHENTICATED`、`PERMISSION_DENIED`、`NOT_FOUND`（不存在与不可见统一，避免枚举）、`CONFLICT`（版本/幂等内容冲突）、`RATE_LIMITED`、`TIMEOUT`、`UPSTREAM_UNAVAILABLE`、`CAPABILITY_NOT_SUPPORTED`、`INTERNAL`；自由文本不能替代稳定码。
- `Failure` 同时携带是否受理（`NotAccepted/Accepted/Unknown`）与恢复动作；业务失败保留 request_id/trace_id，拒绝不伪装成成功或空结果。
- **幂等与重放**：必需幂等的写命令（登录/刷新/主体/凭据/会话撤销/assignment 写）先重新鉴权，再按稳定 key 返回原成功快照；同键异内容 `CONFLICT`。未知提交结果按原身份查询恢复，不用新密钥重试或返回未提交秘密。
- **依赖故障**：数据库不可用、时钟不可信、证书/引导 marker 异常时进程可存活但 `ready=false`，每个请求的 peer 认证/应用门禁返回依赖失败，不降级为允许。密钥未知、签名无效、audience/接收方不符、过期、时钟不可靠一律拒绝。
- **撤销与过期**：`Revoked` 优先于 `Expired`；`created_at` 为权威首次创建时间，刷新/撤销不改写；会话状态以同响应 `checked_at` 判定。资源恢复不复活已撤销凭据、旧 token 或已取消的授权。

## 9 可观测性与运维

- **就绪**：`ServiceReadinessPort` 报告 `BusinessImplementation/DatabaseSchema/Credentials` 等依赖；只有离线引导已初始化 schema、写会话可用、密钥材料已加载且时钟健康时才 `ready`。`probe_listen` 提供进程存活/就绪探测，监听本身不代表 ready。
- **日志**：结构化 stderr 行只含 `operation/outcome/error_code/request_id/trace_id` 等标识，不含 token、密钥、密码或原始错误文本；`KIARABOT_CRASH_DIAGNOSTICS` 可输出不含秘密的 backtrace。
- **审计/日志查询**：`ListAudit` 返回业务审计事实（actor/resource/request/trace/method/rejection/authorization_version/时间），`ListLogs` 是同一审计事实的脱敏 `request.recorded|request.rejected` 投影，二者权限分离；`ListLogChanges` 是 600 秒有界 owner journal，游标绑定 owner/epoch/scope/sequence，超窗必须 resync。
- **配置与密钥**：单进程配置为严格 JSON schema v1（`service_config.hpp`）：`probe_listen`、`grpc_listen`、`tls`、`database`、`identity_security`（token/request/receipt key、delegation key、issuer、clock、verification）。私钥文件必须 owner-only；证书有效期在启动前校验。
- **故障处置**：数据库初始化/迁移属于显式运维命令，常驻进程无 DDL 权限；失效流/收据/审计仅接近上限时通过运维查询与清理批次处理，清理失败不撤销已提交撤销。

## 10 测试与验收证据

- 单元/契约（`core/tests/architecture/`）：`identity_contract_test.cpp`、`identity_application_contract_test.cpp`、`identity_rules_test.cpp`、`identity_grant_projection_test.cpp`、`identity_assignment_state_test.cpp`、`identity_assignment_repository_contract_test.cpp`、`identity_assignment_fingerprint_contract_test.cpp`、`identity_registration_snapshot_test.cpp`、`identity_assignment_snapshot_test.cpp`、`identity_{security,principal,assignment,authorization,invalidation,exchange,admin,verification_keys}_application_test.cpp`、`identity_verification_keys_contract_test.cpp`、`session_control_peer_contract_test.cpp`、`persistence_contract_test.cpp`。
- 传输/密码学/离线工具（`core/tests/platform/`）：`identity_rpc_test.cpp` + `identity_rpc_test.py`（真实 gRPC/mTLS 边界与 fail-first 用例）、`identity_security_clock_test.cpp`、`authorization_method_policy_test.cpp`、`peer_certificate_test.py`、`identity_bootstrap_test.py`、`identity_credential_daemon_test.py`。
- 端到端（CTest 名 / 脚本）：`p04_identity_rpc`、`p04_identity_daemon_*`、`p04_identity_application`、`p04_identity_log_feed_e2e`（`tests/architecture/p04_identity_log_feed_e2e.py`）、`p05_runtime_identity_e2e`（`core/tests/platform/runtime_identity_e2e_test.py`：真 PG16.4 + 双进程 mTLS + 委托/CheckAuthorization）、`p05_runtime_catalog_e2e`、`p13h3_joint_delete_production_e2e`（`tests/architecture/p13h3_joint_delete_production_e2e.py`）、`p14_native_single_host`（`tests/architecture/p14_native_single_host.py`）、`ws03_operation_observation_e2e`。
- 覆盖要求：正常、拒绝、超时/期限、依赖故障、撤销传播、幂等重放、旧代次/旧收据拒绝；真实验收结论以 CTest 结果与[架构设计](../architecture.md) §7、§11 记录的阶段证据为准，测试注册本身不等于通过。

## 11 已知限制与后续

- 仅支持固定角色 + 精确资源授予；自定义角色、完整用户/密码管理、SSO/外部 IAM 不在范围，`admin` 也不自动获得 `message.read`、`audit.read` 等敏感授权。
- 内部委托证明不是通用 OAuth 授权服务器；短证明没有独立吊销列表，靠到期与在线复核收敛，撤销与外部副作用不是全局原子。
- `CheckHostGateAuthorization` 只为 Runtime 的 `SetHostAdmissionGate` 一个下游方法服务，没有通用跨接收方管理员检查路径。
- 生产 CA/KMS、证书轮换编排、备份离站未在仓库内实现：私钥由部署注入文件供给，轮换与托管属环境项；跨主机/断网演练仍延期。
- `.proto` 内联 `CONTRACT_STATUS` 保留 reference/历史语境，target 可用性必须查 `proto/tests/target_spec.json` 与 `execution_scopes.target`，避免把历史注释当成当前状态。
- Identity 不可用时新签发/新判定 fail-closed；已有未过期短证明能否继续，由接收方按在线复核和 60 秒上界自行决定，文档不能承诺更长的离线可用性。
