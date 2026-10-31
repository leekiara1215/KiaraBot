# 分层开发任务与实施台账

## 当前范围和执行约束

遵循用户最新要求：保持 C++20 Core、本仓库维护的 Python NoneBot2 适配器（`adapters/nonebot/`）、Python 进程外 SDK/插件和 TypeScript 面板。适配器不再作为独立仓库维护，后续可再考虑拆成单独插件；纯 C++ 替换只是未来方向，T12 延期。本台账将原 12 个工作包拆成 23 个当前任务和 1 个延期任务；状态只有验收后才能更新，不以协议或占位实现计完成。

共同规则：每个任务只修改列出的目录及直接相关文档，禁止修改其余模块内部实现；跨层任务再按协议、各模块实现、集成测试拆提交。C++20/CMake 3.24+，Protobuf 25+/gRPC 1.60+，Python 适配器及 SDK 3.11+；面板依赖使用现有锁文件。新增依赖先固定版本，不顺手升级。拒绝、超时、依赖故障使用现有稳定错误；尚未冻结的跨边界字段先完成契约任务，不能用自由文本代替。

文档影响：每项列出必须同步的文档；未改变的协议/API 不修改。所有代码任务运行相应构建/测试/格式检查，并检查文档本地链接和 git diff --check。修改协议额外运行解析、生成、兼容门禁；没有历史 descriptor 不得声称向后兼容通过。

## 检查命令组

- C：`cmake --preset dev && cmake --build --preset dev && ctest --preset dev`。
- P：C 加 `cmake --preset proto -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && cmake --build --preset proto && ctest --preset proto`；契约变化运行 `./proto/tests/check_contracts.sh`、相应 OpenAPI/AsyncAPI lint 和 descriptor 基线检查（见 proto/tests/README.md）。
- I：P 加 `cmake -S infrastructure -B build/infrastructure && cmake --build build/infrastructure && ctest --test-dir build/infrastructure --output-on-failure`；新测试注册 CTest。
- S：P/I 的相关项，加 SDK 项目固定依赖环境中的 `python -m pytest`；任务首次落地时提供锁文件、fake host 和可执行测试，不允许“无测试收集”通过。
- E：适配器按锁定环境安装，执行 `python -m pytest`；再运行 T07 新增的集成入口。适配器目录为 `adapters/nonebot/`，按其 README 安装锁定依赖并生成协议。
- W：`cd panel && npm run generate && npm run check:contracts && npm run lint && npm test && npm run build`，再运行 T09 新增的真实服务集成测试。
- O：`bash -n scripts/deploy/*.sh`（逐文件检查），Compose config、镜像构建、隔离部署/备份恢复演练；不得以 config 成功代替服务验收。
- 所有 C++ 变更另跑 `./scripts/check-toolchain.sh`，记录失败原因及直接验证结果。

## 依赖顺序

建议串行执行：T01a → T01b → T05a → T05b → T10a → T02a → T02b → T03a → T03b → T04a → T04b → T06a → T06b → T06c → T07a → T07b → T08a → T08b → T09a → T09b → T10b → T11a → T11b。T10a 虽属于管理工作包，授权是业务闭环前置，所以提前；T07a 在各模块开发时逐步补齐，不能等全系统完成才测试。T12 不执行。

## T01a：Core 进程启停

- 原工作包：1；状态：**已实现；dev 4/4、proto 7/7 通过**。
- 依赖：无。
- 允许修改：core/、根构建依赖、Dockerfile；禁止修改其余模块内部实现。
- 输入、输出与错误：CLI/停止信号 → 退出状态；参数错误 2、系统失败 1、正常 0。
- 文档同步：doc/02-core.md、doc/08-operations.md、doc/11-toolchain.md、doc/12-ci-cd.md。
- 验证命令组：C。
- 验收：
  - [x] 持续存活。
  - [x] SIGINT/SIGTERM 正常退出。
  - [x] --check 一次退出。
  - [x] 未知参数拒绝。

## T01b：gRPC 服务与身份边界

- 原工作包：1；状态：**已实现：本机认证传输；业务注册/投递仍由后续任务实现**。
- 依赖：T01a。
- 允许修改：core/、proto/ 直接受影响契约；禁止修改其余模块内部实现。
- 输入、输出与错误：连接/凭据 → 认证上下文；未认证 UNAUTHENTICATED、无权限 PERMISSION_DENIED。
- 文档同步：doc/01-contracts.md、doc/02-core.md、proto/README.md。
- 验证命令组：P。
- 验收：
  - [x] 监听可配置。
  - [x] 端口占用启动失败。
  - [x] 未认证拒绝。
  - [x] 停止取消流并 join。
  - [x] 不得接收业务后无声丢弃。

## T05a：统一运行时存储端口

- 原工作包：5；状态：**已实现；dev 5/5 通过，完整回归见交付记录**。
- 依赖：T01a。
- 允许修改：core/、infrastructure/ 接口适配；禁止修改其余模块内部实现。
- 输入、输出与错误：事件/Action/投递操作 → 显式成功、未找到、重复、存储失败。
- 文档同步：doc/02-core.md、doc/05-infrastructure.md、doc/01-runtime-semantics.md。
- 验证命令组：C。
- 验收：
  - [x] 保留逐插件唯一键与会话水位。
  - [x] 读故障不能伪装不存在。
  - [x] 事务销毁回滚。
  - [x] 现有内存测试不回退。

## T05b：SQLite 事务、迁移与恢复

- 原工作包：5；状态：**已实现；SQLite 适配器及集成测试通过，进程接线已由 T02a 完成**。
- 依赖：T05a。
- 允许修改：infrastructure/、CMake 集成；禁止修改其余模块内部实现。
- 输入、输出与错误：数据库路径/迁移版本 → 持久化端口；坏 schema/写故障明确失败。
- 文档同步：doc/05-infrastructure.md、doc/08-operations.md。
- 验证命令组：I。
- 验收：
  - [x] 迁移重复安全。
  - [x] 提交/回滚和唯一性。
  - [x] 关闭重开保持事件/投递/Action/会话。
  - [x] 磁盘故障与并发冲突可复现。

## T10a：Core 权限决策与审计端口

- 原工作包：10；状态：**已实现；Core 命令授权与事务审计，T02a/T03a 已接入桥接入口**。
- 依赖：T05a。
- 允许修改：core/、infrastructure/ 审计实现；禁止修改其余模块内部实现。
- 输入、输出与错误：可信身份/权限/资源 → 允许或 PERMISSION_DENIED；审计故障阻止受保护写入。
- 文档同步：doc/02-core.md、doc/05-infrastructure.md、doc/06-control-api.md。
- 验证命令组：I。
- 验收：
  - [x] 默认拒绝。
  - [x] 插件自报权限不授予。
  - [x] 拒绝无副作用。
  - [x] 成功写入与幂等/审计原子提交。
  - [x] 失败审计不泄密。

## T02a：适配器事件确认契约与 Core 接收

- 原工作包：2；状态：**已实现；协议与持久化接收验收通过，插件/Action 闭环仍待后续任务**。
- 依赖：T01b、T05b、T10a。
- 允许修改：proto/、core/；禁止修改其余模块内部实现。
- 输入、输出与错误：注册/事件 → 注册结果及持久化确认；INVALID_ARGUMENT、RATE_LIMITED、存储失败。
- 文档同步：proto/README.md、doc/01-contracts.md、doc/01-runtime-semantics.md、doc/04-nonebot-adapter.md。
- 验证命令组：P。
- 验收：
  - [x] 先定义当前缺少的事件确认消息。
  - [x] 不复用字段编号。
  - [x] 提交后确认。
  - [x] 重投不重复执行。
  - [x] 背压不丢事件。

## T02b：Python NoneBot2 适配器

- 原工作包：2；状态：**已实现；适配器迁入本仓库后验收 34/34（迁入前为 20 项），真实 QQ 留待 T07b**。
- 依赖：T02a。
- 允许修改：适配器实现（`adapters/nonebot/`）；本仓库仅契约测试语料；禁止修改其余模块内部实现。
- 归属调整（2026-10-30）：适配器源码、锁文件和测试已迁入本仓库 `adapters/nonebot/`；迁入后仍须按本次交付结果核对测试。后续可再考虑拆成单独插件。
- 输入、输出与错误：OneBot V11 → EventEnvelope；映射不支持返回稳定错误。
- 文档同步：doc/04-nonebot-adapter.md。
- 验证命令组：E。
- 验收：
  - [x] 文本/群/私聊映射。
  - [x] 注册能力。
  - [x] 断线重投。
  - [x] 确认前保留事件。
  - [x] Python 依赖锁定。
  - [x] 真实平台仅最后验收（未运行，归入 T07b）。

## T03a：插件注册、订阅和投递接线

- 原工作包：3；状态：**已实现；注册、订阅、逐插件投递验收通过，SDK/启动器已由 T03b 完成**。
- 依赖：T01b、T05b、T10a。
- 允许修改：core/、proto/ 必要协商扩展；禁止修改其余模块内部实现。
- 输入、输出与错误：manifest/认证流 → 授权订阅与投递；版本/权限不兼容拒绝。
- 文档同步：doc/02-core.md、doc/03-plugin-sdk.md、doc/01-runtime-semantics.md。
- 验证命令组：P。
- 验收：
  - [x] 两个插件独立 ACK。
  - [x] 有序会话不并发。
  - [x] 断线后停止旧投递者再恢复。
  - [x] 重放 trace 更新。

## T03b：Python SDK、fake host 与生命周期

- 原工作包：3；状态：**已实现；SDK 22/22，proto 15/15，dev 5/5 通过**。
- 依赖：T03a。
- 允许修改：sdk/python/、tools/ 插件工具、core/ 宿主启动器；禁止修改其余模块内部实现。
- 输入、输出与错误：插件清单/生命周期 → 注册、事件处理、停止结果。
- 文档同步：doc/03-plugin-sdk.md、sdk/PLUGIN_SDK_DEPENDENCIES.md。
- 验证命令组：S。
- 验收：
  - [x] SDK 只依赖生成协议。
  - [x] 示例插件收事件。
  - [x] 异常隔离。
  - [x] 停止超时回收进程。
  - [x] 重连重新协商。
  - [x] fake host 测试。

## T04a：Action 结果传递与查询契约

- 原工作包：4；状态：**已实现；协议生成、round-trip、旧端解码和 HEAD descriptor 兼容检查通过**。
- 依赖：T03a。
- 允许修改：proto/、api/ 直接受影响 schema；禁止修改其余模块内部实现。
- 输入、输出与错误：Action/查询 → ACCEPTED 或终态；权限、能力、超时错误。
- 文档同步：doc/01-contracts.md、doc/01-runtime-semantics.md、doc/06-control-api.md。
- 验证命令组：P。
- 验收：
  - [x] 补插件结果/查询承载。
  - [x] 调用方作用域幂等键。
  - [x] 按 ID 查询。
  - [x] 兼容旧客户端。
  - [x] 未知结果不误报失败。

## T04b：Action 分发与适配器执行

- 原工作包：4；状态：**已实现；阶段验收 Core 16/16、SDK 28/28；最终适配器 34/34 通过，当前 send_message 纯文本子集**。
- 依赖：T04a、T02b、T03b。
- 允许修改：core/、独立适配器、SDK 各自公共实现，以及直接受影响的 infrastructure/ 快照编解码和迁移；禁止修改其余模块内部实现。
- 输入、输出与错误：授权 Action → 平台调用与持久化结果。
- 文档同步：doc/02-core.md、doc/03-plugin-sdk.md、doc/04-nonebot-adapter.md。
- 验证命令组：E。
- 验收：
  - [x] 权限拒绝不调用平台。
  - [x] 执行前 deadline。
  - [x] 重复请求重用结果。
  - [x] 断线查询终态。
  - [x] 取消不覆盖已到期。

## T06a：Host 请求分发与存储 API

- 原工作包：6；状态：**已实现；Core Host 测试及 SDK 35/35 通过，schema v4 迁移覆盖 v1/v2/v3**。
- 依赖：T03b、T05b、T10a。
- 允许修改：core/、infrastructure/、sdk/python/；禁止修改其余模块内部实现。
- 输入、输出与错误：HostRequest → 对应 HostResponse；deadline/参数/幂等冲突错误。
- 文档同步：doc/03-plugin-sdk.md、doc/05-infrastructure.md、doc/01-runtime-semantics.md。
- 验证命令组：S。
- 验收：
  - [x] 关联 request_id。
  - [x] 身份确定 namespace。
  - [x] 同键不同内容 CONFLICT。
  - [x] TTL/限额。
  - [x] 业务/幂等/审计原子化。

## T06b：非流式模型 Provider 与示例 AI 插件

- 原工作包：6；状态：**已完成并验收**。
- 依赖：T06a。
- 允许修改：infrastructure/、plugins/、sdk/python/；为接通已有 IModelProvider 端口，直接扩展 Core HostDispatcher、Bridge 功能协商和启动组合根，以及对应构建/测试入口；不修改其他领域规则。
- 输入、输出与错误：ModelChatRequest → 非流式结果；TIMEOUT、RATE_LIMITED、UPSTREAM_UNAVAILABLE。
- 文档同步：doc/03-plugin-sdk.md、doc/05-infrastructure.md。
- 验证命令组：S。
- 验收：
  - [x] fake 正常/超时/限流/中断。
  - [x] 密钥不外泄。
  - [x] 示例插件仅通过 SDK。
  - [x] 外部模型不参与单元测试。

## T06c：持久化一次性任务调度

- 原工作包：6；状态：**已完成并验收**。
- 依赖：T06a。
- 允许修改：core/、infrastructure/、sdk/python/；禁止修改其余模块内部实现。
- 输入、输出与错误：任务时间/取消 → 持久化状态；非法时区/过期/权限错误。
- 文档同步：doc/01-runtime-semantics.md、doc/05-infrastructure.md。
- 验证命令组：I。
- 验收：
  - [x] UTC/时区/DST 校验。
  - [x] claim fencing。
  - [x] 重启 running→retrying。
  - [x] 取消不重复投递。
  - [x] 旧 claim 不覆盖新结果。

## T07a：最小闭环故障测试

- 原工作包：7；状态：**已完成并验收**。
- 依赖：T04b。
- 允许修改：tests/、各模块测试入口；验收定位到 Bridge 未提交派发的可重试事务冲突，因此直接修复发送循环，其他业务实现不变。
- 输入、输出与错误：固定事件与故障脚本 → 可重复断言/测试报告。
- 文档同步：doc/02-core.md、doc/12-ci-cd.md。
- 验证命令组：P。
- 验收：
  - [x] fake 平台事件→插件→Action→结果。
  - [x] 拒绝无副作用。
  - [x] 重复/重启/断线恢复。
  - [x] 可独立 CI 运行（入口本机已执行；远端 Actions 当时尚未运行，后续 5 次运行均未通过，见文末「2026-10-30 现状」）。

## T07b：真实独立组件联调

- 原工作包：7；状态：**等待真实环境，未验收**。用户已确认目前没有隔离 QQ/NapCat、模型及目标会话配置；准备说明见 tests/acceptance/real-environment.md。
- 依赖：T07a、T06b、T06c。
- 允许修改：tests/、独立适配器测试、deploy/ 联调环境；禁止修改其余模块内部实现。
- 输入、输出与错误：隔离 QQ/模型测试配置 → 人工或受控联调结果。
- 文档同步：doc/04-nonebot-adapter.md、doc/08-operations.md。
- 验证命令组：E。
- 验收：
  - [ ] 确认账号及依赖版本。
  - [ ] 真实收发和回滚。
  - [ ] 敏感配置不提交。
  - [x] 平台不可用明确记未验证；用户正在准备环境。

## T08a：控制应用端口与剩余契约决策

- 原工作包：8；状态：**已完成并验收（端口与契约；服务仍属 T08b）**。
- 依赖：T04a、T10a。
- 允许修改：core/、api/、必要 proto/；禁止修改其余模块内部实现。
- 输入、输出与错误：管理查询/配置命令 → 分页资源/接受状态。
- 文档同步：doc/06-control-api.md、doc/13-control-api-contract-handoff.md。
- 验证命令组：P。
- 验收：
  - [x] 先核对历史交接与现行契约。
  - [x] 确认同进程端口或版本化 RPC。
  - [x] 会话撤销、幂等、分页/快照与错误逐项冻结。

## T08b：控制 API 服务

- 原工作包：8；状态：**已完成并验收（本机自动验收；生产部署仍归 T11b）**。
- 依赖：T08a、T05b。
- 允许修改：control/api/、core/ 应用服务；本次配置闭环按用户要求扩展至 proto/ 和 sdk/python/ 的直接接口/实现及对应测试，保持现有混合语言边界。
- 输入、输出与错误：REST 请求 → 契约响应；401/403/409/429/503/504。
- 文档同步：doc/06-control-api.md、doc/08-operations.md。
- 验证命令组：P。
- 验收：
  - [x] 登录轮换/撤销。
  - [x] 每个写入口授权审计。
  - [x] 查询与配置版本冲突。
  - [x] mock 和真实端口同套契约测试。
  - [x] 配置 API → 运行中 SDK → 应用 ACK → GET 状态查询闭环（后续验收见本页末尾）。

## T09a：服务端控制事件流

- 原工作包：9；状态：**已完成并验收（本机自动验收；生产部署仍归 T11b）**。
- 依赖：T08b。
- 允许修改：control/api/、core/ 事件端口、infrastructure/ 保留实现；禁止修改其余模块内部实现。
- 输入、输出与错误：hello/resume → SESSION_READY、重放或 RESYNC_REQUIRED。
- 文档同步：doc/06-control-api.md、doc/01-runtime-semantics.md。
- 验证命令组：P。
- 验收：
  - [x] 绑定认证主体。
  - [x] 10 分钟保留。
  - [x] 频道权限过滤。
  - [x] 过期 token 拒绝。
  - [x] 恢复与快照无遗漏。

## T09b：TypeScript 面板真实集成

- 原工作包：9；状态：**已完成并验收（本机自动验收；生产部署仍归 T11b）**。
- 依赖：T09a。
- 允许修改：panel/、control/api/ 测试夹具；禁止修改其余模块内部实现。
- 输入、输出与错误：真实 REST/WSS → 页面和状态；会话过期回登录。
- 文档同步：doc/07-control-panel.md。
- 验证命令组：W。
- 验收：
  - [x] 保留现有 TS 技术栈。
  - [x] 登录刷新/401/403。
  - [x] 断线恢复/重同步。
  - [x] 角色来自服务端。
  - [x] 日志脱敏。

## T10b：运行时结构化日志接线

- 原工作包：10；状态：**已完成并验收（本机自动验收；生产部署仍归 T11b）**。
- 依赖：T10a、T03a。
- 允许修改：core/ 日志端口、infrastructure/ sink；禁止修改其余模块内部实现。
- 输入、输出与错误：运行时操作 → 结构化标识/结果。
- 文档同步：doc/02-core.md、doc/05-infrastructure.md、doc/08-operations.md。
- 验证命令组：I。
- 验收：
  - [x] 成功/拒绝/重试/死信/故障覆盖。
  - [x] 不记录正文密钥。
  - [x] sink 故障行为明确。
  - [x] 日志不代替业务提交。

## T11a：健康契约、迁移命令和启动接线

- 原工作包：11；状态：**已完成并验收（本机自动验收；生产部署仍归 T11b）**。
- 依赖：T01b、T05b、T08b。
- 允许修改：api/、core/、control/api/、infrastructure/ 迁移入口；禁止修改其余模块内部实现。
- 输入、输出与错误：健康请求/迁移调用 → 稳定响应/退出码。
- 文档同步：doc/08-operations.md、doc/06-control-api.md。
- 验证命令组：P。
- 验收：
  - [x] 先定义进程探针与控制健康关系。
  - [x] 迁移失败禁止 ready。
  - [x] 存活不依赖数据库。
  - [x] 坏 schema 拒绝启动。

## T11b：镜像、部署与恢复演练

- 原工作包：11；状态：**等待依赖：T07b 真实环境由用户准备；本机未安装 Docker，未做部署/恢复验收**。
- 依赖：T11a、T09b、T10b、T07b。
- 允许修改：Dockerfile、deploy/、scripts/deploy/、CI；禁止修改其余模块内部实现。
- 输入、输出与错误：固定镜像/配置/备份 → 可追踪部署。
- 文档同步：doc/08-operations.md、doc/12-ci-cd.md。
- 验证命令组：O。
- 验收：
  - [ ] 真实镜像替换占位。
  - [ ] 启动/关闭顺序。
  - [ ] 备份校验恢复。
  - [ ] 回滚前验证 schema。
  - [ ] 隔离 staging 演练。

## T12：未来 C++ 客户端替换

- 原工作包：12；状态：**延期：用户明确当前不需要**。
- 依赖：未来单独决策。
- 允许修改：未来客户端目录；当前禁止实施；禁止修改其余模块内部实现。
- 输入、输出与错误：复用 REST/WSS → 原生客户端。
- 文档同步：doc/07-control-panel.md。
- 验证命令组：—。
- 验收：
  - [ ] 当前版本不要求。
  - [ ] 未来先确认 GUI/CLI 范围再定义功能等价验收。


## 本轮交付记录

已实现 T01a，未宣称 T01b 或任何完整跨层工作包完成。新增真实子进程测试覆盖持续存活、SIGINT/SIGTERM、--check、--help、非法参数和多余参数；dev 4/4、proto 7/7 通过。新增 Python 3.9+ 构建测试依赖已同步 CMake、Docker 构建阶段和工具链文档；运行镜像不加入 Python。

变更 main.cpp 已使用 Xcode clang-format 格式化且单文件 dry-run 通过。统一检查最初因 PATH 中没有 clang-format 停止；使用 Xcode 工具路径后因缺少 clang-tidy 停止，静态分析未运行。全仓格式脚本还存在既有源码格式差异，并扫描 .venv/.tools 第三方头文件，本轮未批量格式化或修改检查范围。未运行镜像构建、非 POSIX 平台、真实网络或生产部署；无协议/API 字段改动，未重跑 descriptor 基线兼容门禁。文档链接路径和 git diff --check 必须通过。

本轮文档影响：更新 doc/02-core.md、doc/08-operations.md 的启停语义；doc/11-toolchain.md 与 doc/12-ci-cd.md 同步测试依赖和部署限制；索引、评估、缺口清单及本台账同步当前范围。proto/、api/、doc/01-contracts.md、doc/01-runtime-semantics.md、doc/03 至 doc/07 无新增接口、投递、权限或业务状态变化，无需修改；纯 C++ 未来设计保留。

## T01b 验收记录（2026-10-30）

- 实现：core/include/kiarabot/core/runtime/bridge_server.hpp、core/src/runtime/bridge_server.cpp、main.cpp，根 CMake 与 Core 测试注册；两个桥接的本机监听和 bearer 身份边界。不宣称业务注册、事件入库或插件执行可用。
- 验收：proto 配置/编译/CTest 9/9；新增 core_bridge 与 core_grpc_process，覆盖缺失/错误/重复凭据、角色不匹配、身份冒用、空消息、显式不支持、deadline、活动流关闭、端口复用、外部地址拒绝和两个进程停止信号。dev 回归 4/4 通过。
- 文档：services.proto 增补连接认证注释（字段/RPC 签名未变）；更新 proto/README.md、doc/01-contracts.md、doc/02-core.md、doc/08-operations.md 以及台账。API、面板、SDK 与事件投递语义未改动，无需同步。
- 检查：契约脚本、相对 HEAD 的 descriptor 兼容检查和四个变更 C++ 文件格式检查通过；统一工具链仍因缺少 clang-tidy 停止。测试使用隔离的本机临时端口，不依赖外部服务。尚未进行 TLS/跨主机/真实 Python 组件验收，属于后续任务。
- 下一任务 T05a 仍未完成：需消除 RuntimeStore 读取接口的故障歧义，再连接 SQLite，禁止用“未找到”吞掉存储故障。

## T05a/T05b 连续实施记录（2026-10-30）

T05a：公共 Runtime/RuntimeStore 统一为事务快照和显式 StorageResult；旧状态机移入 detail::RuntimeEngine，原三组算法测试保留；新增公共端口测试覆盖读错误、未找到、重复、提交失败、放弃事务、版本冲突及投递唯一性。Host ports 移除不完整 Repository，复用 RuntimeSnapshot 和 Result 约定。

T05b：SQLite schema v1 初始化、64 MiB 私有快照编码、revision 原子更新；支持关闭重开、逐插件恢复、Action 幂等、会话水位、事务回滚、重复迁移与损坏拒绝。测试使用临时数据库及独立连接，注入写锁、并发旧版本提交、损坏数据、未知 schema 和数据库文件移除，验证失败不伪装成功。

验收结果：dev 5/5；proto 启用 KIARABOT_ENABLE_INFRASTRUCTURE=ON 后 12/12；基础设施独立构建 2/2。数据层均为 C++，未迁移 Python SDK/适配器或 TypeScript 面板。快照方案复制全部状态，未做性能或生产容量认证；未连接 daemon、Host 组合事务或健康服务。

文档影响：更新 doc/02-core.md、doc/05-infrastructure.md、doc/01-runtime-semantics.md（公共批量操作整体提交）、doc/08-operations.md、基础设施交接和本台账；跨语言 proto/API 字段未变，无需修改其 schema。业务桥后续必须实现 StorageError 的稳定映射。格式检查覆盖本轮修改 C++；全量工具链检查仍需记录 clang-tidy 可用性，未验证生产部署或真实 QQ。

## T10a 连续实施记录（2026-10-30）

完成静态默认拒绝授权、可信身份上下文、成功命令幂等以及与业务状态原子提交的审计。内存与 SQLite 使用同一验收：拒绝不调用业务、同键重放不重复副作用、不同 fingerprint 冲突、回调失败撤销状态且脱敏、审计容量不足拒绝、提交失败不留下业务/幂等/审计记录。Runtime 普通操作保留审计与命令元数据，SQLite v1→v2 迁移及重开后的审计/幂等恢复纳入验收。

本轮共连续实施 T05a、T05b、T10a。最新集成构建启用基础设施后 13/13 测试通过；此前 T05b 的 12/12 是增加授权测试前的阶段结果。更新 doc/02-core.md、doc/05-infrastructure.md、doc/06-control-api.md 和 schema 迁移说明；REST/Protobuf/AsyncAPI 字段未改变，未新增跨进程授权接口。静态授权配置不等于网络业务已可用，T02a 起还须将桥接认证身份接到应用入口并提供持久化确认。

## T02a 连续实施记录（2026-10-30）

新增 AdapterEventAck 与 CoreToAdapter.event_ack=4；认证注册、配置账号授权、实例替换、事件校验、SHA-256 内容去重、事务审计和提交后确认已接线。队列满返回可重试 RATE_LIMITED 并结束流，未确认事件由发送方保留。可通过 KIARABOT_RUNTIME_DB/KIARABOT_ADAPTER_ACCOUNTS 启用 SQLite 入口；未配置时维持显式拒绝业务。

验收：dev 5/5、proto+基础设施 14/14、独立基础设施 3/3；新增 adapter_ingress 覆盖身份/账号/载荷拒绝、读写失败回滚、重投/内容冲突、队列满、旧连接隔离以及提交后丢失确认、关闭重开后去重。进程测试验证持久化启动、重复打开、非法配置、坏 schema 禁止启动及停止信号。协议检查与相对 HEAD 的 descriptor 兼容门禁通过。

文档影响：同步 proto/README.md、doc/01-contracts.md、doc/01-runtime-semantics.md、doc/02-core.md、doc/04-nonebot-adapter.md、doc/08-operations.md、doc/11-toolchain.md；doc/05-infrastructure.md 与交接说明修正当前接线状态。OpenAPI/AsyncAPI、面板和 SDK 未改变接口，无需更新。OpenSSL Crypto 为显式构建依赖（本机检测 4.1.0，要求 3.0+）；精确发布依赖锁定留待 T11。尚未验证真实 QQ、跨主机 TLS、生产吞吐和完整闭环。

## T02b 连续实施记录（2026-10-30）

适配器当时位于独立检出（已迁入本仓库 `adapters/nonebot/`）。已实现 NoneBot2 正向 WebSocket、群/私聊映射、消息段转换、有界持久化 outbox、会话序号、能力注册、确认前保留、断线重投、稳定错误和脱敏日志。Python 3.11.18 与 uv.lock 固定环境；生成代码放入忽略目录，协议唯一来源仍是本仓库 proto/。

在当时的独立检出运行 `CORE_TEST_BINARY=<KiaraBot 仓库>/build/proto/core/kiarabot-core .venv/bin/python -m pytest -q`：**20/20 通过**（迁入前口径；迁入后验收 34/34，见 T04b/T06a 记录）。包含实际 NoneBot 子进程连接 fake WebSocket/Core，以及 Python 客户端连接真实 C++ Core、重启后去重。真实 NapCat/QQ 留待 T07b，Action 执行留待 T04b。初次清理测试错误地要求正常停止也必须抛取消异常，已修正为两种有效退出都可通过，并始终清理 fake server；业务断言未放宽。

## T03a 连续实施记录（2026-10-30）

已实现 PluginHub、认证注册、配置权限、接收事件时的订阅快照、逐插件投递/ACK、每插件一条在途事件、连接隔离、发送线程受控 join、重启恢复及重放 trace 更新。兼容新增 CoreToPlugin.error=6。当前注册支持文档限定的 Python/process 事件插件子集；进程启动器、配置 schema 和 Host/Action 服务仍待后续任务。CLI 配置一个插件；库与实际 gRPC 测试覆盖两个独立插件。

验收：启用基础设施的 proto 构建及 CTest **15/15**，dev **5/5**，独立基础设施 **3/3**，Python 适配器 **20/20**；插件关闭/重连测试另连续通过 10 次。协议检查和相对 HEAD 的 descriptor 兼容检查通过。SQLite 故障测试还验证读锁返回可重试 Conflict。重连验收等待远端发送线程退出；客户端本地取消完成不代表服务端清理已完成。

文档影响：更新 proto/README.md、doc/01-contracts.md、doc/01-runtime-semantics.md、doc/02-core.md、doc/03-plugin-sdk.md、doc/04-nonebot-adapter.md、doc/05-infrastructure.md、doc/06-control-api.md、doc/08-operations.md、doc/11-toolchain.md、doc/13-infrastructure-integration-follow-up.md、本台账及独立项目 README。OpenAPI/AsyncAPI schema 与面板行为没有改变，无需额外同步；SDK 尚未实现，T03b 需按当前协议校正其历史依赖说明。

最终检查：28 个变更 C++ 文件的 clang-format dry-run、Markdown 本地链接校验、git diff --check 均通过；独立项目 uv lock --check 与 uv pip check 通过。已运行要求的 ./scripts/check-toolchain.sh，在 PATH 加入 Xcode 工具后仍因缺少 clang-tidy 停止，静态分析未运行。仓库改动不包含生成源码、构建产物、token 或本地环境。真实 QQ/模型、TLS、容量性能、Docker 部署及生产恢复未验证。本轮连续完成 T05a、T05b、T10a、T02a、T02b、T03a；**下一待实现任务为 T03b**。当前混合语言架构保持不变，T12 延期。


## T03b → T04a → T04b → T06a 连续实施记录（2026-10-30）

本轮按依赖连续完成四项，保持混合语言架构，未实施 T12。下一待实现为 T06b（非流式模型 Provider 与示例 AI 插件），之后 T06c；其余未完成项仍保持待实现，不以接口草案替代验收。

### T03b：SDK 与进程生命周期

目标模块：sdk/python、plugins/event-observer、core 宿主及对应构建。输入为清单/凭据/停止信号，输出为注册、事件 ACK 和进程退出；非法配置 INVALID_ARGUMENT，不支持运行时 CAPABILITY_NOT_SUPPORTED，回调异常 INTERNAL，协作超时 TIMEOUT，依赖失联 UPSTREAM_UNAVAILABLE。SDK 仅依赖生成协议；Core 不链接 Python。POSIX 启动器使用独立进程组、显式最小环境，启动等待注册，停止 TERM → 有界等待 → KILL/reap。未启动操作系统安全沙箱，当前面向可信插件。

阶段验收 SDK 22/22、proto 15/15、dev 5/5。真实 C++ Core 启动 Python SDK，测试事件接收、正常停止、阻塞停止强制回收、启动超时、插件崩溃 Core 存活、配置错误和环境隔离。FakeHost 覆盖注册拒绝、异常/超时隔离、重连更新实例并重新校验权限。

### T04a：Action 契约

兼容新增 action.result.v1 功能协商、Action 请求关联、查询/取消和结果 oneof，不复用字段。调用方+幂等键决定作用域，按 ID 查询也验证归属；传输未知、查询失败和未知状态不会误报 FAILED。REST/AsyncAPI 路径和 schema 未改变。

验证 C++ roundtrip、Python 生成/旧 reader 未知字段保留、旧服务默认不启用功能、查询 selector 和未知状态；check_contracts.sh、api_contract_test.rb、descriptor_compatibility_test.sh 及相对本仓库 HEAD 的 descriptor 检查均通过。没有发布版 descriptor，不声称发布版本兼容认证。API 校验脚本原来引用旧 LogEnvelope/Session，已改为验证现行 LogStreamEnvelope→REST LogEntry 和 SessionReady；没有改变契约来迎合测试。

### T04b：Action 执行闭环

允许修改扩展至直接受影响的 infrastructure 快照编解码/迁移，用于持久化请求、所有者、fingerprint、派发标志和终态。ActionRouter 在提交后派发，按认证适配器/账号/实例接收结果；同键同内容复用，内容冲突拒绝，派发后取消 CONFLICT，到期优先 EXPIRED。首次已到期请求仍持久化过期结果，不要求在线适配器。当前只支持群/私聊 send_message 的 plain_text，其他操作/段明确拒绝。

独立适配器（已迁入本仓库 `adapters/nonebot/`）增加 .actions SQLite 执行日志；执行前落盘，正常重放返回原结果。平台调用超时/断线/取消保持未知，不再次执行可能发生的副作用。真实 NoneBot 子进程通过 fake OneBot 验证发送，另用真实 Core + Python SDK + Python 适配器验证闭环和重启后查询/去重。未知结果可能最终 EXPIRED，不能撤销平台已发生的副作用。没有连接真实 QQ。

关闭回归发现 gRPC 取消可能以传输失败返回并触发重连退避，已在 SDK/适配器退出前检查任务取消状态；新增模拟该竞态的测试，完整适配器验收 34/34，最终运行约 2.3 秒。

### T06a：Host 存储事务

HostDispatcher、SDK storage_get/set/delete 与 host.storage.v1 协商接线。namespace 由认证插件确定；要求 storage.namespace 权限。值最多 64 KiB，单插件 1024 个有效键/1 MiB，TTL 最多三十天；成功写幂等响应保留七天、单插件最多一万条。缺失/过期 NOT_FOUND，超限 RATE_LIMITED，内容冲突 CONFLICT。请求关联、业务数据、幂等响应和审计在同一 RuntimeTransaction 中完成，提交失败回滚；提交前再次检查 deadline。

SQLite 当前 schema v4，覆盖 v1/v2/v3 迁移、重开与数据保留。旧二进制不能读 v4，回滚需迁移前备份。原 Runtime/插件/事件操作保留 Host 集合。Core 测试验证两个插件隔离、二进制/空值、TTL 重放不续期、容量限制、读写/审计失败和超时回滚；SDK 验证畸形响应、未协商拒绝、权限拒绝及真实 Core 重启后读/重放。模型和调度未接线，不启用其功能。

### 最终验收与文档影响

实际命令及结果：
- cmake --preset dev / cmake --build --preset dev / ctest --preset dev：5/5。
- cmake --preset proto -DKIARABOT_ENABLE_INFRASTRUCTURE=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON / cmake --build --preset proto / ctest --preset proto：17/17。
- cmake -S infrastructure -B build/infrastructure / cmake --build build/infrastructure / ctest --test-dir build/infrastructure --output-on-failure：3/3。
- sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py 与 python -m pytest -q sdk/python/tests：生成成功，35/35。
- 独立适配器重新生成协议，CORE_TEST_BINARY 指向当前 Core 后 python -m pytest -q：34/34。
- SDK uv lock --check、uv pip check：通过；协议/descriptor/API 校验见 T04a 记录。
- 36 个变更 C++ 文件的 clang-format dry-run、21 个 Markdown 文件本地链接/UTF-8 校验、主仓库 git diff --check 均通过；不格式化第三方环境。独立适配器目录尚未初始化 Git，无法运行 git diff --check，改用源文件/文档文本与行尾空白检查通过；没有擅自初始化或提交仓库。
- scripts/check-toolchain.sh：实际执行，因缺少 clang-tidy 停止，静态分析未运行。未运行生产部署、Docker、TLS、容量性能、真实 QQ/模型或非 POSIX 平台验收。

一次全量回归中 sqlite_runtime_store 报告了无行号断言失败。已补充 source_location 便于定位，未放宽断言；随后该测试顺序重复 20 次、8 路并发共 64 次及最终全量回归均通过，暂未复现，仍需在 T07a/CI 中留意该偶发失败，不能宣称已经定位根因。

文档影响判断：功能协商/帧更新 proto/README.md、proto/tests/README.md、doc/01-contracts.md；ACK、未知结果、幂等、取消、TTL 更新 doc/01-runtime-semantics.md；模块行为同步 doc/02-core.md、doc/03-plugin-sdk.md、doc/04-nonebot-adapter.md、doc/05-infrastructure.md；运行配置/迁移同步 doc/08-operations.md、doc/11-toolchain.md；当前交接同步 doc/13-infrastructure-integration-follow-up.md、sdk/PLUGIN_SDK_DEPENDENCIES.md、sdk/python/README.md、本台账及独立适配器 README。doc/06-control-api.md 只补充与插件 Action 查询共享语义，REST 接口未变。

无需更新：api/openapi.yaml、api/asyncapi.yaml 和 doc/07-control-panel.md（REST/WS/面板行为未改）；doc/00-boundaries.md（模块责任与混合语言边界未改）；doc/12-ci-cd.md 与 Dockerfile（本轮未新增容器运行依赖或改变发布流程）；历史评估/缺口文档沿用台账作为实时进度源。构建产物、生成代码、虚拟环境、本机凭据均未纳入交付。


## 2026-10-30 连续实现记录：T06b → T06c → T07a → T08a

按用户要求先提交上轮已验收改动：Core 9d532c8，独立适配器初始化独立 Git 并提交 3b79bd3；没有推送。随后保持混合语言架构，完成下列四项。T07b 真实环境尚未提供，用户已确认将自行准备，因此没有伪造真实收发结果；继续了不依赖它的 T08a。下一实现项是 T08b，T09/T10b/T11 仍待后续，T12 继续延期。

### T06b：模型

新增 infrastructure HttpModelProvider，使用 libcurl >= 7.85、现有 Protobuf JSON，接通 IModelProvider → Core HostDispatcher → host.model.v1 → Python Context.model_chat。配置单个上游模型，default 为别名；仅 HTTPS/回环 HTTP、禁代理/重定向、限额和三十秒上限。权限及调用前审计提交失败不发网络请求，网络期间不持有数据库事务；结果审计单独提交。超时/断连可能已计费，SDK/Provider 不自动重试；原始上游错误与密钥不越过边界。ai-reply 示例仅通过 SDK，记录调用意图避免重放重复计费，未知调用可能不回复。

验收：fake 正常、TIMEOUT、429、503、中断、畸形/超大响应、工具调用、非法 URL、权限/功能拒绝；审计失败不调用、结果审计失败不宣称未执行、deadline、子进程密钥隔离及示例重复事件。T06b 阶段 CTest 18/18、SDK 54/54；后续累计结果见下。

### T06c：任务

新增 Core TaskScheduler、ITimeZoneResolver 及基础设施 Abseil 实现，使用系统 IANA zoneinfo；UTC/本地日期/DST 歧义严格校验。SQLite schema v5 保存任务、原定时间、重试状态、随机 claim token 与唯一 timer event ID，迁移 v1–v4。Host 创建/取消的授权、幂等、审计原子提交。每 100ms 领取最多 32 个，最多五次重试；完成与 owner-only TimerTriggered 入队原子提交。重启恢复撤销旧 token，取消未入队任务不投递，终态不可覆盖。COMPLETED 是入队完成，不是插件 ACK。

验收发现旧快照枚举解码范围不足，已为 TaskState 增加 1–6 范围并验证 CANCELLED/FAILED 重开。UTC/IANA/DST、fencing、旧 token、取消竞争、失败回滚、五次重试、SQLite 重开以及真实 Core 被终止后 SDK 收取定时事件均通过。T06c 阶段 CTest 19/19、SDK 56/56。

### T07a：闭环与 CI

新增 tests/acceptance/run.sh，可独立执行 CTest、编译后 Core/REST 权限对照、SDK 与可选独立适配器验收。新增四种真实 Core/SQLite/SDK + fake 平台场景：正常、拒绝、平台执行后结果丢失/重连、结果丢失/Core 重启；重复事件和 Action ID 不重复副作用。

全量测试定位到派发与 ACK 的乐观事务冲突导致 Bridge 断线，修复为仅对 retryable CONFLICT 等待 20ms 后重取未提交派发；此时尚未向流写出，不重发模型或插件业务调用。修复后四场景连续五轮（20 个场景）通过。此前台账记录的另一个 SQLite 单次未定位断言未据此宣称修复。CI contracts job 补齐 SQLite/libcurl/Abseil、tzdata 与锁定 Python 环境，远端 GitHub Actions 当时尚未运行（后续 5 次运行均未通过，见文末「2026-10-30 现状」）。

### T07b 准备与 T08a 契约

真实环境指南与两个无密钥模板位于 tests/acceptance/real-environment.md、core.env.example、adapter.env.example。按实际源码核对环境变量；Core/独立适配器配置分开，默认消息观察插件不会发消息/调用模型。用户待提供本机配置路径、账号、会话及版本；真实 QQ/模型/TLS/部署恢复未验收。

T08a 选择同进程 C++ IControlApplication，不新增管理 gRPC，不改变 Python/TS 模块。补齐当前主体、会话列表/撤销、逐入口权限与错误响应；以 OpenAPI x-control-policy 和 AsyncAPI x-control-session-policy 冻结角色、幂等/轮换/撤销、十分钟不可变快照、流恢复及 HTTP 错误。C++ 端口类型和校验辅助实现已通过正常/拒绝/超时/依赖错误映射测试，编译后的全部 23 个操作与 OpenAPI 权限/默认角色一致。历史 `doc/13-control-api-contract-handoff.md` 标记为交接历史，不再与现行契约竞争。T08b HTTP 服务未实现，T09 服务端事件流也未实现，不以端口验收冒充运行服务。

### 实际检查与文档影响

- proto + infrastructure 构建无新增编译警告，CTest 20/20；dev 5/5；独立 infrastructure 3/3。
- 统一验收入口：SDK 60/60、独立适配器 34/34；Python 协议生成成功。SDK uv lock --check、uv pip check 通过。
- OpenAPI Redocly 校验、AsyncAPI parser、Ruby API/Core 对照、HEAD 基线 descriptor 兼容检查通过；AsyncAPI 保留现有 2.6.0，只有升级/未引用组件提示。
- panel npm run generate/lint/test/build/check:contracts 全部通过，4/4 测试；生成代码/构建产物不提交。
- 23 个本轮 C++ 文件 clang-format dry-run 通过；Markdown UTF-8/本地链接、shell 模板/入口语法、workflow YAML、git diff --check 通过。
- 实际运行 scripts/check-toolchain.sh，仍因缺少 clang-tidy 停止，静态分析未运行。远端 CI 当时未运行（后续 5 次运行均未通过，截至 `e70de83`）；真实 QQ/模型、生产 TLS、Docker、性能容量及非 POSIX 平台未验证。

文档影响判断：模型/任务新增公共能力与配置，控制面冻结接口/权限/错误，CI 验收入口和 SQLite 迁移也变化，必须同步直接文档。已更新 proto/README.md、plugins.proto 注释、api/openapi.yaml、api/asyncapi.yaml、doc/01-contracts.md、doc/01-runtime-semantics.md、doc/02-core.md、doc/03-plugin-sdk.md、doc/04-nonebot-adapter.md（仅 T07b 准备段）、doc/05-infrastructure.md、doc/06-control-api.md、doc/08-operations.md、doc/11-toolchain.md、doc/12-ci-cd.md、doc/13-control-api-contract-handoff.md、sdk/python/README.md、tests/acceptance/ 和本台账。

无需更新 doc/00-boundaries.md（混合语言边界和责任不变）、doc/07-control-panel.md（面板 UI/业务实现未改，仅验证新增兼容契约生成）、Dockerfile（仍是历史仅 dev Core 容器，不声称支持新增生产能力，部署升级属于 T11）、历史评估清单（实时进度由本台账维护）。文档校验命令为 proto/tests/check_contracts.sh、scripts/ci/check-descriptor.sh HEAD、scripts/quality/check-openapi.sh、scripts/quality/check-asyncapi.sh、ruby proto/tests/api_contract_test.rb、panel npm run check:contracts 及本地链接检查，均通过。


## 2026-10-30 连续实现记录：T08b → T09a → T09b → T10b → T11a

按用户要求连续推进以上五项，保持 C++20 Core、Python 适配器/进程外 SDK 和 TypeScript 面板；未实施 T12。保留同期对 README、适配器归属和风险说明的修改，没有迁移或重写适配器。T07b 明确等待用户的真实环境；T11b 依赖 T07b 且本机没有 Docker CLI，因此不标部署恢复完成。没有提交或推送。

### 目标、输入输出与修改边界

- T08b：control/api 的 HTTP 传输映射、Core ControlService 与组合根；必要的 Core PluginHub/AdapterIngress 公共状态接线。输入 REST 请求，输出 OpenAPI 类型与 400/401/403/404/409/429/500/503/504 稳定错误。共享 RuntimeStore 的业务、加密幂等响应和审计同事务提交；不在 HTTP 层访问 SQL。
- T09a：control/api 事件流与 Core 脱敏投影。hello/resume → SESSION_READY、按序事件或 RESYNC_REQUIRED；非法首帧 4400、认证失败 4401、频道拒绝 4403、依赖失败 1011。保留十分钟且有容量上限，积压按 32 条分批重放；容量/时间缺口要求重同步。每 100ms 合并资源状态刷新，不能拿流通知代替每次操作的审计。
- T09b：仅 panel 客户端、mock/真实服务测试夹具和文档。服务端角色、刷新/撤销、401 回登录、403 保留会话、先流后快照、全部已知资源刷新、断线恢复；迟到的旧请求 401 不清除新令牌。类型从契约生成，继续 React/TS/Vite。
- T10b：Core 日志端口/RuntimeStore 装饰器、infrastructure sink 和组合根。事务结果 → 结构化成功/拒绝/重试/死信/故障记录；不接收正文/凭据/配置值。日志提交后观察，sink 故障计数并 fail-open，不改变已完成业务；事务审计仍 fail-closed。日志缓冲十分钟/4096 条，经应用端口供 API 读取。
- T11a：OpenAPI 进程探针、Core 控制应用探针、HTTP 映射、infrastructure 迁移命令。根路径 live 不访问数据库，ready 探测可写；详情仍为认证 /api/v1/health。迁移成功 0、参数错误 2、损坏/未知 schema/依赖或锁超时 1；SQLite 锁冲突有界等待三十秒。坏 schema/错误控制密钥不能启动控制服务。

### 实现及验收边界

控制配置由当前用户所有的 0600 文件提供，拒绝符号链接；密码使用 Argon2id，令牌随机生成，指纹为 keyed digest，状态/响应用 secretbox 加密后放入保留命名空间。插件身份不能访问 `$control` 命名空间，未增加 SQLite schema 或 Protobuf 字段。会话轮换/撤销/过期、旧 token 同键重放、同键不同请求、配置版本冲突、跨主体分页与会话隔离均有测试。配置更新仅持久化非秘密控制值，当前 SDK 不消费这些值；202 是接收投递开关，不是插件进程已启动。

Core 单测对内存和 SQLite 执行同一应用验收；HTTP schema/路由/角色测试对独立 mock 应用端口和真实 SQLite 端口执行同一套断言。真实进程覆盖重启后会话/幂等恢复、数据库不含明文 token/密码、写锁失败无副作用、监听和配置错误。WebSocket 覆盖主体/频道隔离、恢复顺序、过期窗口、队列分批、认证撤销后一秒内关闭、重启重同步及快照期间变化。TLS 测试使用临时证书并启用客户端证书校验，通过本机 HTTPS/WSS 代理访问真实 Core；不等于生产代理验收。

### 实际检查

- proto + infrastructure 配置/编译无新增警告；CTest **24/24**，含控制应用、事件流、HTTP/WebSocket、结构化日志；dev **5/5**。
- infrastructure 独立配置/构建/CTest **4/4**，新增迁移命令构建成功。
- 统一 `KIARABOT_ADAPTER_DIR=... bash tests/acceptance/run.sh`：控制验收当时 **15/15**，SDK **60/60**，适配器 **34/34**；随后新增可信 HTTPS/WSS 代理验收 **1/1**。控制验收总计 **16 项**，随后全量 **16/16** 通过，无真实 QQ/模型调用。
- panel `npm run generate/check:contracts/lint/test/build` 通过，最终 **7/7**（5 个 UI/恢复/竞态单测，2 个 TypeScript 对真实 C++ 服务测试）。
- `proto/tests/check_contracts.sh`、`scripts/ci/check-descriptor.sh HEAD`、Ruby API/Core 权限对照、OpenAPI lint、AsyncAPI parser 均通过；只有现有 AsyncAPI 升级/未使用组件提示。不声称发布版 descriptor 兼容认证。
- 本轮 25 个 C++ 文件 clang-format dry-run 通过；Markdown UTF-8/本地链接、workflow YAML、变更 shell 语法、uv lock --check / uv pip check、git diff --check 通过。
- 实际运行全仓 `scripts/quality/check-format.sh`：排除生成/依赖目录后，仍有 5 个未改动文件的既有格式问题（file_storage 三文件、host_validation_test.cpp、descriptor_compatibility.cc）；遵照仓库规则未顺手格式化。本轮修改文件检查通过。
- 实际运行 `scripts/check-toolchain.sh`：缺少 clang-tidy，静态分析未运行。远端 CI/actionlint 当时未运行（后续 5 次运行均未通过，截至 `e70de83`）；生产代理、Docker 镜像/部署恢复、性能容量、非 POSIX 平台及 T07b 真实环境未验收。

### 文档影响判断

已同步唯一契约 api/openapi.yaml（进程探针、运行限制与配置语义）、api/asyncapi.yaml（合并状态通知与容量/恢复），及 doc/01-runtime-semantics.md、doc/02-core.md、doc/05-infrastructure.md、doc/06-control-api.md、doc/07-control-panel.md、doc/08-operations.md、doc/11-toolchain.md、doc/12-ci-cd.md、control/api/README.md、tests/acceptance/README.md、THIRD_PARTY_NOTICES.md 与本台账。构建/验收入口及 CI 依赖一起接线；版本固定和本机/CI 版本差别见工具链文档。

本任务无需修改 proto/ 消息定义、doc/01-contracts.md、proto/README.md、doc/03-plugin-sdk.md 或 SDK 依赖（未改变跨语言字段、桥接错误码或 SDK 行为）；doc/00-boundaries.md、doc/04-nonebot-adapter.md 中同期适配器归属说明保留，本任务不改变语言/模块边界或适配器实现。Dockerfile、deploy/ 与 scripts/deploy/ 未改，等待 T11b 的依赖。历史评估/路线图不为局部实现重写。

文档校验命令：契约脚本、descriptor 基线、Ruby 对照、OpenAPI/AsyncAPI lint、panel check:contracts、Markdown 链接/UTF-8、git diff --check；结果如上。最终接续点是等待真实环境后的 T07b → T11b，而非再次从 T08b 开始。

## 2026-10-30：T08b 配置下发闭环补齐

状态：**已完成并通过本机验收**。本记录补齐上一轮“配置仅持久化、SDK 尚未消费”的明确限制；此前验收记录保留其当时范围。C++ Core / Python SDK、适配器 / TypeScript 面板的混合语言架构不变。T07b 和依赖真实环境的 T11b 按用户要求继续暂缓；本次结果不代表真实 QQ、模型或生产部署验收。

目标模块与允许文件：proto/kiarabot/v1/plugins.proto、api/openapi.yaml、core/ 配置投递/插件桥接/组合根/控制端口、control/api/ 映射及验收夹具、sdk/python/ 运行时和测试，以及直接相关文档。未改动适配器或基础设施实现，没有新增依赖、部署配置、数据库 schema 或生成物入库。

输入输出：已授权配置 PUT → 加密持久化 desired version/value → plugin.config.v1 协商 → PluginConfig 完整快照 → SDK on_config 回调 → PluginConfigAck → GET delivery 状态。PUT 和幂等重放只确认持久化；GET 分别报告 UNAVAILABLE、PENDING、APPLIED、REJECTED 与当前连接的 applied_version。保留既有配置冲突、权限和秘密字段拒绝行为。ACK 错误为已有稳定错误码，不新增 HTTP/gRPC 映射。

实现和验收条件：

- [x] 只有协商能力的认证连接收到自己的配置；旧客户端继续使用原事件流程，API 不宣称其已应用。
- [x] 每连接一条在途配置，随机 delivery_id + 会话 fence 隔离确认；更新合并到最新版本，旧 ACK 不确认新版本。
- [x] Core 十秒确认期限，SDK 配置回调上限五秒并受 hook_timeout 限制；拒绝/超时保留旧成功快照，同连接不自动重试失败版本。
- [x] Core 发现新版本后暂停新事件投递；配置/事件回调串行，Host RPC 接收独立。副作用不能自动回滚，插件回调须幂等。
- [x] SDK 上下文返回配置副本，重复当前帧不会重复调用回调；错误仅保留稳定码、retryable 和宿主关联 ID，不输出异常自由文本。
- [x] 离线写入、运行中热更新、拒绝/超时后新版本恢复、重复请求、停用、断线/重启重新应用均已验证；当前连接证据不跨进程持久化。
- [x] 存储故障不回退为空配置，旧连接/错误版本/伪造确认被拒绝；配置未确认或被拒绝时事件保留待投递。

实际验证：

- `cmake --preset proto -DKIARABOT_ENABLE_INFRASTRUCTURE=ON`、`cmake --build --preset proto --parallel 6` 通过，无新增编译警告。
- `KIARABOT_ADAPTER_DIR=<适配器检出绝对路径> bash tests/acceptance/run.sh` 完整通过：真实 HTTP/Core/SDK 控制面验收 **17/17**、proto CTest **24/24**、SDK **66/66**、适配器 **34/34**，API/Core 权限契约对照通过。
- 以上为本机历史记录；仓库当前 HEAD 无法在本机复跑（缺工具链），远端 CI 尚未通过，数字待 CI 复跑校准。
- `cmake --preset dev`、`cmake --build --preset dev --parallel 6`、`ctest --preset dev` 通过 **5/5**。
- panel `npm run generate && npm run check:contracts && npm run lint && npm test && npm run build` 通过，测试 **7/7**；生成文件位于忽略目录，没有提交。
- `proto/tests/check_contracts.sh`、`scripts/ci/check-descriptor.sh HEAD`、`scripts/quality/check-openapi.sh` 通过；descriptor 对照仅针对本地 HEAD，不冒充发布版兼容认证。
- 本次涉及的 **12 个 C++ 文件**通过 `clang-format --dry-run --Werror`；`git diff --check` 通过。全仓既有格式问题不在本次范围。
- 未运行 clang-tidy：本机无该工具。未新增依赖或变更基础设施实现，因此没有重复独立基础设施构建；它们已由完整 proto CTest 回归。远端 CI 当时未运行；后续远端 CI 已运行 5 次但均未通过（截至 `e70de83`），且自 `16191cb` 起暂停自动触发。未运行 Docker/生产环境及 T07b/T11b 验收。

文档影响判断与同步：

- 已更新唯一来源 `proto/kiarabot/v1/plugins.proto`、`api/openapi.yaml`，同步 `proto/README.md`、`doc/01-contracts.md`、`doc/01-runtime-semantics.md`、`doc/02-core.md`、`doc/03-plugin-sdk.md`、`doc/06-control-api.md`、`doc/08-operations.md`、`sdk/PLUGIN_SDK_DEPENDENCIES.md`、`sdk/python/README.md`、`control/api/README.md`、`tests/acceptance/README.md` 与本台账。
- 无需修改 `api/asyncapi.yaml` 或 `doc/07-control-panel.md`：未增加 WebSocket 事件/面板操作，配置应用状态通过 GET 查询，现有 plugin.updated 继续通知期望状态变化。面板类型重新生成和现有交互回归已验证兼容。
- 无需修改 `doc/00-boundaries.md`、`doc/04-nonebot-adapter.md`、`doc/05-infrastructure.md`、`doc/11-toolchain.md`、`doc/12-ci-cd.md`、部署脚本或路线图：模块/语言边界、适配器行为、存储格式、依赖和 CI 入口均未变化；现有统一入口自动纳入新测试。同期其他工作改动予以保留。
- 文档校验包括上述协议生成/descriptor、OpenAPI lint、API 权限对照、panel check:contracts，以及本次文档 UTF-8/本地 Markdown 链接检查和 git diff --check，均通过。没有未验证的新增契约字段。

## 2026-10-30 现状

- **远端 CI**：GitHub Actions 历史上 5 次运行均未通过（4 次失败、1 次取消；HEAD `e70de83` 仍有 4 个 job 失败）。自提交 `16191cb`（2026-10-30）起 `.github/workflows/ci.yml` 暂停自动触发，仅保留手动 `workflow_dispatch`；恢复自动触发前需先让手动运行变绿。具体失败根因尚未确认，本文档不推测。
- **部署骨架不可运行**：`deploy/compose.*.yml` 的 `HEALTH_PORT` 没有代码读取；适配器/面板的 8081/8082 HTTP 健康服务尚未实现；默认 `Dockerfile` 只构建 dev Core，未启用 proto/infrastructure，因此不提供 `/health/live`、`/health/ready`；staging/prod compose 的镜像 digest 仍是全零占位；`scripts/deploy/deploy.sh start`/`upgrade` 的成功路径尚不可用，T11b 未验收。
- **受保护基线差异**：`doc/18-architecture-baseline.md` 附录 A 仍记载“仓库没有 `adapters/`、适配器暂存于同级检出 `../kiarabot-adapter-nonebot/`，规划迁入 `adapters/nonebot/`”，与适配器已入仓 `adapters/nonebot/` 的事实不符。该文档由人类维护、AI 只读；本台账不修改 doc/18，差异由人类维护者裁定。

## 2026-10-30 本轮 CI/测试/文档修整记录

CI 修复（本轮只改工作流，未复跑；不能据此宣称 CI 已绿）：

- 工作流 YAML 解析改用 python3-yaml，不再依赖 ruby/psych。
- 密钥扫描失败时上传 `gitleaks-report.json` 工件；新增根目录 `.gitleaks.toml`（`useDefault = true`），只允许测试占位值（`secret-key-must-not-leak`、`platform-test-token`、`mock-access-token-*` 等），真实凭据仍在扫描范围。
- 闭环验收接入适配器锁定环境；CI 设置 `KIARABOT_ADAPTER_DIR` 并以 `bash -x tests/acceptance/run.sh` 运行，`tests/acceptance/run.sh` 增加分阶段 ERR trap 以定位失败阶段。
- clang-tidy job 改为分析 `build/dev/compile_commands.json` 中 dev 预设的全部翻译单元（`clang-analyzer-*`，warning 即失败）。
- 新增对跟踪 `.sh` 文件的 `bash -n` 语法检查。
- 自动触发仍暂停（仅 `workflow_dispatch`）；需手动 Run workflow 验证全绿后再恢复自动触发；不得写成 CI 已通过。

测试与工具链接线：

- 新增 `infrastructure/tests/http_model_provider_test.cpp`（Provider 配置校验边界与回环 HTTP 响应映射），在 `infrastructure/CMakeLists.txt` 注册为独立基础设施构建的测试目标；本机无 CMake/依赖，未运行。
- `core/CMakeLists.txt` 注册 `deploy_deployment` CTest（`scripts/deploy/tests/deployment_test.py`，5 个用例，TIMEOUT 30）。
- `scripts/check-toolchain.sh` 以 `-DKIARABOT_ENABLE_INFRASTRUCTURE=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` 配置 proto 预设，同时执行 proto CTest 与独立基础设施构建/测试两组。
- `scripts/quality/check-tidy.sh` 增加 compile_commands 覆盖校验，待分析文件缺少编译条目时直接失败。

解耦：

- `IControlApplication` 新增 `readiness()` 与 `stream_snapshot()`（默认分别返回 503 语义与不支持）；`control/api` 的 http_mapper/http_server 去除 `dynamic_cast<ControlService>`，行为不变。

文档同步：README、doc/01、doc/02、doc/05、doc/08、doc/11、doc/13、doc/15、doc/16、doc/17、doc/README。

未验证：本机无 cmake/clang-tidy/uv/node，构建、测试与部署未复跑；远端 CI 未复跑。不得宣称 CI 已绿或上述测试已通过。

## 2026-10-31 contracts 分支：契约门禁与“契约 ≥ 实现”收敛

状态：**契约/测试/文档修改完成；本机可运行门禁通过，依赖构建产物的门禁未运行**。分支 `kiarabot-contracts`（基线 `8caecb1`）。只修改 `proto/`、`proto/tests/`、`api/`、`doc/01-*.md`、`proto/README.md`、本台账 contracts 条目；未修改任何实现目录（`core/`、`adapters/`、`sdk/`、`control/`、`infrastructure/`、`panel/`、`deploy/`、`.github/`、`scripts/ci/`），未修改受保护 `doc/18`。

目标与输入输出：

- 目标：让契约门禁有判别力，并消除契约文件对实现能力的过度宣称，同时补齐 v1 契约缺项。
- 输入：`reports/modules/contracts.md` 的 P0-1/P0-2/P1 缺口与 `proto/`、`api/`、`doc/01-*` 现状。
- 输出：可执行门禁（枚举全量+数值基线、契约→实现状态清单、语义规则检查、旧夹具接入）、状态登记表 `proto/tests/contract_status.json`、API/文档收敛、跨模块语义决定清单。
- 错误语义：缺少 `protoc`/descriptor 工具/Python SDK 生成包时门禁退出码 `2`；状态登记缺失、未实现项缺少 `CONTRACT_STATUS` 注释或文档状态行、API 的 `x-contract-status` 与登记表不一致、OpenAPI 枚举超出实现集合、证据路径不存在时退出码 `1`。

契约/行为变化：

- 兼容新增：`common.proto` `ShutdownNotice`，`CoreToAdapter.shutdown=5`、`CoreToPlugin.shutdown=8`；状态 `not_implemented`，旧客户端按未知字段忽略。既有字段编号/数值/枚举未改。
- 收敛：OpenAPI `PluginManifest.runtime` 只留 `python-rpc`、`isolation` 只留 `process`，补齐 `entrypoint`/`capabilities`/`dependencies`/`config_schema`/`permissions` 并标注不支持字段；`admin` 角色改为 16 项显式权限（无 `*`，仍无 `message.read`）；`/actions/{actionId}`、`/actions/by-idempotency-key`、`/groups` 标记 `x-contract-status: not_implemented` 并写明本应语义与当前恒 `NOT_FOUND`/空页；Action 查询冻结 604800 秒终态保留、`EXPIRED` + `ERROR TIMEOUT`、缺失/过期/不可见 `NOT_FOUND`。
- AsyncAPI：`/ws/messages` 与 `message.updated` 明确 `not_implemented`（无投影/无生产者/无 `message.read` 授予路径，订阅 4403）；`health.updated` 标记未实现；`plugin.updated.health_state` 标注 v1 不产生；补 `slow_consumer: 1013`；securityScheme 注明首帧 in-band 认证。
- 文档：`doc/01-contracts.md` 增“契约实现状态”表、“权限/能力/订阅词表”、“标识符语法”、“优雅停机与 drain”、reserved/deprecated 规则，修正 gRPC 业务错误映射为 in-band；`doc/01-runtime-semantics.md` 增 Action REST 查询与 `ActionRouter::lookup` 的保留/EXPIRED 差异、背压分歧、drain 未知结果规则。

实际运行的检查（本机）：

- `python3 proto/tests/check_enum_consistency.py`：通过（10/10 枚举，名称/数值/顺序+API 词表）。
- `python3 proto/tests/check_implementation_status.py`：通过（66 消息/261 字段覆盖、API 扩展、证据、文档状态表、词表）。
- `python3 proto/tests/check_semantic_rules.py`：通过（ID/deadline/gRPC/reserved/drain/Action 查询/角色/manifest）。
- `python3 scripts/quality/check-boundaries.py`：通过。
- `ruby -c proto/tests/api_contract_test.rb`：语法通过；另将文件截断到 Core `--dump-policy` 之前实际运行了契约形状部分（频道/状态/角色/manifest/Action 保留期/运行时语义断言），通过——完整 Core 策略对照仍未运行。
- `ruby -ryaml`：OpenAPI/AsyncAPI 可解析；新增 JSON 可解析；`bash -n` 覆盖 `proto/tests/*.sh`；`git diff --check` 通过。
- 门禁判别力负例（临时副本，运行后清理）：枚举重编号、删除 `CONTRACT_STATUS` 注释、恢复 admin 通配、PluginManifest 增加 `cpp-rpc`、删除文档状态表行、删除频道 `x-contract-status` 均以退出码 1 失败；未修改仓库文件。
- **未运行**：`./proto/tests/check_contracts.sh` 全流程、`descriptor_compatibility_test.sh` 6 负例、`action_contract_test.py`、`ruby proto/tests/api_contract_test.rb` 完整版、`protoc`/descriptor 生成、OpenAPI lint（redocly）、AsyncAPI parser、CTest/CMake 构建、`roundtrip.cc`（含新增 drain 帧断言）——本机无 protoc/cmake/node/uv/redocly/asyncapi parser 及 build 产物，故不能声称通过；CI 自动触发仍暂停，需人工 `workflow_dispatch` 复跑校准。

跨模块交接（本分支不改实现）：

- core：REST Action 记录 owner/168h 保留/EXPIRED；deadline ≤300 秒；target_source 字符集与入口统一；队列满返回可重试 `RATE_LIMITED`；Host 未知 operation 返回 `CAPABILITY_NOT_SUPPORTED`；关闭前发送 `ShutdownNotice`。
- control-api：按 OpenAPI 新语义实现两个 Action 查询与 `/groups`；messages 频道保持 4403/停用声明；读取显式 admin 权限表。
- adapter-nonebot：消费 `heartbeat_interval_seconds`；补 `MessageSent` 等生产端（当前契约标记未实现）；消费 `ShutdownNotice`（未收到时按未知结果）。
- plugin-sdk：未知枚举值保留、冻结词表、drain 客户端流程；`doc/03-plugin-sdk.md` 更宽词表按本契约 v1 子集校正。
- panel：不得依赖 `message.updated`；权限展示改用显式 admin 列表。
- ci-cd：确认 `check_contracts.sh` 新依赖（descriptor 兼容工具、SDK 生成包）在 contracts job 中可用；恢复自动触发前先手动跑绿。

未完成/风险：未运行项如上；`roundtrip.cc`/`api_contract_test.rb` 的新增断言未在本机执行；新增 `ShutdownNotice` 帧与 OpenAPI/AsyncAPI 变更尚未经远程 CI 验证；`doc/06-control-api.md`、`doc/03-plugin-sdk.md` 等接口文档按本任务范围未更新（已列为交接），与 `api/` 的同步需由对应分支完成。本记录不改变 `doc/18` 受保护基线，其附录 A 的既有差异仍只报告不修改。

## 2026-10-31 W2 core-runtime 分支：维护循环、可配置容量、ACK 超时与冲突重试

状态：**实现与测试完成；dev/full 构建与 CTest 在本机通过**。分支 `kiarabot-core-runtime-w2`（基线 `7235b07`，作为 `kiarabot-core-runtime` 的 W2 提交推送）。只修改 `core/`、`doc/02-core.md` 与本台账；未修改 `proto/`、`api/`、`adapters/`、`sdk/`、`panel/`、`control/`、`infrastructure/`、部署与 CI 文件，未修改受保护 `doc/18`。

目标与输入输出：

- 目标：消除上一轮 `reports/modules/core-runtime.md` 的 P0/P1 阻断项——终态记录与审计无清理导致写路径永久失败、容量上限硬编码、插件 ACK 无超时、桥接层冲突即断连、`stream_snapshot` 未判空。
- 输入：主进程装配的 `RuntimeStore` 与新增环境变量；插件事件 ACK、适配器 Action 结果、Host 请求。
- 输出：单事务维护结果（删除计数/存储错误）、可配置容量与超时、结构化 `operation=maintenance` 日志、稳定错误码；非法配置 `INVALID_ARGUMENT` 退出 1。
- 错误语义：维护提交失败返回存储错误且不改变持久状态，按周期重试；审计达到硬上限仍返回可重试 `UPSTREAM_UNAVAILABLE`；维护的 revision 冲突不自动重试、不阻塞业务。

实现要点：

- 新增 `RuntimeMaintenance`（`core/include|src/application/runtime_maintenance.*`）：终态事件+逐插件投递与终态 Action 按保留期删除；审计按保留期并裁剪到上限的五分之四；命令幂等记录按确定性数量上限裁剪；过期 namespace TTL 与过期 Host receipt 清理；只在实际删除时提交。
- 组合根新增维护线程（默认 60 秒、启动时先跑一次）与六个可配置环境变量：`KIARABOT_EVENT_QUEUE_LIMIT`、`KIARABOT_AUDIT_LIMIT`、`KIARABOT_ACTION_LIMIT`、`KIARABOT_ACK_TIMEOUT_MS`、`KIARABOT_MAINTENANCE_INTERVAL_MS`、`KIARABOT_RETENTION_HOURS`；`RuntimeLimits` 注入 `ActionRouter`/`PluginHub`/`TaskScheduler`/`HostDispatcher`/`ControlService`，不再一律使用默认值。
- 插件 ACK 超时：`dispatched` 超过 `KIARABOT_ACK_TIMEOUT_MS` 未确认按一次可重试失败处理，达到上限进入死信，解除队列阻塞。
- 桥接冲突重试：事件 ACK、配置 ACK、Action 提交/查询/取消、适配器 Action 结果、Host 请求对 `retryable=true` 的 `CONFLICT` 做 3 次 20ms 重试；预算耗尽按原断连语义。
- 控制应用 `stream_snapshot` 增加用户查找失败的防御性判断；`InMemoryRuntimeStore` 增加冲突注入与并发保护，仅用于测试。

测试与检查（本机实际运行）：

- `cmake --preset dev && cmake --build --preset dev --parallel 4 && ctest --preset dev --no-tests=error`：**9/9 通过**。
- `cmake --preset full && cmake --build --preset full --parallel 4 && ctest --preset full --no-tests=error`：**28/28 通过**（含新增 `core_runtime` 维护用例、`action_router` 容量用例、`plugin_hub` ACK 超时与冲突重试用例、`core_grpc_process` 非法/合法配置用例）。
- 全部改动文件 `clang-format --dry-run --Werror` 通过；构建无新增告警。

文档影响判断：已更新 `doc/02-core.md`（维护循环、容量/超时配置、失败语义、ACK 超时与冲突重试、未完成项）与本台账。未更新 `doc/01-runtime-semantics.md`（ACK 超时/维护周期属运行语义，需由 contracts 分支或后续 core 任务同步）、`doc/08-operations.md`（新增环境变量需运维文档同步）、`doc/05-infrastructure.md`（时间型命令幂等过期需快照格式版本升级）——均列为交接项；`proto/`/`api/` 无变更。

交接与未完成：`control/api` 的 `/groups` 仍无数据源（契约已标 `not_implemented`）；时间型命令幂等过期需基础设施快照格式升级；多适配器/多插件装配、完整 RBAC 与 `message.send` 速率/内容策略不在本轮；任务每插件上限仍固定 1024。

## 2026-10-31 W2 基础设施可靠性补强记录

范围：`infrastructure/`（include/src/tests）、`doc/05-infrastructure.md`、本条台账。未改 `core/`（含 `core/ports`）、`proto/`、`api/`、`deploy/`；`doc/18` 只读未改。本记录不代表 CI 已绿。

- P1-1 容量与失败语义：新增 `infrastructure/include/kiarabot/infrastructure/snapshot_limits.hpp`（64 MiB / 1,000,000 项公开上限 + `SnapshotLimits`/`SnapshotRejections`），编解码新增独立 `SnapshotLimitError`；`SqliteRuntimeStore::limits()`/`rejected_snapshots()` 暴露上限与单调拒绝计数。语义：超限 commit 仍返回 `Unavailable` 且持久状态不变（可缩容重试）；超限存量载荷返回 `Corrupt`。新增测试 `snapshot_limits`（字节超限、百万项超限、超大存量载荷、失败后重试、计数与重开清零）。
- P1-2 崩溃/半写恢复：新增 `crash_recovery_test`：子进程持续提交 12 MiB 快照，父进程在 1–200 ms 不同延迟 SIGKILL（可能落在提交中），随后重开数据库断言最后已提交快照可解码、`PRAGMA integrity_check=ok`、可继续提交。不依赖真实断电；生产备份/恢复演练仍归 T11b。
- P1-3 migrate CLI：新增 `migrate_cli_test`（CTest `migrate_cli`）直接调用构建产物：缺参数/`:memory:`/`file:` 退出 2；首次与重复迁移退出 0；v1 旧库迁移成功；未知 schema 与损坏载荷退出 1 且不改写数据库；持有写锁 1.5 秒后释放时 CLI 重试成功；两实例并发启动都成功；父目录缺失退出 1（依赖故障，不是参数错误）。
- P1-4 时区预检：新增 `TimeZoneResolver::zone_info_available()`（用真实 IANA 地区名探测，`UTC` 可能内建于 Abseil）与可注入 availability 探针；zone 数据不可用返回 `UPSTREAM_UNAVAILABLE`，仅名称/日期/DST 歧义等调用方错误返回 `INVALID_ARGUMENT`。新增 `time_zone_resolver_test`。**交接 core**：`core/app/main.cpp` 仍未在启动时调用预检（本分支不可改 core）。
- P1-5 日志字段：JSON 行新增 `timestamp`（与 `occurred_at` 同为 UTC ISO-8601 的 doc/08 别名）与 `resource_id`（`resource` 别名）；明确不输出独立 `event_id`/`action_id`（Core `RuntimeLog` 无此字段，事件/动作标识在 `resource`）、不输出 `message`/`error_code`（sink 不收自由文本，`outcome` 即稳定码）。`BufferedRuntimeLogSink::write_failures()` 记录写失败；**交接 core/运维**：组合根尚未导出 `write_failures()`/`sink_failures()` 到健康或指标。
- P1-6 端口处置：`doc/05` 新增「端口覆盖现状」并修正历史草案表述——`INamespaceStore`/`IIdempotencyStore`/`IAuditLogger`/`IUnitOfWork(Factory)` 无实现且不使用（由 `RuntimeSnapshot` + `HostDispatcher` 组合事务承担）；`IFileStorage` 已实现但 Core 无调用链、组合根未接线；`IScheduler` 由 Core `TaskScheduler` 实现；`IInboxRepository`/`IOutboxRepository`/`IActionRepository` 从未在 `ports.hpp` 定义。**交接 core**：不删除/不修改 `core/ports` 接口；若启用这些扩展端口，由 core 先定事务所有权与错误语义。
- P1-7 文件键长：`valid_key` 上限由 256 收敛为 255 字节（POSIX `NAME_MAX`），256 字节键在 syscall 前返回 `INVALID_ARGUMENT`；`file_storage_test` 补 255 字节 put/get/erase 与 256 字节拒绝（FileStorage 与 FakeFileStorage 都覆盖）。
- 模型 provider 测试：W1 修复后 `http_model_provider` 在独立 infra 构建与 `full` 预设中实际执行并通过（此前从未运行）；真实上游/TLS 仍属 T07b 未验收。
- 文档影响判断：端口观测行为、日志 JSON 字段、文件键校验、迁移/时区错误语义与测试接线变化，更新 `doc/05-infrastructure.md`；本条记录 `doc/16`。`doc/08` 的迁移退出码/日志字段运维口径差异不在本分支允许修改范围（交接 ops）；`doc/18` 只读未改。

实际运行的命令与结果（本机 2026-10-31，cmake 4.4.4 / protoc 36.2 / local clang-format 23；与 CI 钉住的 clang-format-16 不同，不能据此宣称 CI 通过）：

- `cmake -S infrastructure -B build/infrastructure -DCMAKE_BUILD_TYPE=Debug && cmake --build build/infrastructure --parallel 4 && ctest --test-dir build/infrastructure --output-on-failure --no-tests=error`：configure/build 成功，**9/9 测试通过**（time_zone_resolver、http_model_provider、file_storage、sqlite_runtime_store、snapshot_limits、crash_recovery、authorized_commands、runtime_log、migrate_cli），总耗时 6.83 s。
- `cmake --preset full && cmake --build --preset full --parallel 4 && ctest --preset full --no-tests=error`：configure/build 成功，**32/32 测试通过**（含 9 个 infra 测试与全部 core/control/proto/部署脚本测试），总耗时 22.80 s。
- `clang-format --dry-run --Werror`（本机 clang-format 23）检查本分支改动的 C++ 文件：新增/修改文件通过；唯一报错是 `infrastructure/src/file_storage.cpp:83` 的既有排版，7235b07 基线同样报错（未顺手格式化，按仓库规则保留；CI 使用 clang-format-16，是否同样报错未复核）。
- 未运行：远端 CI（自动触发仍暂停，未手动 dispatch）；真实模型上游/TLS（T07b）；生产部署/备份恢复演练（T11b）；Docker 构建；Windows 构建。`doc/08` 未同步（超出本分支允许范围）。

剩余与交接：

- core：启动时调用 `TimeZoneResolver::zone_info_available()` 预检并 fail fast；导出 `sink_failures()`/`write_failures()`（或接入健康/指标）；若启用 `IUnitOfWork`/独立仓储/审计端口，先定事务语义再要求 infrastructure 适配器。
- ops：`doc/08` 的“非法路径退出 2”与实测（路径不存在退出 1）对齐；日志通用字段（event_id/action_id/message/error_code）与 sink 实际字段的差异同步；生产备份/恢复演练（T11b）。
- 风险：容量上限仍无监控导出（仅 API 计数）；`create_schema` 之外没有性能/容量压测；崩溃测试是 SIGKILL 模拟而非断电；`file_storage.cpp` 既有格式问题可能被 CI clang-format-16 判失败（待 CI 校准）。
