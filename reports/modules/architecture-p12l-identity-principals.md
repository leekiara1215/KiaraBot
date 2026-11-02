# P12l：Identity ListPrincipals 真实实现与 target 翻转

- 基线：main@`fc454bc`（P12k 已合入）；工作树 `codex/p12l`。
- 范围：Identity 只读人类主体目录（P12a-2 契约已声明，本任务只实现，不改 proto）。

## 实现

- 应用：`core/include/kiarabot/core/application/target/identity_principal_directory_application.hpp`
  + `core/src/application/target/identity_principal_directory_application.cpp`。
  - Control platform peer 由 RPC 边界以真实 TLS 证书认证，应用再经 `read_peer`/`check_peer`
    复核当前登记、凭据、SAN、期限与时钟。
  - 权限：要求调用者当前 assignment 明确授予 `identity.principal.read`；默认角色不隐含。
    资源范围规则：`RESOURCE_CATALOG id=control` 为管理目录全域；其他 exact resource 仅返回
    与该资源绑定的 principal 及其可见 roles，避免跨范围枚举。
  - 只返回 `PrincipalKind::Human`；`LIFECYCLE_UNSPECIFIED` 返回所有非 deleted/purged 状态，
    显式 filter 按当前状态；deleted/purged 永不返回（显式过滤返回权威空页 + checked_at）。
  - stable principal-id keyset 分页：`page_size` 0→20、最大100；opaque token 绑定
    state filter+最后 principal id；`checked_at` 取事务内真实 DB UTC（`current_time`），
    不取 Control/本地时钟。
- 存储：`IdentityUnitOfWork::query_principals`（默认 fail-closed，其他 owner/无存储 fixture
  返回 UpstreamUnavailable）；PG `CommonWork` 在 Identity 私有库一个 UoW 内查
  `principals`/`exact_assignments`/`exact_roles`，限定 kind=human、state NOT IN (3,5)，
  可见性用绑定资源或 active exact roles 资源；roles 再按可见范围过滤。无跨库读取。
- RPC：`IdentityRpcOptions.principal_directory`；`IdentityService.ListPrincipals` 仅接受
  Control platform peer（`run(..., control_only=true)`），校验 context/deadline/UUID/页码边界，
  投影 `IdentityPrincipalSummary`（binding/version/state/roles）与 SnapshotPage/checked_at；
  未装配应用时稳定 CAPABILITY_NOT_SUPPORTED，不伪造空页。
- 组合根：Identity 进程装配 application 并注入 RPC；Control 既有 `/api/v1/principals` 转发不变。

## 测试（main@fc454bc 工作树，full-locked）

- `cmake --build --preset full-locked --parallel 6`：PASS。
- `ctest -R 'p04|p12|control|p05|p03_postgres'`：**56/56 passed**，包括：
  - `p04_identity_application`（真实 PG16.4）：新增目录成功/两种 keyset 页/state filter 空页/
    非法 page_size 400/非法 token 400；含真实 Control peer、真实 PG 读写。
  - `p04_identity_rpc`（真实 TLS mTLS fixture）：成功投影（principal_id/binding/kind/state/
    version/ROLE_ADMIN/page token/checked_at）、page_size=101→INVALID_ARGUMENT、
    PermissionDenied 映射、page token/state filter 透传。
  - `p01_target_api`（需 node_modules fixture）PASS。
- contract 门禁：`check_implementation_status.py`、`target_api_test.rb`、`evidence_scopes_test.py`
  （9 tests）、`check_contracts.sh`（descriptor compatibility）、OpenAPI lint（13 既有 warnings）
  全部 PASS。

## target 翻转

- `target_spec.json`：IdentityService.ListPrincipals status implemented。
- `contract_status.json` target overrides/evidence：`operations.listPrincipals`、3 个 principal
  目录消息、12 个字段 implemented。
- `evidence_scopes_test.py` expected 集合同步；`doc/01` 执行范围表新增行；OpenAPI
  `GET /principals` 的 `x-contract-status: implemented`，但 `x-target-contract-status` 按
  target_api 认证投影规则保持 not_implemented（非 owner-rpc 场景，不弱化规则）。
- 文档：doc/06/08/21 + 本报告；未改 proto/实现语义之外契约。

## 未完成/风险

- 原生单机栈 principals 成功链由 P14j 在 P12l 合入后复验；本任务只以真实 PG + 真 mTLS RPC
  fixture 和 Control 转发证明。
- “分页快照”为 keyset 稳定视图（本任务契约按 principal-id keyset）；未实现十分钟不可变快照
  缓存，跨页期间新增/删除 principal 可能改变后续页（与 session 快照不同）。若 P12 后续要求
  严格不可变快照，需另立合同/缓存。
- 可见性语义：catalog control 视为管理目录全域；其他 exact resource 过滤主体/roles。若业务希望
  “catalog control 也按资源收窄”，需 P12a 澄清。
- 未覆盖跨主机、真实目标栈、真实 QQ/Provider；真实 Control→Identity 原生链路由 P14j 验证。
