# 账号封禁风险评估与分层控制建议

- 评估日期：2026-10-30；代码基线：工作区 `9d532c8`（含未提交改动）。
- 范围：本仓库（协议、C++ Core、SDK、插件、部署）与相邻独立适配器 `../kiarabot-adapter-nonebot`；外部事实核对日期 2026-10-30。
- 定位：这是风险评估与实施建议，不是已批准设计、契约变更或法律意见。任何落地项都应先按 [任务模板](10-task-template.md) 拆分、确认优先级，并按 AGENTS.md 的文档同步规则更新对应契约。

## 结论

1. **有封号风险，且根因不在 Core/插件层。** 当前链路用 NapCatQQ 以第三方工具操控 NTQQ 客户端登录**个人 QQ 号**（[README](../README.md#目标架构)）。《QQ 软件许可及服务协议》禁止以非腾讯授权第三方工具接入、以非人工方式批量操作，并明列封禁措施。腾讯据此处置账号时，代码层没有"技术合规"申诉空间。
2. **工程手段只能在保持个人号协议端的前提下降低概率、控制影响范围，不能"避免封号"。** 真正称得上"避免"的只有**渠道层替换**：接入官方 QQ 机器人（QQ 开放平台），或改用其他 IM 的官方 bot。
3. **现有架构的隔离方向正确。** 渠道适配器可替换、Core 不解析 OneBot、当前只开放纯文本发送、Action 幂等且未知结果不重复调用平台。这样换渠道、停用个人号或按渠道降级时，Core 与插件不需要重写。
4. **缺口集中在四处：** 没有出站发送配额；危险 Action 没有默认门禁；适配器不区分风控类平台错误；没有账号、网络和暴露面的运维规范。前两项应在 Core 的 Action 路径上强制，第三项在适配器落地，第四项写入运维文档。
5. **自用/内测与对外服务的结论不同。** 自用可以用小号、独立设备与网络、被动响应、关闭公网暴露面把概率降到较低，但仍属违约状态；对外提供服务或商用，应直接按官方渠道设计。

## 1. 当前链路与各层风险

```text
个人 QQ 号
  │ 第三方工具操控 NTQQ（NapCatQQ）      ← 风险根源：违反平台协议
  │ OneBot V11 / WebSocket
  ▼
kiarabot-adapter-nonebot                ← 收发行为、重连、错误映射
  │ gRPC / Protobuf
  ▼
C++ Core                                ← 权限、幂等、审计、Action 分发
  │ PluginSDK
  ▼
插件                                    ← 业务回复、群管理、模型调用
```

各层风险与现状：

| 层 | 当前实现 | 典型封号/风控触发 | 现有控制 | 残余风险 |
| --- | --- | --- | --- | --- |
| L0 平台/协议端 | NapCatQQ 登录个人号（[README](../README.md#目标架构)、[THIRD_PARTY](../THIRD_PARTY_NOTICES.md#L27-L38)） | 第三方客户端识别、账号画像（新号、机房 IP、异常活跃）、被举报 | 无；由外部组件决定 | **高**，Core 无法消除 |
| L1 账号/网络 | 单账号，可部署在服务器 | 公网暴露 OneBot/WebUI、token 缺失、频繁切 IP/设备、多开 | 适配器要求 token；Core gRPC 只连 `127.0.0.1`；NapCat 不进入镜像 | 中高 |
| L2 适配器 | NoneBot2 转发、本地 outbox、Action 执行日志 | 平台限频、错误重试导致重复发送、登录态失效 | 执行日志按 `action_id` 去重；未知结果不重发；重连退避 1–30 秒（[doc/04](04-nonebot-adapter.md#L7-L8)；适配器独立项目 README 的 outbox/去重说明） | 中 |
| L3 Core/Action | 权限、能力、幂等、审计、1 万条容量 | 高频发送、群发、危险管理动作 | 只接受 `send_message` 文本；`MuteMember`/`KickMember`/`ApproveRequest` 当前被拒绝（[action_router.cpp](../core/src/action_router.cpp#L110-L119)） | 中，缺速率与内容策略 |
| L4 插件/业务 | `ai-reply` 只响应 `/ai` 命令；`event-observer` 只记录 | 全量自动回复、主动推送、批量 @ | `message.send`、`model.chat` 等权限需显式配置 | 中低，取决于新增插件 |
| L5 运营/合规 | 第三方声明、不分发 NapCat | 商用或再分发、平台投诉 | 仅 [THIRD_PARTY](../THIRD_PARTY_NOTICES.md#L36-L38) 免责声明 | 高（商业场景） |

## 2. 外部事实（2026-10-30 核对）

以下外部结论用于判断风险来源；标注"传闻"或"未核实"的内容不能作为实施依据。

1. **NapCat 不是协议自实现，而是操控官方 NTQQ 客户端。** 官方文档明确"并非完全自己实现……主动调用客户端存在的功能"，V3 起通过原生包拦截（Hook）工作（[NapCat 文档](https://napneko.github.io/guide/napcat)，官方）。这意味着账号行为会带着客户端与注入特征。
2. **个人号协议端属于违约使用。** 《QQ 软件许可及服务协议》8.2.2(4)(6) 禁止用插件、外挂或非腾讯授权第三方工具接入；8.2.3(3)(5)(6) 禁止非人工方式登录、发消息、加好友/群和批量操作；8.3.1 列明封号等措施（腾讯官方页面为 JS 渲染，正文经[镜像](https://webvpn.jiangnan.edu.cn/https/77726476706e69737468656265737421e4fe0f8d367e6b5f73/agreement/index.html)核对，强）。
3. **已有实际处置案例。** 2025-09-05 一批未设 token 的公网 OneBot 服务被攻击者调用发送违法内容，涉事 QQ 号与群被永久封禁（[复盘](https://wesley-young.github.io/2025-09-05-attack-on-onebot-service)，个人复盘，中）；NapCat issue #1304 记录开启 HTTP 服务后不久被下线并触发安全校验，发帖人称多次被封与警告（[issue](https://github.com/NapNeko/NapCatQQ/issues/1304)，2025-10-09，中）。
4. **账号画像与行为是更强触发项。** 社群反馈新注册小号当晚被风控、数小时内被踢下线，老号与稳定设备更少触发；同时存在幸存者偏差，不能量化（[linux.do 讨论](https://linux.do/t/topic/2228020)，2026-05，弱/传闻）。2025-04 腾讯公告 QQ 机器人主动推送"能力收敛"（[IT之家](https://www.ithome.com/0/846/651.htm)，转官方公告，强）。
5. **官方 QQ 机器人是唯一合规 QQ 群渠道，但约束明显。** 个人可注册并实名，未认证只能进入自己是群主/管理员的群，个人认证"暂不支持公开使用"、进群上限 500，企业认证才可公开（[QQ Bot 介绍](https://bot.q.qq.com/wiki/)、[运营规范](https://bot.q.qq.com/wiki/business/)，官方）；入群后群主需授予"接收全量消息/主动推送/撤回成员消息"等权限（[FAQ](https://bot.q.qq.com/wiki/bot_FAQ/)，官方）；被动回复窗口为群聊 5 分钟、每条消息最多回 5 次，单聊 60 分钟 5 次（[发送消息文档](https://bot.q.qq.com/wiki/develop/api-v2/server-inter/message/send-receive/send.html)，官方；主动推送限额在官方文档间存在矛盾，应按"仅被动回复"设计）。
6. **NapCat 许可证限制使用与商用。** 许可文本规定未经作者许可禁止使用、复制、修改、分发，禁止商用，修改版不得公开发布（[LICENSE](https://github.com/NapNeko/NapCatQQ/blob/main/LICENSE)，强）。当前仓库只声明不分发，不等于获得使用授权。
7. **开源协议端生态在收缩。** Lagrange V2 明确不再实现 OneBot 11，V1 的 OneBot 实现不再维护（[Lagrange 文档](https://lagrangedev.github.io/Lagrange.Doc/v2/)，官方）；LLBot 停更仍属社群传闻（[讨论](https://linux.do/t/topic/2441914)，2026-06，弱）。
8. **替代渠道各有硬门槛。** Telegram 免审核但 bot 不能主动发起私聊，大陆需自建出口（[官方](https://core.telegram.org/bots)）；企业微信群机器人 webhook 只能推送不能接收（[官方社区](https://developer.work.weixin.qq.com/community/question/detail?content_id=16822979298793752201)）；钉钉、飞书自定义群机器人同样以推送为主，企业机器人需企业认证与公网回调（[钉钉](https://open.dingtalk.com/document/development/development-robot-overview)、[飞书](https://open.feishu.cn/document/client-docs/bot-v3/bot-overview)，官方入口，正文未逐项核对）。
9. **无法核实：** 腾讯是否有针对 NapCat 的专门识别规则；封号率的量化数据；官方主动推送当前是否恢复及限额；LLBot 官方停更声明。

## 3. 现有设计中有效的隔离

- **渠道可替换**：目标架构明确适配器可替换为 C++ 适配器而不改 Core（[README](../README.md#目标架构)、[doc/04](04-nonebot-adapter.md#L124-L134)）。换官方渠道时主要新增适配器实现，不改业务规则。
- **Core 不接触平台对象**：Core 禁止导入 NoneBot2/NapCat，插件不得调用 OneBot API（[doc/00](00-boundaries.md#L21)、[doc/00](00-boundaries.md#L72-L74)）；平台字段不会写进业务分支（[doc/02](02-core.md#L186-L197)）。
- **当前 Action 面很窄**：Core 只接受 `send_message` 纯文本，踢人、禁言、审批等危险动作虽在契约中定义（[actions.proto](../proto/kiarabot/v1/actions.proto#L18-L26)），当前实现会拒绝（[action_router.cpp](../core/src/action_router.cpp#L110-L119)；适配器 `../kiarabot-adapter-nonebot/src/kiarabot_adapter_nonebot/actions.py` 的 `validate` 同样只放行 `send_message`）。
- **不重复副作用**：适配器执行日志按 `action_id` 落盘占用，同键重投返回原结果；平台调用超时或未知时不重发，由 Core 按 deadline 过期（[doc/04](04-nonebot-adapter.md#L7-L8)）。
- **账号预先授权**：账号必须由 Core 管理员授权，capabilities 只描述能力、不授予权限（[doc/04](04-nonebot-adapter.md#L153)）。

## 4. 缺口

1. **没有出站速率/配额，只有容量上限。** `ActionRouter` 对每个插件只检查 Action 表是否达到 1 万条（[action_router.cpp](../core/src/action_router.cpp#L240-L245)），没有按账号、会话、Action 类型的发送速率限制。而设计文档声称发送会经过"限流"（[doc/02](02-core.md#L156)、[doc/03](03-plugin-sdk.md#L175)），当前实现与描述不一致。
2. **危险 Action 没有默认门禁设计。** `MuteMember`/`KickMember`/`ApproveRequest` 已在契约中，但尚未实现，也没有"哪些插件、哪些群、每天多少次"的授权模型。一旦后续实现，现有权限模型（仅 `message.send`）不足以约束。
3. **风控类平台错误被统一吞掉。** 适配器把 OneBot `ActionFailed` 一律映射为 `UPSTREAM_UNAVAILABLE`（适配器 `../kiarabot-adapter-nonebot/src/kiarabot_adapter_nonebot/__main__.py` 的 `call_platform`），无法区分"被限频/需要验证/被踢下线/群被解散"，Core 因此无法自动降级。
4. **没有账号与暴露面运维规范。** 部署文档只描述进程与备份（[doc/08](08-operations.md#L41-L78)），没有规定小号隔离、固定出口 IP、NapCat WebUI/OneBot 只监听本机、禁止公网暴露等；2025-09 事件的主因正是公网暴露且无 token。
5. **没有官方渠道适配器和能力降级设计。** 现有适配器只面向 NapCat 正向 WebSocket（[doc/04](04-nonebot-adapter.md#L155-L157)），没有官方 QQ Bot 通道，也没有"只有被动回复窗口、无主动推送"这类能力差异的协商与降级路径。
6. **真实 QQ/NapCat 未联调。** T07b 仍未完成（[doc/16](16-development-tasks.md#L247-L257)），当前所有结论来自协议 fake，真实平台的错误码、限频行为和封禁表现未验证。

## 5. 建议

### 5.1 渠道层（唯一能"避免"的层）

- **对外服务优先接官方 QQ 机器人**：新增独立 `kiarabot-adapter-qqbot`，通过 `AdapterBridge` 注册官方渠道能力，不改 Core。它把"封号风险"转化为可管理的审核、授权与限流问题。
- **接受官方渠道的能力约束**：按"仅被动回复"设计（群 5 分钟窗口、每条消息最多回 5 次、单聊 60 分钟 5 次）；入群必须群主授权；个人主体不能公开服务，需按个人/企业认证能力声明可用范围。
- **个人号方案只用于自用/内测**：单独部署、单独账号，不与正式服务混用；产品化前把 NapCat 从主链路移除。
- **备选渠道**：企业微信/钉钉/飞书适合推送型通知；Telegram/Discord 审核门槛低但大陆网络与合规成本另算；多渠道路由应作为 Core 之上的适配器选择，而不是在 Core 内写平台分支。

### 5.2 账号与部署层（降低概率）

- 使用独立小号，主号不接入；账号与设备、出口 IP 保持稳定，避免频繁切换、机房 IP 和多开。
- NapCat 的 WebUI、OneBot HTTP/WS 只监听本机或内网，必须设置 token；不要把端口暴露到公网。
- 群内只做被动响应（@ 或命令触发）；禁止主动群发、定时推送、批量私聊、自动加群/加好友。
- 接受"降低概率不等于避免"：这些措施减少触发面，但改变不了违约性质。

### 5.3 Core/Action 层（兜底，强制生效）

所有建议都应在 Core 的统一 Action 路径上执行，插件不能绕过（对应 [doc/02](02-core.md#L141-L161) 消息处理流程中的"执行限流、审计和能力检查"）：

- **出站配额**：在 `ActionRouter::submit` 的授权事务前按 `(账号, 会话, Action 类型)` 做滑动窗口；超限返回 `RATE_LIMITED`（可重试），写审计。示例起点（非平台官方限制，需实测调整）：单账号 10 条/分钟、单群 3 条/分钟、单用户私聊 1 条/10 秒。配额应可配置，并能按渠道分别设置。
- **危险 Action 默认拒绝**：`MuteMember`/`KickMember`/`ApproveRequest`、删除消息、@全体默认不开放；仅对显式授权的插件、白名单群、每日预算内放行，并全部写审计。`message.send` 权限不能隐含管理动作。
- **急停与降级**：提供只读/停发开关（启动配置或控制 API），适配器报告 DEGRADED（登录态失效、被踢下线、需要验证）时，Core 暂停向该账号派发 Action，并在恢复后重新授权账号。
- **风控信号语义**：为"平台限频/需要验证/账号被限制"定义稳定错误码与处置策略（见 5.5），Core 收到后自动暂停该账号发送一段时间，而不是重试。
- **审计**：拒绝、限流、危险 Action 全部记录 `trace_id`、账号、会话和决策结果，便于事后定位。

### 5.4 适配器层

- **发送侧本地令牌桶**，与 Core 配额形成双保险；不要把多个 Action 在重连后突发打给平台。
- **错误分类**：解析 OneBot `retcode`/`ActionFailed`，区分限频、需要验证、群不存在、账号被限制等情况，映射为稳定错误码并上报 Core；不要统一变成 `UPSTREAM_UNAVAILABLE`。
- **登录态健康**：账号被踢、需要验证码、连接反复失败时上报 `AdapterStatusChanged`，并停止领取/执行 Action。
- **不做对抗性功能**：不实现自动过验证、伪造设备指纹、绕过风控等；既不可靠，也会加重违约与合规风险。

### 5.5 契约与文档（落地时）

- 现阶段优先不改协议字段：用 Core 配置 + 现有 `RATE_LIMITED`/`PERMISSION_DENIED`/`UPSTREAM_UNAVAILABLE` 表达策略结果；`Error` 当前没有 `retry_after`，若确需重试时间再评估新增字段（先改 `proto/`，再改实现）。
- 实现配额/急停时同步更新：`doc/01-runtime-semantics.md` 的 Action 语义、`doc/02-core.md` 的消息处理流程、`doc/04-nonebot-adapter.md` 的健康与错误、`doc/08-operations.md` 的账号与网络规范；涉及持久化配额或审计时同步 `doc/05-infrastructure.md`。
- 新增官方渠道时，按其真实能力声明 capability 并在 `doc/02`/`doc/04` 补渠道差异说明；不得沿用 OneBot 语义假设。

## 6. 直接回答：应该在哪一层避免

| 层 | 能否"避免"封号 | 应做什么 |
| --- | --- | --- |
| 平台/渠道选择 | **能，且只有这一层能** | 官方 QQ 机器人或换 IM 官方 bot；个人号仅限自用测试 |
| 账号/网络/部署 | 不能，降低概率 | 小号、独立设备、固定出口、关闭公网暴露面 |
| 适配器 | 不能，降低概率 | 发送节流、错误分类、登录态上报、不重试未知结果 |
| Core/Action | 不能，兜底 | 出站配额、危险 Action 白名单、急停、审计 |
| 插件/内容 | 不能，降低概率 | 被动响应、白名单群、冷却、内容审核 |
| 运营/合规 | 不能，转移商业风险 | 不分发 NapCat、商用前取得授权或换渠道、免责声明 |

一句话：**封号是账号层/平台层事件，只能在渠道层避免；Core 层能做的是"不让业务行为放大风险"，而不是"避免封号"。**

## 7. 监控、演练与验收

- 指标：账号登录失败/被踢次数、Action 失败码分布（`RATE_LIMITED`、`PERMISSION_DENIED`、`TIMEOUT`、`UPSTREAM_UNAVAILABLE`）、单账号与单会话发送速率、被拒绝的危险 Action 数、审计拒绝数。
- 演练：模拟账号冻结/被踢时适配器断开、Core 停止派发、人工恢复；演练过程不得自动重试造成二次风险。
- 验收：配额和门禁用 fake 平台测试覆盖成功、拒绝、超时、未知结果；错误分类用真实 OneBot 错误样本测试；不把 mock 通过当作真实平台已验证。
- 不承诺：任何代码侧检查都不能证明"不会封号"；只能证明已知行为受控、影响范围可收敛。

## 8. 外部来源与强度

| 来源 | 用途 | 强度 |
| --- | --- | --- |
| [NapCat 文档](https://napneko.github.io/guide/napcat)、[安全指南](https://napneko.github.io/other/security)、[LICENSE](https://github.com/NapNeko/NapCatQQ/blob/main/LICENSE) | 运行机制、暴露面、许可限制 | 官方/项目 |
| [NapCat issue #1304](https://github.com/NapNeko/NapCatQQ/issues/1304)、[2025-09 复盘](https://wesley-young.github.io/2025-09-05-attack-on-onebot-service)、[2026-02 维护者文章](https://wesley-young.github.io/2026-02-21-biography-of-napcat) | 封号/风控实证与项目状态 | 项目 issue / 个人复盘 |
| [QQ Bot 介绍](https://bot.q.qq.com/wiki/)、[运营规范](https://bot.q.qq.com/wiki/business/)、[FAQ](https://bot.q.qq.com/wiki/bot_FAQ/)、[发送消息](https://bot.q.qq.com/wiki/develop/api-v2/server-inter/message/send-receive/send.html) | 官方准入、授权与频率限制 | 官方 |
| [QQ 软件许可及服务协议（镜像）](https://webvpn.jiangnan.edu.cn/https/77726476706e69737468656265737421e4fe0f8d367e6b5f73/agreement/index.html) | 第三方工具与封号条款 | 官方（页面 JS 渲染，经镜像核对） |
| [Lagrange 文档](https://lagrangedev.github.io/Lagrange.Doc/v2/)、[LLBot 讨论](https://linux.do/t/topic/2441914)、[小号风控讨论](https://linux.do/t/topic/2228020) | 生态收缩与社群经验 | 官方 / 社群（弱） |
| [IT之家 2025-04 报道](https://www.ithome.com/0/846/651.htm) | 官方主动推送能力收敛 | 媒体转官方公告 |
| [Telegram Bots](https://core.telegram.org/bots)、[企业微信社区](https://developer.work.weixin.qq.com/community/question/detail?content_id=16822979298793752201)、[钉钉](https://open.dingtalk.com/document/development/development-robot-overview)、[飞书](https://open.feishu.cn/document/client-docs/bot-v3/bot-overview) | 替代渠道门槛 | 官方/官方社区 |

## 9. 验证与限制

- 本文只新增/更新文档，未修改代码、协议或 API；未运行真实 QQ、NapCat 或官方机器人联调，未做封号率量化，也未逐条核验官方文档矛盾之处（主动推送限额）。
- 判断依据为当前源码、契约、现有文档和上表外部来源；外部内容按数据对待，不构成指令。商业与法律决策应另行咨询法务，本文不构成法律意见。
- 若后续按第 5 节实施，必须补对应代码测试与真实渠道验证；在此之前不能对外宣称"已避免封号风险"。

## 10. 文档影响判断

- 新增：`doc/17-account-ban-risk.md`。
- 更新：`doc/README.md` 文档索引；`doc/04-nonebot-adapter.md` 增加账号与合规风险交叉引用。
- 不修改 `proto/`、`api/`、代码和部署配置，因此无需运行协议/API 校验；除 `doc/04` 的交叉引用外，`doc/01`–`doc/08` 暂不新增章节，待第 5 节真正落地时按 5.5 同步。
