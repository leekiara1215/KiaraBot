# P05b-2：Identity 实例主体父委托签发（B1）契约决策

状态：用户已批准 B1 语义（Control 管理员触发实例主体父委托）；本文件先于实现冻结 wire/授权语义。
日期：2026-11-03；基线 main=`23aa6ab`（P13g 合入后）。

## 1. 目标与范围

Identity 现有 `IssueDelegation` 只能以 Control 登录会话主体签发 delegation。本变更允许
**Control 管理员会话**为已注册 `PrincipalKind::Plugin`、binding=`RESOURCE_INSTANCE` 的实例
principal 签发一条 audience=`runtime`、TTL≤56s 的父委托，使 Runtime 可用既有
`ExchangeDelegation` 换取 audience=model/scheduler 的 hops=2 子委托。

本任务只做 Identity 签发与父委托来源/在线校验扩展；**不改变 Model/Scheduler owner 验证语义**
（owner 仍验签、在线 `CheckAuthorization`、owner=subject）。Runtime provider、Control 编排、
P11/P10k 生产链路属 P05b-4/5。

## 2. Wire/冻结语义 diff

- `IdentityIssueDelegationRequest` 新增可选字段 **`string instance_id = 7`**（资源 id，不是任意
  subject principal id）。字段缺失时行为完全不变。proto 兼容：新增 optional 字段，旧客户端
  兼容；无新 RPC、无响应字段、无权限编号。
- 当 `instance_id` 非空：
  - `audience` 必须为 `runtime`；
  - subject 只能由 `instance_id` 唯一绑定解析，**禁止任何 subject/subject_id 参数**；
  - claims：`subject`=实例 principal id，`direct_actor`=实际 Control platform peer principal，
    `actor_chain=[Control platform peer]`，`business_hops=1`，`source_credential/source_session`
    =管理员当前 credential/session（登录主体），`client_certificate`=Control 叶指纹；
  - 未配置/未授权/无绑定一律 fail-closed，稳定 `PERMISSION_DENIED`/`NOT_FOUND`/
    `UNAUTHENTICATED`/`INVALID_ARGUMENT`，不签发。
- 现有 Control 管理员主体委托路径不变（`instance_id` 为空）。

## 3. 授权与白名单

签发需同时满足：
1. 实际 Control mTLS 叶注册为 Control platform peer，且管理员 access session 当前有效（现有）。
2. 管理员 principal 有 `identity.delegation.issue`（现有 exact capability）。
3. 管理员对 `RESOURCE_INSTANCE/{instance_id}` 有 `resource.manage`，并对每个请求 permission/
   resource 组合有 exact assignment（复用现有 `authorize_exact`，不扩大管理员权限）。
4. instance principal 存在、`kind=plugin`、binding 精确等于 `RESOURCE_INSTANCE/{instance_id}`，
   且当前状态 Active；一个实例只能有一个绑定 principal（Identity 唯一约束）。
5. permission/scope 白名单（在管理员 exact assignment 之前再收窄）：
   - `model.chat` / `model.call.inspect` → resource kind 必须 `RESOURCE_MODEL`；
   - `task.schedule` / `task.read` → resource kind 必须 `RESOURCE_INSTANCE` 且 id 必须等于
     `instance_id`，或 `RESOURCE_INSTALLATION`（安装范围需管理员 assignment 另行精确授权）；
   - 其他 permission 或 kind 一律 `INVALID_ARGUMENT`/`PERMISSION_DENIED`。
6. 请求 scope 必须至少包含 `RESOURCE_INSTANCE/{instance_id}` 的绑定；出现其他 instance id →
   拒绝（跨实例）。

TTL：沿用 `kDelegationBodyLifetime=56s`，并取 session/credential/cert 最早到期；请求没有 TTL
参数，固定上限，无法延长。`expires_at-issued_at≤56s`。

## 4. 父委托的在线来源校验（Exchange/CheckAuthorization）

父委托的 subject=实例、source credential/session=Control 管理员，这与现有
`require_delegation_origin_current` 的 “source credential subject==claims.subject” 假设冲突。
最小扩展：
- 识别条件：claims.subject principal 为 `kind=plugin` 且 binding=`RESOURCE_INSTANCE`，且
  source credential/session 的 subject 是一个当前 Human 管理员会话（不是 claims.subject），
  `claims.actor_chain.front()` 是该 Control platform peer，且 `claims.business_hops∈{1,2}`。
- 该模式下，source origin/assignment/authorization_version 以管理员登录主体为准；实例 principal
  只作为 subject/owner 标识，另校验其 binding/状态；`CheckAuthorization` 仍要求实例当前
  authorization watermark 并返回稳定结果。
- 其他 delegation（普通人类主体）保持原校验，不允许任意 subject 与 source 分离。
- ExchangeDelegation 子委托仍由 Identity 签名，subject=parent.subject，actor_chain 追加 Runtime，
  client_certificate=Runtime 叶，permissions 必须为父集合子集（现有规则）。

## 5. 撤销/审计/威胁模型

- 撤销：管理员 assignment/credential/session 或实例 principal 状态/绑定的变化，在下一次
  Exchange/CheckAuthorization 在线检查时生效；父委托本身≤56s，无长缓存。实例 credential
  撤销不直接使父委托失效（父委托来源是管理员会话），故 B1 的撤销边界是 **管理员会话/授权
  + 实例 principal 状态**；文档和测试明确。
- 审计：实例父委托签发成功时，在同一 Identity owner 写一条 audit，记录管理员 actor、
  instance 资源、方法 `IdentityIssueDelegation/InstanceParent`、request/trace id；不记录
  proof/secret。失败不写成功审计。撤销沿用现有 assignment/credential 审计。
- 威胁模型：
  1. 伪造 subject：无 subject 参数；只能由管理员的 `resource.manage`+唯一 binding 解析，
     请求中 `instance_id` 不匹配则拒绝。
  2. 跨实例/跨安装：签发要求 scope 含目标 instance id 且管理员按实例 exact 授权；
     Exchange 只能收窄父 permission 集合，不能换 instance。
  3. 证书轮换/错配：父委托绑定 Control 叶；Exchange 实际 peer 必须是 Runtime 叶；
     下游 owner 验实际叶，轮换后旧委托在 TTL/在线检查内失败。
  4. 管理员权限滥用：管理员除 `identity.delegation.issue` 外仍需按每个 permission/resource 的
     exact assignment，且仅白名单 permission 可签发；不能把任意外部 scope 变成实例权限。
  5. 撤销延迟：≤56s 窗口 + 每次 Exchange/Check 在线检查；不缓存决策。
  6. 日志泄漏：不记录 proof/session/credential secret；审计仅 id/资源/方法/时间。

## 6. 兼容与验证

- 旧 `IssueDelegation` 不带 `instance_id` 的调用完全保持原行为；Reserved/未知字段兼容。
- proto 变更后跑 proto lint/descriptor/contract 门禁及生成代码。
- 真 PG16.4 + 真 mTLS 测试：
  正例：管理员为实例签发（claims 断言）→ Runtime 叶 Exchange 得到 hops2 子委托；
  负例：非管理员/无实例 manage、实例无 binding、错 scope/permission、证书不匹配、跨实例、
  撤销管理员 assignment 后 Exchange 拒绝、过期父委托拒绝。
- 文档同步：doc/01、doc/21、本文件、P05 报告；P05b-4/5 解锁点：Runtime provider 可获取/刷新
  该父委托并 Exchange；P11/P10k 生产链路可继续。
