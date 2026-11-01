# 待确认与验收清单

> 更新时间：2026-11-01；既有验收清单核对基线：本地 HEAD `c63ce59`（W7）；本轮架构分析的源码核对点为 `b5f1446`。16 项方向裁定已完成，具体结论见第 3 节。
> 本清单根据仓库文档、契约和关键实现整理；本次未查询远端最新 CI、分支保护或环境状态，未重新运行业务验收。历史测试结果不替代当前 HEAD 验收。
> 跨语言契约以 `proto/` 为准，HTTP/WebSocket 契约以 `api/` 为准。开发记录见[开发台账](../doc/16-development-tasks.md)；历史报告保留当时范围，不代表当前待办。

## 1. 已解决（无需再次裁定）

“已解决”指对应决策或本机实现完成；真实平台、部署及当前 HEAD CI 验收仍见第 2 节。

| 编号 | 原待确认项 | 当前结论与边界 | 依据 |
| --- | --- | --- | --- |
| R01 | messages 频道是否实现 | 已采用方案 C。`message.read` 仅由私有控制配置 `users[].permissions` 显式授予，默认角色均不持有；未授予订阅以 4403 关闭。事件仅含消息 ID、会话 ID、方向，v1 仅 received，不含正文；十分钟/4096 条有界内存重放，不是消息历史或审计，重启不补历史 | [AsyncAPI](../api/asyncapi.yaml)、[台账 W5](../doc/16-development-tasks.md) |
| R02 | Action 是否仅限文本发送 | 方案 B 已完成 React/Delete/Mute/Kick/Approve；Delete 仅限本 bot 已发送消息，Approve 仅好友请求，FriendRequest 已接线。群邀请/加群请求不支持；白名单和预算默认策略见 D01 | [运行语义](../doc/01-runtime-semantics.md)、[台账 W6](../doc/16-development-tasks.md) |
| R03 | ScheduleTask 是否继续实现 | 已 deprecated，调度走 `host.tasks.v1`；旧 Action payload 仍以 `CAPABILITY_NOT_SUPPORTED` 拒绝 | [状态登记](../proto/tests/contract_status.json)、[交接结论](../doc/13-infrastructure-integration-follow-up.md) |
| R04 | 群/私聊同号共享排序键 | 已加入 conversation_type，排序域为带长度编码的四元组；Core schema v6 重键和适配器 outbox DB 1→2 迁移已实现。无法从保留事件归因的旧水位被丢弃，升级后可能出现一次 DEGRADED | [运行语义](../doc/01-runtime-semantics.md)、[台账 W7](../doc/16-development-tasks.md) |
| R05 | Core 缺清理、容量配置、ACK 超时和冲突重试 | W2 已接维护循环、容量配置、ACK 超时及有界冲突重试；命令幂等固定时间保留仍未实现 | [Core 文档](../doc/02-core.md)、[台账 W2](../doc/16-development-tasks.md) |
| R06 | message.send 无速率限制 | 已实现按 Action 类型的滑动窗口及持久化每日预算能力；窗口重启重置，预算可选。内容策略未因此完成 | [配额定义](../core/reference/include/kiarabot/core/application/action_quota.hpp)、[运行语义](../doc/01-runtime-semantics.md) |
| R07 | 面板没有管理动作 | W4 已接入插件、会话及 Action 相关操作；Action 查询后端仍无生产者，UI 接线不等同能力可用 | [面板文档](../doc/07-control-panel.md)、[OpenAPI](../api/openapi.yaml) |
| R08 | SDK 校验与示例缺少补强 | W3 已补清单词表校验、示例和测试；配置应用确认链路此前已闭环。剩余平台能力见第 4 节 | [SDK 边界](../sdk/PLUGIN_SDK_DEPENDENCIES.md)、[开发台账](../doc/16-development-tasks.md) |
| R09 | CI 从未运行 full-build、本地仅用 clang-format 23 | 已有首轮远端 CI：10 个 job 中 8 成功，含 full-build/static analysis；format 和 secret scan 失败，记录称本地已修复。W6/W7 已有 clang-format 16.0.6 和 legacy reader 通过记录；当前 HEAD 全绿仍待 V01 | [CI 首轮记录](execution/2026-10-31-ci-run1.md)、[台账 W6/W7](../doc/16-development-tasks.md) |

## 2. 待验收与执行（已有方向）

| 编号 | 事项 | 当前证据与限制 | 完成条件 |
| --- | --- | --- | --- |
| V01 | 当前 HEAD 远端 CI 全绿 | 首轮基于 bac6fe3，8 成功、2 失败；有本地修复记录，本次未查询后续运行。不能再称“从未远端执行”，也不能认定当前全绿 | 对待验收提交运行全部 job，记录 SHA、run 链接和结果，覆盖 W5–W7 |
| V02 | 恢复 PR/push 自动触发 | workflow 仍仅 workflow_dispatch；既定顺序为手动全绿后恢复 | 恢复并验证 PR/push；同步调整手动输入和基线表达式，不能只取消注释 |
| V03 | Descriptor 独立基线与发布兼容 | 默认 base_ref=origin/main 在 main 手动运行时可能自比；legacy reader 已有通过记录。支持可选 baseline_release，但未核实可用发布资产 | 使用明确的独立基线 SHA 或上一发布版 descriptor，记录来源；自比通过不作为兼容证明 |
| V04 | 分支保护 / required checks | 服务器设置未核实，workflow 文件不能证明已启用保护 | 核对实际 check 名称、矩阵项、required checks 及绕过规则；覆盖构建、协议、full-build、格式、静态分析、API、依赖和密钥扫描 |
| V05 | Docker 与部署恢复（T11b） | W4 骨架已接线，历史记录无 Docker 实机验收；本次未探测主机。当前 Compose 使用 Linux host 网络，范围见 D02 | 在支持该拓扑的 Linux 环境构建 Core/adapter/panel，执行 Compose config/up、迁移、健康、冒烟、备份恢复、schema 校验和回滚，覆盖 Core v6 与适配器独立 DB 升级 |
| V06 | 真实 QQ/NapCat/模型（T07b） | 台账记录环境尚未提供，用户此前暂缓。本机已有真实 C++/Python 混合进程测试，不能笼统称“全部 fake”；平台和模型替身不代表真实渠道验收 | 按[真实环境准备](../tests/acceptance/real-environment.md)配置隔离账号、会话和模型，验证收发、断线、错误分类、限流及扩展 Action 权限/限制；秘密不提交 |
| V07 | TLS、生产反代和证书 | 控制面回环 HTTP，生产 HTTPS/WSS 由同机反代承担 | 验证证书、路由、暴露面、认证、撤销、长连恢复及优雅关闭 |
| V08 | Core/适配器一致备份 | 只恢复一侧存在排序水位不一致风险，当前主要依赖操作约定 | 验证成对备份恢复和单侧恢复故障处理，包括 sequence gap/out_of_order、重投及去重；不宣称端到端零丢失 |
| V09 | 长期稳定性和容量 | 维护、冲突重试、快照上限有测试，不等于生产负载认证；历史 control-api 有时序偶发失败记录 | 压测快照编码、revision 冲突、维护线程、流轮询、限流默认值和迁移锁竞争；SIGKILL 测试不等价断电验收 |
| V10 | 工具链与全量检查 | 历史本机 Python 3.11.17、Homebrew Protobuf/gRPC 与 CI 固定版本不同；本机曾缺 clang-tidy，但首轮 CI 已跑静态分析。历史环境描述不是当前探测结果 | 当前 HEAD 在 CI 固定工具链通过；统一检查若未运行应记录缺项。面板 live 需 C++/venv 前置，记录实际 skip，不沿用旧用例数 |
| V11 | 依赖审计范围 | panel/Python 审计仍 report-only，job 成功不代表无漏洞 | 分诊结果、明确接受项并决定何时阻断；核对适配器依赖审计覆盖 |

执行顺序与命令参考：[CI 流程](../doc/12-ci-cd.md)、[当前 workflow](../.github/workflows/ci.yml)、[运维文档](../doc/08-operations.md)。配置解析成功不代替部署验收。

## 3. 本轮架构裁定：已确认结论

确认日期：2026-11-01。以下十六项为用户已确认的架构方向，尚未实现或验收。本节只记录确认结论，原方案比较、历史建议及未确认方案已移除。

详细方向、首版与当前实现的对应关系见 [本轮架构方向基线](../doc/19-architecture-direction-baseline.md)。该基线为本轮方向结论的完整记录，本节为确认索引。

| 编号 | 用户选择 | 确认结论 |
| --- | --- | --- |
| A01 | C | 控制管理拆为独立服务，通过明确的服务协议与 Core 交互。 |
| A02 | C | 转向独立 Repository 与关系表，替换以全量快照为中心的持久化设计。 |
| A03 | C | 按服务划分独立进程；拆分范围超出构建库拆分，具体服务清单和粒度在详细设计中确定。 |
| A04 | C | 服务内部命令、结果、错误与业务模型完全独立于协议类型；Protobuf/JSON/HTTP 仅用于边界，边界负责校验和双向转换。 |
| A05 | B | 生产显式初始化/迁移；Core 启动默认只验证，开发自动迁移必须显式选择。 |
| A06 | B，并补充要求 | 保留首版设计及有依据的目录与契约来源；在新基线中记录当前实现和本轮演进结论，不替换首版冻结基线。 |
| D02 | C | 首期支持独立服务跨主机分布式部署；具体寻址、认证、加密通信、故障恢复和版本协同另作详细设计。 |
| D03 | C（核心运行管理） | 完善现有插件、登录会话、Action 查询及状态展示闭环；增加账号与适配器绑定、模型配置及密钥更新、群白名单管理、任务查询与取消；消息历史、上下文编辑、插件市场后续排期，用户/角色范围按 D05 执行。 |
| D05 | C（多业务实例与资源范围授权） | 本轮完成多账号、适配器连接、插件运行实例的独立装配与隔离，固定角色配合资源范围授权；自定义角色管理后续排期，撤销生效和在途行为须详细定义，不自动承诺服务多副本高可用。 |
| D07 | C（结合新架构统一整理） | 按用例和新事务设计逐项确定端口保留、改造、替代或移除，检查使用方后迁移；FileStorage 暂不接入生产，不据此新增文件功能；存储和审计不自动拆为远程服务。 |
| D06 | B | 本轮完成最小内容规则与主要平台异常闭环，Core 决定业务策略、适配器转换平台信号、控制服务提供受权处置；异常按实例隔离，具体规则、错误映射及恢复条件须详细设计确认。 |
| D01 | B | 对 Mute/Kick 强制显式启用、目标群白名单和正每日预算，缺配置即拒绝；权限、资源授权及限流同时生效，其他动作保留各自约束，预算及策略更新细则另作详细设计。 |
| D04 | B | 内部成功命令幂等记录至少保留七天，期内同键同内容重放、异内容冲突，重放前重新鉴权；容量不足拒绝新命令，不提前淘汰有效记录，不自动修改控制 API/Host 既有期限。 |
| D08 | C | 增加投递尝试编号与受限续租，ACK 绑定有效尝试，旧 ACK 不得确认新尝试；租约有总时长上限，保留业务幂等，协议/Core/SDK/持久化共同设计升级。 |
| A07 | B | 使用已认证注册元数据并校验、协商有效能力，移除固定适配器类型；区分实例身份、声明/有效能力、在线状态与最后注册信息，声明能力不授予权限，不新增真实平台适配器。 |
| D09 | B | 按“现行契约—实际行为—确认目标—实现与验证”逐项统一；已有契约偏差修复，未定义及新增行为先确认，覆盖 HTTP/WS/RPC、身份、幂等、续租、兼容与关联标识；不自动扩展跨域或方法。 |

本轮只整理裁定文档，不启动代码、协议、数据库或部署改造。本轮 16 项方向裁定已完成；后续详细设计已按授权定稿，具体来源和结论见第 3.1 节，不混淆用户逐项选择与助手自主决定。既有验收与未排期事项保留在本清单相应章节，不因上述架构方向自动完成或取消。

### 3.1 详细设计已确认结论

架构方向 16 项保持不变；以下详细设计不混入方向项计数。DD01、DD02.1–DD02.6、DD03.1–DD03.4 由用户逐项确认；其余按用户“后面的决策由你来定”授权助手定稿。用户另明确开发阶段无需历史兼容包袱。细节见 [设计总览](../doc/20-detailed-architecture-design.md)、[通信方法](../doc/21-service-contract-design.md)、[事务与初始化](../doc/22-transactions-and-migration-design.md)。

| 编号 | 用户选择 | 确认结论 |
| --- | --- | --- |
| DD01 | B | 六类平台服务：控制管理、身份与访问、Core 运行时、插件宿主、任务调度、模型网关；名称为逻辑暂名，具体数据、通信和事务继续逐项评审。 |
| DD02.1 | B | 各业务服务拥有其权威数据，Control 通过协议提交命令和聚合查询；其他服务只持有受约束投影，不直接跨服务读写业务表，物理数据库隔离等另行确认。 |
| DD02.2 | B | 服务专有逻辑数据库、独立运行账户和迁移身份，首期允许共用物理数据库部署；Host/Adapter 保留隔离日志，Control 不强制建库，产品及版本另行确认。 |
| DD02.3 | B | Identity/Runtime/Scheduler/Model 使用各自 PostgreSQL 逻辑库；Host/Adapter 本地执行及去重日志使用 SQLite，不为中央业务维护双生产后端；版本、驱动和 HA 后续确认。 |
| DD02.4 | B | 插件类型/安装/逻辑实例分层稳定标识，执行代次、连接代次、投递尝试及配置版本分离；资源身份不因改名或换机变化，平台原始 ID 保留作用域，编码和兼容迁移另行设计。 |
| DD02.5 | C | 插件存储默认实例私有；安装内共享须显式开启并授权成员及读写范围，禁止跨安装默认共享；共享不扩展其他业务权限，接口、并发控制、配额和迁移细则后续确认。 |
| DD02.6 | B | 停用、逻辑删除、受控物理清理分离；按数据所有者协调撤权及清理，保留幂等/审计/未决恢复依据，恢复不自动复活旧凭据或任务，具体期限和状态后续确认。 |
| DD03.1 | B | 查询/短命令用类型化 RPC，长连接用协议流；恢复型交接采用本地事务 outbox、可恢复派发和接收持久去重，首轮不部署消息中间件，不把持久接受当作全链路完成。 |
| DD03.2 | B | 平台服务使用 mTLS，Identity 签发短期、接收方及操作/资源受限的主体与委托证明，验证直接调用服务绑定；业务层使用内部身份上下文，不直接解析凭据，具体时效及接入引导另行确认。 |
| DD03.3 | C | 高风险/敏感操作在线核验，普通只读明确列出后可有限缓存；内部证明及授权新鲜度上限 60 秒，取更早失效，收到撤销即失效；浏览器保持 REST 即时拒绝及 WS 最迟一秒关闭的既有语义。 |
| DD03.4 | B | 使用最小公共调用上下文及类型化输入输出，区分 request/幂等/operation/交接身份、受理与完成及未知结果，统一内部错误到协议/gRPC/HTTP 稳定映射，具体方法与参数见通信/事务定稿。 |
| DD03.5 | 授权助手采用 B | Identity 基础身份链路定稿：会话、主体/凭据、受限委托、当前授权及失效同步，不扩展自定义角色。 |
| DD03.6 | 授权助手定稿 | Runtime 方法族、账号/插件资源、事件投递、Action、namespace CAS、操作与删除查询；实例事件 fan-out。 |
| DD03.7 | 授权助手定稿 | Host/Scheduler/Model/Control 方法与调用矩阵；所有者提交，Control 不持业务权威。 |
| DD03.8 | 授权助手定稿 | 通信 deadline、有限重试、30秒/总300秒投递租约、心跳、队列、证书与受限证明及流恢复。 |
| DD03.9 | 用户开发阶段要求，助手落实 | 统一目标契约/客户端，无旧版兼容层、历史数据导入、双写或混合版本生产；本轮不实际清理数据。 |
| DD04.1 | 授权助手定稿 | 每服务本地 Repository/UoW，READ COMMITTED+行锁/条件版本/唯一约束，outbox/inbox，无跨库2PC；首期单活动写进程。 |
| DD04.2 | 授权助手定稿 | 事件、水位、目标投递同事务，尝试/租约/有序头原子竞争，过期旧 ACK 不确认新尝试。 |
| DD04.3 | 授权助手定稿 | Action、限流/日预算、收据与审计原子受理，派发前复核，配额保守不自动退还；平台去重及未知结果保留。 |
| DD04.4 | 授权助手定稿 | 任务 occurrence/outbox→Runtime inbox 去重，确认后 COMPLETED；触发交接开始后普通取消冲突。 |
| DD04.5 | 授权助手定稿 | 插件期望、进程执行、注册和配置应用分离；不确定旧执行已停时不跨主机接管。 |
| DD04.6 | 授权助手定稿 | 私有/共享 namespace 的授权、CAS、限额、TTL及收据原子更新；共享不等于外部副作用只执行一次。 |
| DD04.7 | 授权助手定稿 | 模型调用前后分别本地事务，固定配置版本；logical_call_id 防重复供应商调用，未知不伪失败。 |
| DD04.8 | 授权助手定稿 | Identity/凭据/删除的版本与失效原子组，30天非秘密恢复窗、最小墓碑/审计180天及分类清理。 |
| DD04.9 | 用户开发阶段要求，助手落实 | 显式空库初始化、运行只验证、统一组件就绪顺序；无旧开发数据迁移，保留新业务备份/故障恢复。 |

## 4. 延期及未排期事项

T12 继续遵守既有延期决定；D03 已另行明确消息历史、会话上下文编辑、插件市场后续排期。下表其余项为当前缺口或待排期能力；其中 D03/D05 纳入本轮的能力仍未实现，不能继续视为范围未定。其余项不自动视为发布阻塞，也不表示维护者已同意延期。

| 编号 | 能力组 | 当前边界与后续事项 |
| --- | --- | --- |
| F01 | C++ 客户端替换（T12） | 用户明确当前不需要，继续延期；C++ 适配器、C ABI、C++/TS SDK 不能当作当前能力 |
| F02 | 控制面数据和状态 | groups 空页；两个 REST Action 查询无生产者、恒 NOT_FOUND，终态保留/过期查询未闭环；health.updated、插件健康投影未实现。以现行 OpenAPI/AsyncAPI 为准 |
| F03 | 消息/事件扩展 | MessageSent、GroupMemberChanged、群邀请/加群请求；音视频/文件/位置、CHANNEL 和发送 reply 字段未实现。AdapterStatusChanged 登记一致性见第 5 节 |
| F04 | 插件平台 | 动态订阅、依赖解析、发现/多实例、自动重启、日志/健康通道、会话服务、权限查询、流式模型、native/wasm 待排期；受管进程参数透传等见 SDK 边界说明 |
| F05 | SDK、工具和示例 | 专用 FakeEvent/FakeMessageService/FakeModelService/FakeStorage/FakeScheduler、脚手架及部分高级封装未实现；已有 FakeHost，不能说无夹具。新示例受管进程部署验收仍需补充 |
| F06 | 消息历史和面板体验 | 无 REST 消息历史、发送方向事件及消息页面；axe/i18n、服务端过滤、过滤导出、视觉走查待排期；新增业务入口先补契约 |
| F07 | 基础设施与观测接线 | 时区数据启动预检未接组合根；快照拒绝、日志失败、session_degraded/sequence_gap 未完整导出健康/指标/告警。每插件任务上限仍固定 1024；PostgreSQL 已按 DD02.3 纳入目标设计但未实现，Redis 仍为未来项 |
| F08 | 发布自动化 | Docker 构建 CI、staging/production CD、镜像发布及生产运维流程待建设；现有 workflow 不等于自动发布系统 |
| F09 | 可选本机清理 | 临时工作树/build 不阻塞发布；先检查当前 worktree 和未提交内容，不按历史路径批量删除 |

边界依据：[SDK 依赖说明](../sdk/PLUGIN_SDK_DEPENDENCIES.md)、[基础设施文档](../doc/05-infrastructure.md)、[控制 API](../doc/06-control-api.md)、[面板文档](../doc/07-control-panel.md)、[契约状态登记](../proto/tests/contract_status.json)。

## 5. 待同步的登记与历史说明

以下是既有一致性工作，不以旧登记反推实现不存在，也不直接翻转未完成端到端核对的状态。

- 状态登记仍将 AdapterStatusChanged 及 envelope 标为 not_implemented，但适配器 [status.py](../adapters/nonebot/src/kiarabot_adapter_nonebot/status.py) 已生产且有测试；需核对 Core 消费链路后同步消息、字段、事件登记和说明。
- `semantic.transport.graceful_shutdown` 仍为 not_implemented，而 W3 已有实现和验收记录；背压 retryable 等登记也需与后续修复逐项核对。更新时运行状态、语义及契约门禁，不能仅凭搜索翻转。
- [doc/06](../doc/06-control-api.md) 仍称 1013 未登记，当前 AsyncAPI 已包含；其他传输语义按 D09 收口。
- [doc/11](../doc/11-toolchain.md) 的“待 CI 配合 full 覆盖”与当前 full-build job、首轮 CI 记录不一致，应说明 dev/full 实际覆盖。
- [doc/12](../doc/12-ci-cd.md) 等旧部署说明需与 W4 接线记录对照；保留“未实机验收”，避免笼统称没有接线。
- [doc/17](../doc/17-account-ban-risk.md) 的旧问题段落已有 W6 更新注记；Action、配额和授权以更新内容及实现为准，不再列为全部未实现。
- [历史模块报告](modules/)、[独立复核](verification.md)、[W1–W4 总结](execution/final-summary.md) 的完成率、缺口数、用例数和环境/远端状态是当时快照，不作为当前统计。两份 doc/13 也已明确为历史材料。
- [doc/18](../doc/18-architecture-baseline.md) 受保护，不随上述同步修改；目录、端口及管理范围差异仅报告人类维护者。

## 6. 本次文档影响与检查

- 修改范围：第 3.1 节记录用户确认与授权助手定稿的全部详细设计索引；本清单第 3 节保留十六项方向确认结论（含 D09=B 契约与实现收口），删除原方案比较和未确认方案；原有已解决、待验收、未排期及历史登记说明保留。原文末架构补充迁入新基线。
- 详细设计完整记录在 `doc/20-detailed-architecture-design.md`、`doc/21-service-contract-design.md`、`doc/22-transactions-and-migration-design.md`；总览位于 `doc/20-detailed-architecture-design.md`，DD01、DD02、DD03、DD04 本轮范围已定稿，未实现/未验收；新增 `doc/19-architecture-direction-baseline.md`，记录本轮十六项已确认方向、当前实现对照和首版关系；更新 `doc/README.md` 的文档索引。
- `doc/18-architecture-baseline.md`、AGENTS.md 及保护清单保持不变；新文档不自动加入受保护清单。
- `proto/`、`api/`、模块实现说明本轮无需同步：本次仅记录目标方向，未定义新线协议或改变现行运行行为。实施时须先明确契约并同步直接受影响的模块文档。
- 检查：`git diff --check`、修改文档的本地 Markdown 链接与标题锚点检查、确认编号与受保护文件检查；结果见交付说明。
- 未运行构建、业务测试、格式/静态分析和协议生成：本轮无代码或契约变更，不以历史结果冒充本次验证。
