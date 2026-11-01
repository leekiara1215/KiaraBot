# KiaraBot 新架构分步实施计划

- 版本：v1.0
- 日期：2026-11-01
- 状态：计划已整理；P00–P14 均未开始，不代表实现或测试通过
- 依据：[方向基线](19-architecture-direction-baseline.md)、[设计总览](20-detailed-architecture-design.md)、[通信方法](21-service-contract-design.md)、[事务与初始化](22-transactions-and-migration-design.md)
- 当前授权：用户已要求在现有 worktree 新开多个对话并行开发、各模块独立小步提交，由主对话统一审查集成；不执行未授权旧数据清理或推送/部署

## 1. 实施原则与完成口径

按“契约/接口 → 内部模型与事务 → 业务闭环 → 管理与部署”实施。六类平台服务分别是 Control、Identity、Runtime、Plugin Host、Scheduler、Model；Adapter 与插件业务实例仍独立运行。

采用统一目标契约和空库初始化，不做旧协议兼容层、旧快照导入、双写或混合版本生产运行。可复用已经验证的算法、平台转换、错误分类与测试场景，但必须适配目标身份/事务语义。删除旧代码前先迁移仍有效的行为断言，不用删除测试掩盖回归。

每阶段必须交付：允许范围、输入输出、稳定错误/状态、文档影响判断、实际命令/结果、未运行项及原因。跨模块阶段拆为表中子任务，先合并契约再改各自实现，不能授权一个模块直接修改另一个模块内部数据。

P00–P02 在同一集成工作分支形成目标构建基线；P01 的契约生成通过不等于旧消费者或全仓通过。P02 建立新目标编译图后，每阶段要求全目标构建与已实现测试通过；未实现方法显式拒绝，未配置依赖不报 ready，不用 mock/skip 冒充生产功能。阶段门槛与最终发布门槛分开记录。

不安排虚构日期或工期。每个阶段可以拆成若干可审查提交，阶段未达到退出条件不能仅凭提交/文档完成打勾。

## 2. 总体顺序与依赖

| 阶段 | 交付目标 | 前置 | 状态 |
| --- | --- | --- | --- |
| P00 | 实施规格、依赖锁、文件归属与测试入口 | 定稿设计 | 进行中：规格/依赖锁已提交，完整环境验收未关闭 |
| P01 | 统一目标 proto/API 与内部公共接口 | P00 | 首轮契约/内层接口已集成、独立验收通过；完整门禁未关闭 |
| P02 | 六进程、分层构建与阶段测试门禁 | P01 | 未开始 |
| P03 | 私有 PostgreSQL 库、UoW、收据、outbox/inbox、显式初始化 | P02 | 未开始 |
| P04 | Identity 全链路与 Control 最小认证入口 | P03 | 未开始 |
| P05 | Runtime 账号/插件资源、代次与注册 | P04 | 未开始 |
| P06 | Adapter 事件→Runtime→Plugin，尝试与续租 | P05 | 未开始 |
| P07 | SDK Action→Runtime→Adapter→平台替身闭环 | P06 | 未开始 |
| P08 | 插件宿主、期望状态与配置应用闭环 | P05、P06 | 未开始 |
| P09 | 私有/共享 namespace、CAS、配额与收据 | P05、P03 | 未开始 |
| P10 | Scheduler 一次性任务及可靠触发 | P06、P03、P04 | 未开始 |
| P11 | Model 配置/密钥及调用前后记账 | P05、P03、P04 | 未开始 |
| P12 | Control 与 Panel 核心运行管理全部接线 | P07–P11 | 未开始 |
| P13 | 生命周期协调、分类清理与容量观测 | P07–P12 | 未开始 |
| P14 | 跨主机部署、故障矩阵、恢复及统一发布验收 | P13 | 未开始 |

推荐单人执行顺序为表中顺序。多人实施时，P08/P09/P10/P11 可在公共契约与相应前置通过后分别推进；P12 页面骨架可提前开发，但真实能力完成必须等待业务所有者。并行对话与工作区分配见 [执行记录](../reports/execution/2026-11-01-architecture-parallel.md)；按依赖逐波启动，不让业务线程自行发明共享协议。

### 里程碑

| 里程碑 | 阶段 | 可演示结果 | 尚不能宣称 |
| --- | --- | --- | --- |
| M1 可信运行基础 | P00–P05 | 六进程构建、私有库、真实身份与资源注册 | 完整消息机器人或管理平台 |
| M2 消息业务闭环 | P06–P07 | 真实 gRPC/PG/SDK/Adapter 与协议平台替身收发、拒绝与恢复 | 真实 QQ 或生产验收 |
| M3 核心能力齐备 | P08–P13 | 宿主、配置、共享存储、任务、模型、面板管理及清理 | 高可用、无限容量或自动故障接管 |
| M4 可交付部署 | P14 | 跨主机身份、故障恢复、成对备份演练及目标检查全通过 | 未执行的真实平台/外部模型联调 |

## 3. 文件归属与依赖版本

### 3.1 目标目录约定

尽量沿用模块根，不为了进程拆分搬动全部仓库。P00 固定具体文件清单；以下为新文件建议位置，当前尚未创建。

| 所有者 | 允许的主要实现范围 | 禁止事项 |
| --- | --- | --- |
| 契约 | `proto/kiarabot/v1/`、`proto/tests/`、`api/`、对应契约文档 | 写业务实现、手编生成代码 |
| Core 应用/领域 | `core/include/kiarabot/core/{domain,application,ports}/`、`core/src/{domain,application}/`，按 identity/runtime/scheduler/model 子域分组 | SQL、HTTP/JSON/Protobuf 类型进入内层、平台 SDK |
| Core 传输/运行支撑 | `core/src/runtime/`、对应 runtime 头文件、`core/app/` 组合根 | 将传输类型反向泄漏应用层、任意远程 shell |
| 基础设施 | `infrastructure/include/`、`infrastructure/src/`、`infrastructure/tests/`，新增 PG/证书/Provider 等适配实现 | 决定业务权限、读写他服务逻辑库 |
| Control | `control/api/`，新增独立进程入口与所有者 RPC 适配 | 直连业务库、复制权威配置/授权 |
| SDK/插件 | `sdk/python/`、批准的 `plugins/` 示例 | 直连数据库/平台，绕过认证实例派生 owner |
| Adapter | `adapters/nonebot/` | 插件业务、中央数据表直接访问 |
| Panel | `panel/` | 后端权限/路由/预算逻辑 |
| 工具与运维 | 根 CMake、`cmake/`、`scripts/`、`deploy/`、`.github/workflows/`、锁文件及验收夹具 | 私自放宽业务语义、提交秘密或本地路径 |

目标二进制采用 `kiarabot-control`、`kiarabot-identity`、`kiarabot-runtime`、`kiarabot-plugin-host`、`kiarabot-scheduler`、`kiarabot-model`；组合根是允许装配外层实现的位置，业务源码仍只依赖内层端口。初始化命令使用按服务选择 schema 的显式入口，不混入 daemon 自动升级。

全阶段禁止修改 `doc/18-architecture-baseline.md` 或保护清单，不修改无关工作区内容，不提交 `build/`、`.venv/`、生成 `.pb.*` 或秘密。

### 3.2 版本输入

沿用仓库 C++20、CMake>=3.24、GCC>=13/Clang>=16/AppleClang>=15 及当前锁定的 Python/Node/Protobuf/gRPC 工具链作为起点，具体已记录版本见 [doc/11](11-toolchain.md) 和现有 lock 文件，不能把不同本机版本说成同一验收环境。

P00 必须输出一份可重复的目标依赖清单：PostgreSQL 服务端与客户端、PG 驱动、JWT/JWS 库、TLS/密码库及 CA 工具、现有 gRPC/Protobuf、SQLite、Boost、libcurl 的确切版本/来源/hash。只选一套中央 PG 生产适配，不加入 SQLite 中央业务备用实现；本地 SQLite 仅 Host/Adapter 日志。JWT 采用成熟库的固定 Ed25519 验证能力，不自行实现密码算法。

本计划不凭未探测的机器环境臆造补丁版本，也不要求保持旧开发锁文件不变；版本锁是 P00 的退出产物，后续阶段继承该锁，升级须单独记录并验证。

## 4. 验证命令与门禁配置

以下命令为实施时执行清单，本轮未执行。V1–V4 使用仓库现有入口；P02/P03 必须按目标结构扩展覆盖。V5 的 `pNN` 是计划新增 CTest label（如 p03/p07），目前尚不存在，缺 label/无测试必须失败，不可误报通过。

### V0：文档和变更范围（所有阶段）

```bash
git diff --check
git diff --name-only
```

另检查相对链接/锚点、契约/错误/状态一致性与受保护文件 diff，记录新增未跟踪文件，不能只看 tracked diff。

### V1：协议/API

```bash
./proto/tests/check_contracts.sh
./scripts/quality/check-openapi.sh
./scripts/quality/check-asyncapi.sh
```

P01 需同步更新脚本与目标 fixture，生成 C++/Python、descriptor 及 API 类型；不拿旧开发快照作必须兼容基线，不以 descriptor 自比证明兼容。

### V2：C++ 与边界

```bash
cmake --preset full
cmake --build --preset full
ctest --preset full --no-tests=error
python3 scripts/quality/check-boundaries.py
python3 scripts/quality/tests/boundaries_test.py
./scripts/quality/check-format.sh
./scripts/quality/check-tidy.sh
```

full 构建必须产生涵盖六进程和新基础设施的编译数据库。新增源码不在 tidy/边界覆盖中直接阻断，而非沿用旧目录白名单漏检。

### V3：Python

```bash
uv sync --project sdk/python --locked
sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py
sdk/python/.venv/bin/python -m pytest -q sdk/python/tests
uv sync --project adapters/nonebot --locked
adapters/nonebot/.venv/bin/python adapters/nonebot/tools/generate_proto.py
adapters/nonebot/.venv/bin/python -m pytest -q adapters/nonebot/tests
```

插件示例测试采用 SDK 环境、明确项目配置和目标测试目录。Python 小版本由 P00 锁定；不把“依赖未安装”作为跳过集成测试的成功条件。

### V4：Panel 与全链路

```bash
npm --prefix panel ci
npm --prefix panel run check:contracts
npm --prefix panel run lint
npm --prefix panel test
npm --prefix panel run build
bash tests/acceptance/run.sh
./scripts/check-toolchain.sh
```

验收入口在实施中升级为目标六服务/PG/证书环境；不可继续用旧单 Core/SQLite fixture 却宣称新架构通过。真实 QQ/付费模型不是多数核心测试前提，使用协议平台替身和本机模型假服务。

### V5：阶段测试（P02 起新增 label）

```bash
ctest --preset full -L p03 --no-tests=error
```

每个阶段注册对应 `pNN`、`unit`/`integration`/`fault` label 并在该阶段实际使用。真实 PG 集成 fixture 只连接专用临时测试环境：创建测试专属数据库、初始化、分配角色、运行、销毁，并拒绝生产库/非测试白名单连接。测试网络与额外权限按环境审批处理，不试图绕过。

PG Repository/COMMIT/锁/并发测试缺少真实 PG 时失败并报告原因，不回退内存/SQLite 冒充。生产进程不能依赖测试用无限信任、固定 token 或关闭 TLS 验证。

## 5. 分阶段任务

### P00：冻结实施规格与环境

- **目标/范围**：工具链、模块边界和任务规格；允许 CMake/依赖锁/CI 检查配置及直接相关文档，禁止先改业务实现。读取目标设计与各模块文档，盘点可复用代码和测试不变量。
- **输入→输出**：定稿设计→具体文件清单、六进程目标表、依赖精确锁、权限/错误/状态/限额矩阵、测试环境与 label 约定、旧目标删除清单。把文档中的逻辑方法转为待定义的唯一契约条目。
- **错误/状态**：工具缺失、版本冲突、目标 schema 或角色映射不明确必须显式失败/列任务，不得默选不兼容版本或吞掉检查。
- **文档**：更新 `doc/00-boundaries.md`、`doc/11-toolchain.md`、`doc/12-ci-cd.md` 的目标实施约束；`doc/16` 仅登记新计划，不覆盖旧执行历史。无接口变化时无需生成契约。
- **验证/退出**：V0、版本/依赖可取得性和锁解析；阶段规格能直接按任务模板下发，无缺失的驱动/证书/测试环境前置。该阶段通过不代表新工具链全量构建已过。

### P01：统一目标契约和内部接口

- **目标/范围**：协议/API/内层公共接口。P01a 只改 `proto/`、`api/`、契约测试；P01b 只改 Core domain/ports 的独立值类型和端口。禁止连接真实平台或先写 SQL 实现。
- **输入→输出**：方法表→Identity/Runtime/Host/Scheduler/Model 管理契约，双向流注册、实例/代次、ACK/续租、operation、namespace scope/CAS、错误/恢复上下文、健康/生命周期和 REST/WS schema；内部模型不引用生成类型或 JSON。
- **错误/状态**：逐方法列出正常、输入拒绝、认证/授权、冲突、限流、超时、依赖及未知结果；HTTP/gRPC 映射与字段类型不能只写自由文本。新状态先登记，不以未实现状态冒充已有生产者。
- **文档**：`proto/README.md`、`doc/01-contracts.md`、`doc/01-runtime-semantics.md`、`doc/06-control-api.md`；直接受影响模块增加目标接口章节，标明未实现。
- **验证/退出**：V0/V1，C++/Python 生成、round-trip、目标 descriptor/枚举/状态登记；方法有调用者、owner、权限、幂等及 deadline，必需 generation 不会因缺省字段静默放行。此阶段仅协议门禁，不宣称旧消费者全仓通过。

### P02：六进程与分层构建

- **目标/范围**：根/模块 CMake、`core/app/`、`control/api/` 入口、传输适配与质量脚本；每个入口依赖自己的内层库和外层组合根，不在 app 层实现业务。
- **输入→输出**：P01 接口→六个可构建/启停进程、独立 application 库、协议转换边界、配置校验、live/ready/优雅关闭、结构化错误，目标编译数据库与测试 labels。
- **错误/状态**：live 只表达进程存活；未初始化库/身份/必要依赖时 ready=false，未实现方法明确拒绝；参数/证书配置错误非零退出，不输出假 ready。
- **文档**：`doc/00`、`doc/02`、`doc/06`、`doc/08`、`doc/11`、`doc/12-ci-cd`；禁止改受保护 doc/18。移除旧生产链接目标与失效测试 wiring，保留待重建行为案例清单。
- **验证/退出**：V0/V2，六入口独立生命周期测试与内层 include/link 门禁，单进程故障不要求其他进程退出。此后全构建与所有已实现测试持续通过；未实现业务仍不 ready。

### P03：PostgreSQL、UoW、可靠交接与初始化

- **目标/范围**：P03a Core ports/UoW 接口；P03b `infrastructure/` 的 PG、初始化、schema、Repository、收据、outbox/inbox；P03c 测试专用 PG fixture/CI。禁止基础设施决定业务授权。
- **输入→输出**：关系模型→Identity/Runtime/Scheduler/Model 四个私有逻辑库、运行/迁移身份隔离、显式空库初始化、schema 校验；统一 commit/rollback、条件版本、唯一约束、claim、交接有效期/去重及操作记录基础。
- **错误/状态**：区分不存在、冲突、不可用、损坏；COMMIT 回执丢失为未知，不伪装回滚；普通 outbox 满拒绝新增，已受理不丢；撤销可合并版本水位不被普通队列挤掉。
- **文档**：`doc/02`、`doc/05`、`doc/08`、运行语义、工具链；新初始化命令输入/退出码、数据库权限及测试拒绝生产连接必须写清。
- **验证/退出**：V0/V2/V5(p03)，真实 PG 测试隔离/锁/唯一/并发/提交未知/重启、其他服务账户跨库拒绝、运行账户 DDL 拒绝、重复/半初始化失败。没有真实 PG 不能关闭该阶段。

### P04：Identity 与最小认证入口

- **目标/范围**：P04a Core identity domain/application 与传输；P04b 身份 Repository；P04c Control 最小 Login/Refresh/Me/Sessions/Revoke 映射；P04d 受控证书/管理员 bootstrap fixture。禁止扩建自定义角色/完整用户管理。
- **输入→输出**：主体/凭据/会话与固定角色资源授予，mTLS、短期受限委托、在线授权、验签材料与失效流；登录和撤销可经真实 Control→Identity 演示。
- **错误/状态**：错误签名/audience/服务绑定/范围/过期拒绝；Identity 不可用不当成无权限或缓存成功；浏览器 REST 当前校验、WS 最迟一秒失效，内部证明/普通读总预算不超60秒。
- **文档**：`doc/01`、`doc/02`、`doc/06`、`doc/08`；凭据轮换/一次性交付/加密收据、引导关闭、时钟异常和撤销分类同步。
- **验证/退出**：V0/V1/V2/V5(p04)，真实 Identity 数据库、刷新并发与响应丢失、撤销丢通知、未知验签键、证书绑定、依赖分区/重启缓存；Control 认证 HTTP 小闭环。安全材料不能进日志/仓库。

### P05：Runtime 资源、绑定和当前代次

- **目标/范围**：Runtime domain/application、对应 Repository/传输；契约修改只补 P01 发现的遗漏并先审查。SDK/Adapter 实现留 P06。
- **输入→输出**：账号/适配器绑定、插件类型/安装/实例、期望状态/配置版本、host登记，认证注册、有效能力与执行/连接代次，operation与查询。
- **错误/状态**：声明能力不授予权限；不匹配主体/账号/安装/旧代次拒绝；同资源预期版本冲突不覆盖；同实例不同时接受两个活动写执行，离线不代表已停止。
- **文档**：`doc/02`、`doc/03`、`doc/04`、`doc/06`、运行语义；明确身份与资源唯一来源以及当前状态/历史观察。
- **验证/退出**：V0/V1/V2/V5(p05)，两模拟适配器、多安装/实例、注册并发、换连接旧消息、禁用/删除 gate、重连元数据变化；API查询实际权威数据，不能固定 nonebot2。

### P06：事件、投递、SDK 与 Adapter 入站闭环

- **目标/范围**：P06a Runtime 事件/水位/目标/尝试事务；P06b Python SDK 注册、配置门禁、事件处理/ACK/续租；P06c Adapter mapper、SQLite outbox与认证流；P06d 集成夹具。每项只改自己的实现模块。
- **输入→输出**：平台事件→Adapter 持久上报→Runtime PG 同事务入库/目标→插件 SDK→ACK/续租；先用外部自管插件验证，不假装已实现 Host。
- **错误/状态**：同 ID 异内容冲突、旧 sequence 拒绝、跳号 degraded、容量不足背压；30秒租约/总300秒续租、旧ACK不确认新尝试、5次重试与死信；fan-out 到每个获准活动实例。
- **文档**：`doc/01-contracts`、运行语义、`doc/02`、`doc/03`、`doc/04`、SDK/Adapter README和验收文档。
- **验证/退出**：V0/V1/V2/V3/V5(p06)，PG/SQLite及真实 gRPC 混合进程验证重复事件不增目标、有序头、ACK/续租与超时竞争、确认丢失重投、重启。得到首条可恢复事件链路。

### P07：Action、配额与平台执行闭环

- **目标/范围**：P07a Runtime Action/策略/预算事务；P07b SDK submit/query/cancel；P07c Adapter 平台执行日志与当前批准动作；P07d 协议平台替身验收。无其他平台扩展。
- **输入→输出**：插件Action→Runtime原子受理→Adapter去重执行→结果→同一权威记录查询。覆盖文本发送、回应、bot自撤回、Mute/Kick、好友审批；ScheduleTask继续拒绝。
- **错误/状态**：Mute/Kick缺开关/白名单/正预算拒绝；同键不重复占额；取消与dispatched竞争；平台结果未知不自动再执行，EXPIRED不被迟到结果覆盖；控制查询须显式scope，不能admin隐式越权。
- **文档**：运行语义、`doc/02`、`doc/03`、`doc/04`、`doc/06`、`doc/17`；配额保守不自动退额及重启不重置写清。
- **验证/退出**：V0–V3/V5(p07)及目标闭环；并发预算不超额、提交失败零平台调用、执行后丢结果/两侧重启不重复、取消与到期、旧Adapter执行不确定不接管。达到 M2。

### P08：Plugin Host、期望状态与配置应用

- **目标/范围**：P08a Host进程控制与SQLite日志；P08b Runtime desired/outbox/执行观察；P08c SDK配置回调ACK与最小秘密交付；不实现自动插件包下载/市场。
- **输入→输出**：管理期望→Host持久命令→受控进程组→插件注册/配置应用→真实状态。包摘要、启动参数与环境白名单可验证。
- **错误/状态**：启动中宕机不能重复spawn；PID复用不误认旧进程；旧配置ACK不盖新版本；Host离线不显示已停；未确定旧执行已停不跨主机重启同一实例。
- **文档**：`doc/02`、`doc/03`、`doc/06`、`doc/08`与部署配置说明；期望/进程/注册/配置应用四层分开。
- **验证/退出**：V0/V1/V2/V3/V5(p08)，start前后kill、重复命令、Host重启认领/未知、包/参数拒绝、配置版本冲突和重连；10秒停止宽限及超时结果符合设计。

### P09：Namespace、共享、CAS 与限额

- **目标/范围**：P09a Runtime storage domain/application；P09b namespace Repository/事务；P09c SDK私有/共享scope与CAS；P09d 获授权管理入口。禁止插件直接数据库访问。
- **输入→输出**：稳定实例私有空间、显式安装共享成员、版本写/CAS、TTL、安装聚合配额、七天成功收据。
- **错误/状态**：未授权共享拒绝；撤销与并发写竞争同gate；同键跨实例不串收据，重放不续TTL；1MiB/1024键等默认与安装8MiB/8192键共同限制，不因加实例绕过。
- **文档**：运行语义、`doc/02`、`doc/03`、`doc/05`、`doc/06`及SDK文档；逻辑所有权与托管职责清楚。
- **验证/退出**：V0/V1/V2/V3/V5(p09)，真实PG两实例CAS、配额并发、成员撤销、TTL、幂等冲突、实例重启/换宿主保留数据；不宣称CAS保证外部只执行一次。

### P10：Scheduler 与可靠 TimerTriggered

- **目标/范围**：P10a Scheduler domain/application与PG任务/occurrence；P10b Runtime接收交接；P10c SDK任务方法；P10d IANA时区适配。禁止直接调插件或增加cron。
- **输入→输出**：一次性UTC/当地时间任务→领取与outbox→Runtime去重入队→Scheduler确认COMPLETED；任务查询/取消可用。
- **错误/状态**：歧义/不存在当地时间拒绝；owner从可信上下文派生；取消在触发交接开始后返回冲突；确认丢失同occurrence恢复，COMPLETED只代表持久入队。
- **文档**：运行语义、`doc/02`、`doc/03`、`doc/05`、`doc/06`、`doc/08`；取消窗口和1024实例/8192安装默认限制。
- **验证/退出**：V0/V1/V2/V3/V5(p10)，发送前后宕机、入队后确认丢失、取消/到期竞争、DST、重复恢复、目标停用；同occurrence只建一次事件与目标。

### P11：Model 网关、配置密钥与计费边界

- **目标/范围**：P11a Model应用/PG调用记录；P11b infrastructure HTTP Provider/密钥适配；P11c Runtime远程端口、SDK模型方法；P11d管理配置入口。禁止流式、工具调用和消息历史扩展。
- **输入→输出**：受限主体/模型别名→先提交意图→调用Provider→记录已知结果或UNKNOWN；模型配置/密钥更新只影响后续请求。
- **错误/状态**：意图提交失败不调用；上游429/超时/畸形响应稳定映射；同call_id不重复调用；已调用但响应/第二次提交丢失保持未知，不自动换供应商/重计费。
- **文档**：运行语义、`doc/02`、`doc/03`、`doc/05`、`doc/06`、`doc/08`；密钥根与密文分离、正文不记日志、30秒上限及当前纯文本限制。
- **验证/退出**：V0/V1/V2/V3/V5(p11)，仅本地HTTP假服务验证计费次数、第一/第二事务故障、配置轮换固定版本、断线、未知恢复、敏感查询授权；真实模型不是该阶段前置。

### P12：Control 与 Panel 核心运行管理

- **目标/范围**：P12a Control所有者RPC适配/分页/WS；P12b Panel类型与功能页；P12c全链路夹具。不允许Control跨库查询或复刻授权规则。
- **输入→输出**：登录/会话、账号绑定、插件启停/配置、模型密钥更新、白名单、任务查询取消、Action元数据、日志审计/健康管理；来自真实权威服务，默认live目标环境。
- **错误/状态**：区分受理/实际完成/未知，部分依赖不可用可表达；operation按scope查询，密钥不回显；WS授权/resync、REST会话撤销、CORS/405/Allow与目标契约一致。
- **文档**：`api/`、`doc/06`、`doc/07`、`doc/08`及面板/控制README，API类型和权限词表不能手抄漂移。
- **验证/退出**：V0/V1/V2/V4/V5(p12)，真实服务HTTP/WS+Panel live测试覆盖所有批准写入口、拒绝、版本冲突、状态进度、断流补快照。消息历史/上下文编辑/市场/自定义角色不加入本轮。

### P13：生命周期协调、清理与可观测性

- **目标/范围**：各业务所有者内的删除/恢复/清理用例、Identity失效收敛、Host/Adapter收尾、Control进度展示、运维指标。分子任务在各自模块实现，禁止跨库级联。
- **输入→输出**：停用gate→逻辑删除→参与者回执→受控清理；30天非秘密恢复、180天最小墓碑/必要审计、各自收据期和60秒清理批次；容量/租约/积压/未知结果可观测。
- **错误/状态**：依赖离线Blocked不伪全局完成；恢复不复活旧凭据/任务；过期TTL不续期；已受理/未决记录不被容量淘汰；备份中历史不自动激活资源。
- **文档**：运行语义、全部直接受影响模块与`doc/08`；日志仅标识/状态，告警含操作引用不含秘密。
- **验证/退出**：V0/V1/V2/V3/V4/V5(p13)，重复删除/恢复并发、共享成员删除不清其他人数据、满盘/清理失败、失效outbox满、未决副作用、保留期与配额；达到M3。

### P14：部署、故障与统一交付

- **目标/范围**：`deploy/`、`scripts/deploy/`、CI、目标验收；确因故障测试暴露的问题回归所属模块修复，不能由部署脚本绕过业务门禁。
- **输入→输出**：统一目标镜像/锁/契约→私有网络、PG逻辑库、证书引导/轮换、初始化、六进程健康、Host/Adapter部署、统一版本客户端与发布证据。
- **错误/状态**：库版本/证书/时钟异常阻止ready；不自动清旧数据或误恢复一侧日志；单活动进程锁丢失停止副作用；断网不无限离线放行，不宣称HA。
- **文档**：`doc/08`、`doc/11`、`doc/12-ci-cd`、README/启动示例和目标验收入口；记录目标SHA、镜像digest、schema/配置版本、命令日志及未验收项。
- **验证/退出**：完整V0–V5和统一工具链；至少两个独立Linux主机/VM验证真实网络分区/TLS/撤销/重连，全部事务故障矩阵、数据库+Host/Adapter协调恢复，RPO24h/RTO4h作为演练目标记录实测。真实QQ/真实模型有环境才单独执行；缺失如实列未验收，不能由假平台结果替代。

## 6. 最终验收清单

- [ ] 六进程真实配置与依赖就绪，不用stub/关闭TLS替代生产实现。
- [ ] 所有跨边界契约、错误、权限、状态在唯一来源登记，生成/解析/跨语言验证通过。
- [ ] 内层无Protobuf/JSON/HTTP/SQL依赖，数据库私有权限和运行/迁移身份实际验证。
- [ ] 真实PG事务测试通过，四类业务库及两侧SQLite日志恢复通过。
- [ ] 消息→插件→Action、配置应用、CAS、定时触发、模型未知结果、生命周期全部闭环。
- [ ] 60秒授权上界、高风险在线核验、浏览器REST/WS撤销、秘密不泄露均有故障测试。
- [ ] 旧协议桥接/快照中心/双写路径移除；旧行为不变量已迁为目标测试，不通过删断言凑绿。
- [ ] full构建、CTest、格式、tidy、边界、API/协议、Python与Panel门禁完整覆盖新增目录，无缺前置导致的静默skip。
- [ ] 跨主机及协调备份恢复演练有实测证据；真实平台、远端CI和部署状态分别记录。
- [ ] 每阶段文档同步与检查报告齐全，受保护首版基线及保护清单未改。

## 7. 本轮计划文档的影响与验证

本轮仅新增本计划并更新 `doc/README.md` 导航；已有设计、裁定及历史执行记录保持。代码、proto/API、SQL、依赖、测试和部署均未修改，不执行数据库/文件清理。

契约与模块文档无需因计划本身改变运行说明；后续实施每步按本计划同步。文档验证为格式差异、相对链接/锚点、阶段编号/依赖图、每步范围与I/O/错误/检查/验收字段完整性。构建与测试未运行：当前仅整理计划，不能把以上命令列为实际检查结果。
