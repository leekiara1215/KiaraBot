# P05b-6：B1 实例父委托 sealed 短期文件交付

状态：用户选择 B（sealed 短期文件 + Control/部署刷新）；无 proto/API 契约变更。基线 main=`4be0d76`
（P05b-2 实例父委托签发已合入）。本文件先于实现冻结文件/安全/运维契约。

## 1. 交付边界

Identity 仍是唯一签名方；Control/部署侧以**管理员 access token**（由 Control 管理员登录取得）
作为 subject proof，用 Control 的 mTLS 叶调用既有 `Identity.IssueDelegation(instance_id=...)`，
将返回的 hops1 父委托 proof 封装为 JSON 文件，经单机共享卷交给 Runtime。Runtime 不由文件内容
获得新的授权语义：读取后仍必须用既有 `ExchangeDelegation` 换子委托，并让 Model/Scheduler 做
签名/在线 `CheckAuthorization`。P05b-4/P05b-5 的 Runtime provider/读取侧不在本任务。

## 2. 路径与文件格式

- 部署配置目录（示例 `/run/kiarabot/delegations`），由部署配置指定；目录应 0700 且属于
  部署/运行用户。默认文件 `instance-delegation-<instance_id>.json`，可通过配置覆盖完整路径。
- 文件必须是 **regular file、mode 0600、owner=部署用户（euid 或配置的 allowed_uid）**，任何
  组件不得是符号链接；拒绝 group/world 权限。
- JSON（UTF-8，≤64 KiB）：

```json
{
  "schema_version": 1,
  "instance_id": "<uuid>",
  "installation_id": "<uuid>",
  "audience": "runtime",
  "permissions": ["task.schedule", "task.read"],
  "scope": [
    {"kind": "RESOURCE_INSTANCE", "id": "<uuid>"},
    {"kind": "RESOURCE_INSTALLATION", "id": "<uuid>"}
  ],
  "issued_at": 1793000000,
  "expires_at": 1793000056,
  "ttl_seconds": 56,
  "delegation_proof": "<Identity signed JWT>"
}
```

- `issued_at/expires_at` 是 JWT NumericDate 秒；必须与 proof 内层 `iat/exp` 一致，
  `0 < expires_at-issued_at <= 56`，`ttl_seconds == expires_at-issued_at`。
- `permissions/scope` 只允许 P05b-2 白名单：`model.chat/model.call.inspect`（MODEL）与
  `task.schedule/task.read`（INSTANCE/INSTALLATION）；本交付默认 task 组合。
- `delegation_proof` 仅是短期证明，不是长期 secret；不得写日志/审计/错误消息/argv/env。

## 3. 写入安全（Control/部署侧）

1. 配置/凭据文件必须 0600 regular file，拒绝符号链接；admin token 通过 stdin 或 0600 文件
   提供，绝不通过 argv、环境变量（除测试 harness 显式注入）或日志。
2. 管理员鉴权：token 必须来自 Control 管理员登录；Identity 以 Control mTLS 叶 + token 验证
   （P05b-2）。未授权/无实例 binding/错误 instance → 非零退出，**不创建/不替换目标文件**。
3. 原子写：同目录创建 `O_CREAT|O_EXCL|O_NOFOLLOW` 临时文件（0600），写入 JSON、`fsync(file)`，
   `rename()` 覆盖目标，`fsync(dir)`。失败清理临时文件；目标保持旧文件或不存在。
4. 目标已存在时：必须是 regular non-symlink、owner 正确；若是符号链接/非 regular/owner 错误
   直接拒绝（不跟随、不先删除）。替换采用 rename 的原子覆盖语义。
5. 旧文件仍有效时刷新失败 → 保留旧文件；旧文件已过期 → Runtime fail-closed，不把“文件存在”
   当授权成功。
6. 刷新工具（`scripts/deploy/instance-delegation-refresh`）支持 `--once`、`--interval SECONDS`
   （必须 <56s，建议 15–20s）；每轮重新签发并原子替换，结果幂等（同一实例/格式，旧内容被
   完整替换，无临时文件残留）。

## 4. 轮换、撤销与过期

- 父委托≤56s，部署刷新周期必须小于 TTL；超过 TTL 未刷新 → Runtime 稳定 `UNAUTHENTICATED`/
  fail-closed，不伪造成功。
- Identity 撤销管理员 session/credential/assignment、实例 principal 状态或 binding → 下一轮
  签发/在线校验失败；sealed 文件本身不缓存决策，Runtime 每个 Exchange/Check 仍在线。
- 轮换：重复刷新签发新 proof 并原子替换；旧 proof 只在 TTL 内可能被已读取的进程使用。
- 撤销传播受 TTL 与刷新周期上界约束，运维文档必须明示；不提供“文件级 revoke”RPC。

## 5. Runtime 读取校验规则（P05b-4/5 集成点）

Runtime 读取侧（或本任务提供的 reader helper）必须：
1. `lstat` 目标：regular、非符号链接、mode 0600、owner=配置 allowed_uid；
2. 大小/JSON/schema/字段类型校验；拒绝未知/缺失字段；
3. `instance_id` 必须等于 Runtime 当前管理的实例，`installation_id` 匹配安装；
4. audience=runtime；permissions/scope 白名单且含目标 instance；
5. `exp > now`，`0<exp-iat<=56`，proof 为三段 JWT 且内层 `iat/exp` 与文件一致；
6. **文件不是签名信任根**：Runtime 必须把 proof 交给 Identity Exchange/CheckAuthorization 在线
   验签，且必须把 JWT subject 与实例 principal 的绑定关系由 Runtime 注册信息/Identity 校验
   确认；禁止仅凭 JSON 的 instance_id 接受 token，防止文件配对替换。
7. 任何失败 fail-closed，不重试使用过期文件。

## 6. 日志/凭据

日志仅允许：instance_id、文件路径、expiry、刷新结果/稳定错误码；禁止 access token、JWT、
admin 密码、完整配置。错误消息不得回显文件内容。临时文件权限 0600。

## 7. 多主机扩展点

单机共享卷（compose volume）为当前默认；多主机后续通过内部 mTLS 通道/对象存储+完整性校验
扩展，不改变本 JSON/安全语义；不让 Runtime 在网络上暴露读取端。

## 8. 验证

- 单元：0600/regular/符号链接拒绝/owner 错误/原子替换/失败不破坏旧文件/过期和 TTL 拒绝/
  错误 instance/重复刷新幂等/日志不含 token。
- 真 PG16.4+真 mTLS：管理员 token + Control 叶调用 Identity IssueDelegation → sealed 文件 →
  reader 校验；未授权/错误 instance 不产生文件；刷新替换后 reader 接受、旧文件失效。
- 门禁：p05b/p05 label，contract 门禁，全量构建，`ctest -R 'p05|p04|p12|p03_postgres'`。

## 9. P05b-7：Model scope 扩展（2026-11-03）

P05b-4 发现 model.chat 父委托无法交付。sealed 文件与刷新工具扩展为**按 kind 分组**，不把
task 与 model 权限混在同一 envelope/token（避免 Identity 的 scope×permission 交叉积）：

| kind | permissions | scope | 默认文件 |
| --- | --- | --- | --- |
| task | task.schedule, task.read | INSTANCE/{instance_id} + INSTALLATION/{installation_id} | `instance-delegation-<instance>.json` |
| model | model.chat, model.call.inspect | RESOURCE_MODEL/{model_id}（恰好一个） | `instance-delegation-<instance>-model.json` |

- envelope 顶层继续包含 instance_id/installation_id/audience=runtime/issued_at/expires_at/
  ttl_seconds/delegation_proof；model kind 的 scope 不要求 INSTANCE，但 instance_id 仍是
  父委托的 subject 目标，必须与 Runtime 当前实例核对。
- refresh 工具新增 `--scope-kind task|model`、`--model-id`、`--permissions`（逗号/重复；
  model 默认 model.chat,model.call.inspect；task 默认 task.schedule,task.read），也支持配置文件
  `scope_kind`/`model_id`/`permissions`。TTL/0600/原子写/审计不变。
- reader `read_sealed_delegation` 增加 `expected_model`，并拒绝：task/model 权限混用、
  model scope 与其他 kind 混合、多个 MODEL、缺失/错误 model_id、非白名单权限。
- Identity 侧 model 父委托由 P05b-7 修正：instance_id 绑定用于 subject，model 权限的
  resource 是 RESOURCE_MODEL；管理员 exact assignment 仍按 Model 资源，不要求 INSTANCE scope
  交叉；task 路径不变。跨模型（管理员未授权 Model）与错误 model scope 拒绝。
- 文件仍是 envelope，不是签名信任根；Runtime 读取 model kind 后必须用 ExchangeDelegation
  换 audience=model 子委托，再由 Model 在线 CheckAuthorization。
