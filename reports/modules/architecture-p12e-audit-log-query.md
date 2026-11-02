# P12e Identity/Runtime 只读审计/日志查询 RPC

状态：实现完成（Control REST 聚合接线留给 P12c）。

## 范围

- 新增 4 个只读 owner RPC：Identity `ListAudit`/`ListLogs`、Runtime `ListAudit`/`ListLogs`；
  不新增服务、权限编号、错误码、proto 字段语义或数据库表。权限复用 P12a 的 `audit.read`(49) /
  `log.read`(48)。
- caller/scope：Identity 接受 Control platform peer 并要求其 principal 有精确 catalog `control`
  的 `audit.read`/`log.read` assignment；Runtime 接受 Control 当前在线委托并经
  Identity.CheckAuthorization 复核同一 scope。两者只读自身权威 `audit_entries`，不跨库。
- 统一分页/过滤：`from` 含、`until` 含；actor、resource 可选过滤；`occurred_at DESC, id DESC`
  keyset cursor（opaque `nanos:id`，长度/规范校验），默认 20 最大 100；无效 cursor/窗口
  `INVALID_ARGUMENT`。
- 脱敏：审计只投影 entry_id/actor/resource/request_id/trace_id/method/rejection/
  authorization_version/occurred_at 与可选 Action account/conversation；日志是同一事实的运维
  投影（severity=info/rejected、component=owner、message 闭集），不含凭据、正文、模型密钥或
  自由错误文本。无权威来源返回 `UPSTREAM_UNAVAILABLE`，不返回成功空页。

## 实现

- `core/include/kiarabot/core/domain/target/audit_query.hpp` + `UnitOfWork`
  `query_audit/query_logs`（默认 fail-closed）+ `CommonWork` 真 SQL + `WorkDelegate` 转发。
- 纯应用：`IdentityAuditApplication`（Identity 本地认证/assignment）、
  `RuntimeAuditApplication`（RuntimeAuthorizationPort 在线复核）。
- RPC：Identity/Runtime `ListAudit`/`ListLogs` 适配器，proto 新增 request/response 与
  `PERMISSION_AUDIT_READ`/`PERMISSION_LOG_READ` + `RESOURCE_CATALOG` scope_kinds。
- 组合根：Identity/Runtime 各自装配应用；owner RPC 未配置时返回稳定 CAPABILITY_NOT_SUPPORTED。
- Control `/logs`、`/audit` OpenAPI 增加 owner-RPC 映射并保持 `x-contract-status:
  not_implemented`，待 P12c 接线。

## 验证

- `p03_postgres_database`（真 PG16.4）：三行审计种子，keyset 两页、actor/resource/时间过滤、
  空过滤不等于不可用、脱敏日志 severity/message、非法 cursor 拒绝；Identity/Runtime/Scheduler/
  Model 四个库均执行公共检查。
- `p12e_audit_query`：cursor 编解码规范/拒绝。
- `proto/tests/check_implementation_status.py`、`evidence_scopes_test.py`、
  `check_semantic_rules.py`、`check_enum_consistency.py`、`check-target-api.sh` 通过；
  `check_target.sh` 因本机 protoc 版本与 vcpkg 不一致未运行（构建用 vcpkg protoc 29.3，
  系统 protoc 36.2）。

## P05b 记录

P05b（Runtime 实例委托获取 + 真实插件→Runtime→Model 深链路 e2e）方案 A/B/C 已被 Lead 暂缓：
A 改 Identity 委托冻结语义、B/C 新增 proto 字段，均超出本轮确认范围；记录为「待用户确认的
契约变更 → 深链路 e2e 待验收」，P05 已有平台 peer 真实授权 e2e 与分层证据不受阻。

## P12e-2：真实 TLS 成功聚合 + 跨源 keyset 分页（2026-11-02）

- 修复 Control 跨 owner 分页缺陷：原实现把单一 `occurred_at:id` token 同时透传两源；当某
  owner 只贡献合并页前缀时，另一 owner 中位于合并页末行之前、该 owner 页末行之后的行会被
  跳过。现 token 为复合 keyset（每 owner 独立 cursor 或 exhausted），下页归还各自 cursor；
  同时间戳 id 按数值 DESC，owner ASC。gateway 单元测试用两源交错/同时间戳/4 页数据断言
  无重复无遗漏，并保留 legacy token 兼容。
- 真实 TLS fixture：Identity fixture 实现 `ListAudit/ListLogs`（成功页/过滤/keyset）并允许
  Control 的 runtime audience log/audit delegation；新增 `runtime_fixture` 提供第二权威源。
  `p12_audit_logs_rest` 验证真实浏览器 TLS→Control→mTLS 两源成功聚合：owner 标注、顺序、
  4 页 token 翻页、actor 过滤；401/400/403/503 语义保持（无 runtime fixture 的 p04 用例仍覆盖）。
- 真 PG16.4：`p03_postgres_database` 增加相同 `occurred_at` 的 keyset 全量分页（limit 2）与
  id DESC 断言；原有过滤/脱敏/空页/非法 cursor 覆盖保留。
- 验证：`p12_audit_logs_rest`、`p12_control_owner_gateway`、`p04_control_auth_https`、
  `p11_control_model_rest`、`p03_postgres_database` 通过；全量构建通过。
