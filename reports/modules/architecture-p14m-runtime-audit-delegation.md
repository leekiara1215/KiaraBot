# P14m：Runtime 审计源 401（Control 委派 UNAUTHENTICATED）根因修复

- 基线：main@`1e4e52a`；工作树 `codex/p12m2`。
- 现象：native 栈 Control `/logs` `/audit` 聚合中 Identity 源已成功，Runtime 源仍以
  `UNAUTHENTICATED` 失败；即使无数据也应 200 空页。

## 根因

`core/src/runtime/platform/runtime_identity_client.cpp::decode_delegation_claims` 对 Control
一次性 delegation 的结构校验有两处与 P07/P12 已冻结权限/scope 不一致，均发生在在线
`Identity.CheckAuthorization` 之前，因此直接返回 UNAUTHENTICATED：

1. `delegation.permissions` 的 permission 数值上限为 **46**；`log.read=48`、`audit.read=49`
   被当成非法 claim 拒绝。P12e 的 `/logs` `/audit` 正是用这两个权限。
2. scope resource id 必须满足 `canonical_uuid`；Control 聚合使用的
   `RESOURCE_CATALOG id=control` 是稳定契约 id、不是 UUID，也被拒绝。

这两点与 domain 的 `valid_policy_resource` / `known_permission` 规则不一致：Catalog scope
允许契约 id，权限上限应覆盖 P12 的 48/49（及 50）。

## 修复

仅修改 Runtime 的委派 claim 结构校验（不扩 proto、不改业务授权语义）：

- permission 数值上限改为 `Permission::HealthRead`（50），仍要求 `known_permission` 且
  `permission_name` 非空；负数/越界/未知值继续拒绝。
- scope 使用 `valid_policy_resource` 校验（非空、非 `*`、kind 合法）；仅 **Catalog** 允许
  非 UUID 的契约 id，其他资源种类仍要求 canonical UUID；permission/resource 的最终权威仍是
  Runtime 的 `Identity.CheckAuthorization`（在线、逐命令），结构校验只负责 fail-closed 解析。
- 不伪造成功：无数据时由 owner 授权后查询返回 200 空页；权限不匹配→PERMISSION_DENIED；
  过期/篡改→UNAUTHENTICATED。

## 测试（真实 PG16.4 + 真 mTLS 双进程 Identity/Runtime + Control 签发 delegation）

扩展 `core/tests/platform/runtime_identity_e2e_test.py`（真实 Identity/Runtime 进程、真实
PG16.4、真实 TLS、Control 证书签发的 delegation）：

- 新增 administrator 的 `log.read`+`audit.read` 精确 `RESOURCE_CATALOG id=control` assignment，
  由 Identity IssueDelegation 签发真实 proof。
- `RuntimeService.ListAudit`：真实 delegation 成功并返回有数据页（entry_id/method/checked_at）；
  未来时间窗过滤→**200 空页**（不是 401）；`ListLogs` 成功。
- 拒绝：用 `log.read` delegation 调 `ListAudit`→PERMISSION_DENIED；把已验证 proof 的 claims
  exp/iat/nbf 篡改为过期→UNAUTHENTICATED（结构校验在查询前拒绝）。
- CTest `p05_runtime_identity_e2e` 增加 `p12;p14` label；不修改 build-deploy 占用的
  `tests/architecture/p14_native_single_host.py`。

验证命令/结果见提交说明；最终由 build-deploy 在 native 栈重跑 models/audit 矩阵。

## 未完成/风险

- native 单机栈复跑由 build-deploy 执行（本任务只修 Runtime 解码并补真实双进程 e2e）。
- 真实 Control 进程仍需部署 bootstrap 给调用主体/Control peer 配置对应 catalog assignment；
  缺授权时保持 403/fail-closed。
- 本修复不改变审计/日志的 owner 权威、保留期、脱敏或 keyset 语义；仍不返回跨库数据。
