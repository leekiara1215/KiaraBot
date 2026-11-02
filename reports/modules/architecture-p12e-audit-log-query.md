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
