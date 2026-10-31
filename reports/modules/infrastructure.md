# 基础设施（kiarabot-infrastructure）覆盖与缺口分析

> 分析对象：main@8caecb1；静态分析，未构建/未运行测试（本机无 cmake/node/uv/protoc/docker），未联网、未安装依赖。唯一写操作是本报告；仓库其他文件未改动。

## 1. 范围与规模

- 目录/文件：`infrastructure/`（`include/kiarabot/infrastructure/`、`src/`、`tests/`、`CMakeLists.txt`），共 **18 文件 / 2006 行**；其中测试 5 文件 / **738 行**，生产代码+构建 **1268 行**。
- 规模明细：`sqlite_runtime_store.cpp` 239、`runtime_snapshot_codec.hpp` 282、`file_storage.cpp` 203、`http_model_provider.cpp` 200、`runtime_log_sink.cpp` 64、`time_zone_resolver.cpp` 42、`migrate.cpp` 29、`CMakeLists.txt` 90；测试 `http_model_provider_test` 330、`sqlite_runtime_store_test` 198、`authorized_commands_test` 99、`runtime_log_test` 58、`file_storage_test` 53。
- 关键入口与代码地图：
  - `SqliteRuntimeStore::open/read/begin`：`infrastructure/src/sqlite_runtime_store.cpp:134-238`；schema 创建/迁移 `:144-211`；乐观 revision 提交 `:91-120`；载荷解码 `:55-82`。
  - 私有快照编解码（版本化、非线协议）：`infrastructure/src/runtime_snapshot_codec.hpp:64-281`。
  - `kiarabot-migrate DATABASE_PATH`：`infrastructure/src/migrate.cpp:6-28`。
  - `HttpModelProvider`：`infrastructure/src/http_model_provider.cpp:79-199`；配置校验 `:29-49`。
  - `TimeZoneResolver`：`infrastructure/src/time_zone_resolver.cpp:6-41`。
  - `FileStorage` / `FakeFileStorage`：`infrastructure/src/file_storage.cpp:68-201`（接口 `file_storage.hpp:14-37`）。
  - `BufferedRuntimeLogSink`：`infrastructure/src/runtime_log_sink.cpp:18-63`。
  - 组合根接线（Core 的 `main.cpp`）：`core/app/main.cpp:99`（sink）、`:123-142`（SQLite + Logging 装饰器）、`:186-207`（模型）、`:208-218`（时区→TaskScheduler）、`:229`（日志回调）、`:280-295`（调度线程）。
  - 构建入口：`infrastructure/CMakeLists.txt:32-90`；根构建开关 `CMakeLists.txt:120-125`。
- 对应设计文档：`doc/05-infrastructure.md`、`doc/00-boundaries.md`（基础设施职责）、`doc/18-architecture-baseline.md:184-209,417`（只读基线）；历史快照 `doc/13-infrastructure-integration-follow-up.md`、`doc/14-implementation-assessment.md:25,37`、`doc/15-cross-layer-gaps.md:16,17,24,25`；台账 `doc/16-development-tasks.md`（T05a/T05b/T06b/T06c/T10b/T11a 及 2026-10-30 记录）；运维 `doc/08-operations.md`、工具链 `doc/11-toolchain.md`、CI `.github/workflows/ci.yml`。

## 2. 模块职责（依据）

- 基线自述职责：`doc/00-boundaries.md` 定义 `infrastructure/` 实现 Core 在 `core/ports/` 定义的数据库、模型、调度、审计和测试替身，不决定业务规则、不接收插件直接调用；依赖规则 `core/ports ← infrastructure`。`doc/18:184-209` 第六层为基础设施适配器（IMessageBus/IModelProvider/IConversationStore/IUserRepository/IPermissionStore/IScheduler/IFileStorage/IAuditLogger，具体实现可为 SQLiteAdapter/PostgresAdapter/OpenAICompatibleModelAdapter/RedisAdapter/CronSchedulerAdapter）；`doc/18:417` 明确当前只实现 SQLite RuntimeStore、HTTP 模型 Provider、时区解析、文件存储，Postgres/Redis/CronScheduler 未实现（调度由 Core TaskScheduler 承担）。
- `doc/05` 自述职责：把 Core 端口连到具体数据库、模型、缓存、文件系统和调度器；不实现消息路由、插件权限或面板逻辑（`:7-11`）；给出目标目录（`:13-38`，实现已收敛到 `src/`/`include/`）；“第一批端口”7 个（`:40-50`）；Core 端口约定（`:52-54`）；存储适配器“基础设施必须实现 Core 定义的接口”（`:89-98`）；迁移锁/同步/冲突语义（`:100`）；文件存储语义（`:102-104`）；模型非流式语义（`:58-68`）；时区/IANA 语义（`:116`）；T05a/T05b/T10a/T10b/T11a 细节（`:159-188`）。
- 契约来源：**不存在 infra 专属线协议**；端口来自 `core/include/kiarabot/core/ports/ports.hpp:23-137` 与 `core/include/kiarabot/core/ports/runtime_store.hpp:15-28`，日志端口来自 `core/include/kiarabot/core/runtime/runtime_log.hpp:20-38`；快照编码是私有磁盘格式（`runtime_snapshot_codec.hpp:43` 注释）。`doc/05:54` 亦声明“接口的存在不表示适配器或插件 Host 服务已经接入”。

## 3. 已实现能力（逐条带证据）

| # | 能力 | 证据（file:line） | 对应测试 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | SQLite 单行快照存储 `RuntimeStore::read/begin` + 条件 UPDATE 提交 | `sqlite_runtime_store.cpp:55-82,91-120,221-238` | `sqlite_runtime_store_test.cpp:100-141` | 完整（历史台账称通过；本轮未运行） |
| 2 | schema v5 创建（单行表 + `PRAGMA user_version=5`，单事务） | `sqlite_runtime_store.cpp:144-176` | `sqlite_runtime_store_test.cpp:101`（新库） | 完整 |
| 3 | v1/v2/v3/v4 → v5 快照迁移（decode→encode→同事务回写） | `sqlite_runtime_store.cpp:177-199`；编解码版本门 `runtime_snapshot_codec.hpp:214-224,241-246,265-281` | `sqlite_runtime_store_test.cpp:60-99` | 完整 |
| 4 | 未知 schema / 损坏载荷 / 缺表拒绝打开 | `sqlite_runtime_store.cpp:159-207`（`version!=5`→Corrupt；`load` 失败→Corrupt） | `sqlite_runtime_store_test.cpp:174-178` | 完整 |
| 5 | 并发提交冲突（revision CAS，`sqlite3_changes()!=1`→Conflict） | `sqlite_runtime_store.cpp:91-120` | `sqlite_runtime_store_test.cpp:134-160` | 完整 |
| 6 | 外部锁/SQLITE_BUSY/LOCKED → Conflict（不自动重试） | `sqlite_runtime_store.cpp:13-23` | `sqlite_runtime_store_test.cpp:161-173` | 完整 |
| 7 | 依赖故障显式返回（目录不存在→Unavailable；文件/目录被删提交失败） | `sqlite_runtime_store.cpp:137-141,96-119` | `sqlite_runtime_store_test.cpp:179-190` | 完整（删除场景细节见 §8） |
| 8 | 私有文本快照编码：二进制 envelope、纳秒时间、版本门、重复键/尾随数据/枚举范围校验 | `runtime_snapshot_codec.hpp:37-191,259-281` | 经 #3/#5 间接覆盖 | 完整 |
| 9 | 快照容量上限（读 64 MiB、提交 64 MiB、集合合计 1,000,000 项、`SQLITE_LIMIT_LENGTH`） | `sqlite_runtime_store.cpp:12,143`；`runtime_snapshot_codec.hpp:67-73,156-185` | 无 | 部分（仅代码，无测试） |
| 10 | 迁移命令：参数校验、30s 冲突重试、退出码 0/1/2、稳定输出 | `migrate.cpp:6-28` | 无 | 部分（实现完整，无自动化测试） |
| 11 | 非流式 HTTP 模型调用：OpenAI 兼容 Chat Completions，`stream=false` | `http_model_provider.cpp:111-122,160-196` | `http_model_provider_test.cpp:240-259`（从未执行） | 未验证（代码完整） |
| 12 | 模型配置边界：HTTPS 或 127.0.0.1 HTTP、禁 userinfo/query/fragment、key 字符集 | `http_model_provider.cpp:29-49` | `http_model_provider_test.cpp:181-208` | 未验证（测试从未执行） |
| 13 | 模型请求/响应限额与畸形拒绝：64 KiB、128 消息、1 MiB 响应、64 KiB 文本、tool_calls、choices 数 | `http_model_provider.cpp:50-61,107-110,173-183` | `http_model_provider_test.cpp:210-238,277-314` | 未验证（测试从未执行） |
| 14 | 模型错误映射：429→RATE_LIMITED(retryable)、超时→TIMEOUT、其余→UPSTREAM_UNAVAILABLE；无自动重试 | `http_model_provider.cpp:14-20,160-168` | `http_model_provider_test.cpp:261-305`（429/500/超时） | 部分（retryable 标志、503、中断未断言；测试从未执行） |
| 15 | 单在途请求（`try_lock` 竞争→RATE_LIMITED） | `http_model_provider.cpp:97-100` | 无 | 部分（代码，无测试） |
| 16 | 密钥隔离：key 仅进 Authorization；错误只含稳定码；子进程环境白名单不含模型配置 | `http_model_provider.cpp:132-143`；`core/app/main.cpp:186-207,306-312`；`core/src/application/host_dispatcher.cpp:82-89` | 间接（`http_model_provider_test.cpp:254-258`） | 部分（未见端到端断言；测试未执行） |
| 17 | 时区解析：UTC/IANA 地区名、公历 1970–2100、拒绝非法日期/DST gap/fold/未知 zone | `time_zone_resolver.cpp:12-35` | `core/tests/task_scheduler_test.cpp:42-75`（仅在含 infrastructure 的 proto 构建中编译/运行） | 完整（历史台账称通过；本轮未运行） |
| 18 | POSIX 文件存储：键字符集/长度、O_NOFOLLOW、0600 临时文件+fsync+rename 原子替换、删除幂等 | `file_storage.cpp:36-47,70-161`；`file_storage.hpp:12-24` | `file_storage_test.cpp:13-52` | 完整（测试用 assert，Release 下会失效，见 §6） |
| 19 | `FakeFileStorage` 单次故障注入 | `file_storage.cpp:163-201`；`file_storage.hpp:26-37` | `file_storage_test.cpp:49-51` | 完整 |
| 20 | 有界结构化日志 sink：互斥、10 分钟/4096 条、单行 JSON、输出失败返回 false | `runtime_log_sink.cpp:24-63`；`runtime_log_sink.hpp:10-21` | `infrastructure/tests/runtime_log_test.cpp:17-54` | 完整（4096 上限未测；daemon 未接 stdout，见 §5） |
| 21 | Core 日志装饰器：提交后观察、fail-open、失败计数、日志脱敏由 Core 侧完成 | `core/src/runtime/runtime_log.cpp:37-140`；`core/app/main.cpp:141-142,229` | `runtime_log_test.cpp:33-51` | 完整（`sink_failures()` 无出口） |
| 22 | 构建与测试接线：独立 infra 构建 + 可选根构建（`KIARABOT_ENABLE_INFRASTRUCTURE`） | `infrastructure/CMakeLists.txt:1-90`；`CMakeLists.txt:120-125` | 5 个 CTest 目标（见 §6） | 完整（配置层面；本机未构建） |

补充（逐题回答）：

- **端口→实现覆盖表**（`core/include/kiarabot/core/ports/` 全部接口）：

| 端口 | 声明 | infrastructure 实现 | 其它实现/承载 | main.cpp 接线 | 结论 |
| --- | --- | --- | --- | --- | --- |
| `INamespaceStore` | `ports.hpp:23-32` | 无 | 功能由 `RuntimeSnapshot.namespaces` + `HostDispatcher` 直接读写快照承担（`core/src/application/host_dispatcher.cpp:147,164-208`） | 不适用 | **端口无实现、无消费者（死接口）** |
| `IIdempotencyStore` | `ports.hpp:40-47` | 无 | 由 `RuntimeSnapshot.host_receipts`（T06a）承担（`core/src/application/host_dispatcher.cpp:147`） | 不适用 | **端口无实现、无消费者** |
| `IAuditLogger` | `ports.hpp:125-129` | 无 | 由 `RuntimeSnapshot.audit`（`RuntimeAuditEntry`，非协议 `AuditRecord`）承担（`core/src/application/host_dispatcher.cpp:52-61`） | 不适用 | **端口无实现、无消费者** |
| `IUnitOfWork` / `IUnitOfWorkFactory` | `ports.hpp:52-69` | 无 | 由 `RuntimeTransaction` + 各应用服务组合承担；`doc/02-core.md:11` 称其为“后续扩展端口” | 不适用 | **端口无实现、无消费者** |
| `IModelProvider` | `ports.hpp:71-76` | `HttpModelProvider`（`http_model_provider.hpp:14`） | 测试 fake：`core/tests/model_host_test.cpp:17` | 条件接线：`core/app/main.cpp:186-207`（需 `KIARABOT_MODEL_*` 三者齐全且 `plugins` 存在） | 实现完整；测试从未运行 |
| `ITimeZoneResolver` | `ports.hpp:87-91` | `TimeZoneResolver`（`time_zone_resolver.hpp:6`） | 无 | 条件接线：`main.cpp:208-218`（`plugins` 存在时注入 Core `TaskScheduler`） | 已接线 |
| `IScheduler` | `ports.hpp:98-112` | **无**（不是基础设施实现） | Core `TaskScheduler`（`core/include/kiarabot/core/application/task_scheduler.hpp:7`） | `main.cpp:208-218,280-295` 直接使用 Core 类 | infra 无调度适配器（设计如此，见 §4 A22 与 §5） |
| `IFileStorage` | `ports.hpp:131-137` | `FileStorage` + `FakeFileStorage`（`file_storage.hpp:14,27`） | 无 | **未接线**：全仓除自身测试外无 `IFileStorage`/`FileStorage` 使用者（仅 `file_storage_test.cpp:13`） | **实现存在但未接线** |
| `RuntimeStore` / `RuntimeTransaction`（另一头文件） | `runtime_store.hpp:15-28` | `SqliteRuntimeStore`（`sqlite_runtime_store.hpp:6`） | Core `InMemoryRuntimeStore`（`in_memory_runtime_store.hpp:8`）、`LoggingRuntimeStore` 装饰器（`runtime_log.hpp:25`） | 条件接线：`main.cpp:123-142`（需 `KIARABOT_RUNTIME_DB`+`KIARABOT_ADAPTER_ACCOUNTS`+`--grpc-listen`），外层包 `LoggingRuntimeStore` | 已接线（条件性） |
| `IRuntimeLogSink`（`runtime_log.hpp`，非 ports.hpp） | `runtime_log.hpp:20-23` | `BufferedRuntimeLogSink` | 无（**只有内存实现**，无文件/轮转/持久 sink） | `main.cpp:99,141-142,229` | 已接线（内存缓冲；stdout JSON 参数未传，见 §5） |

- **哪些端口只有内存实现**：`IRuntimeLogSink` 仅 `BufferedRuntimeLogSink`（内存环 + 可选 `std::ostream`，组合根传 `nullptr`）；`IFileStorage` 的可用实现只有 POSIX 文件系统实现与内存 `FakeFileStorage`（后者仅测试）；命名空间/幂等/审计“端口”的持久承载实际只有 `RuntimeSnapshot` 快照这一种抽象（底层可为 SQLite 快照或内存快照），没有可替换的独立适配器。
- **未被 main.cpp 接线的实现**：`FileStorage`（完全无调用链）、`FakeFileStorage`（仅测试）、`kiarabot-migrate`（仅独立构建目标，默认镜像不打包，见 §4 A13 与 §5）、SQLite/模型/日志在非 `KIARABOT_ENABLE_INFRASTRUCTURE` 或非 proto 构建中编译为空（`CMakeLists.txt:120-125`，`core/app/main.cpp:126-133,191-206`）。
- **测试矩阵与 CI 归属**（正常/失败/并发/迁移/未纳入 CI）：
  - 正常路径：SQLite 读写/重启（`sqlite_runtime_store_test.cpp:100-133`）、模型 200 映射（`http_model_provider_test.cpp:240-259`）、文件存储 get/put/erase（`file_storage_test.cpp:13-25`）、日志 4 条结构化记录（`runtime_log_test.cpp:17-54`）。
  - 失败路径：SQLite 损坏/未知 schema/缺目录（`:174-190`）、模型 429/500/坏 JSON/超大/工具调用/超时（`http_model_provider_test.cpp:261-314`）、文件非法键/符号链接/注入故障（`file_storage_test.cpp:21,42-51`）、日志 sink 写失败（`runtime_log_test.cpp:47-50`）。
  - 并发路径：仅 SQLite 双事务 CAS 与外部 BEGIN IMMEDIATE/EXCLUSIVE（`sqlite_runtime_store_test.cpp:142-173`）；模型单在途竞争、文件并发、日志并发均无测试。
  - 迁移路径：v1/v2/v3/v4 各 1 例 + 重复打开（`:60-99`）；无 v0 首次建库的显式断言（仅间接 `:101`）、无迁移失败回滚断言、无 CLI 测试。
  - CI 归属：5 个 infra CTest 只在 `KIARABOT_ENABLE_INFRASTRUCTURE=ON` 构建中注册（`infrastructure/CMakeLists.txt:49-88`），dev 预设 CI job 不含它们；契约 job 通过 `tests/acceptance/run.sh:20` 的 `ctest` 覆盖 infra，但 CI 自动触发暂停且历史未绿（`doc/16:588`）；`kiarabot-migrate` 无任何测试目标；CI clang-tidy 不覆盖 infra（§6）。

## 4. 缺口清单

| # | 缺口 | 类型 | 严重度 | 证据（file:line / 文档行） | 影响 |
| --- | --- | --- | --- | --- | --- |
| A1 | `doc/05` “基础设施必须实现 Core 定义的接口”所列 `IUnitOfWork.begin/commit/rollback`、`IInboxRepository`、`IOutboxRepository`、`IActionRepository` 在 `ports.hpp` 中或有接口无实现、或根本不存在 | 契约不一致 + 未实现 | P1 | `doc/05:89-98`；`core/include/kiarabot/core/ports/ports.hpp:1-139`（无 Inbox/Outbox/ActionRepository）；`IUnitOfWork` `ports.hpp:52-69` 全仓无实现 | “替换数据库/只换适配器不改业务代码”的验收（`doc/05:143-153`）在当前抽象上不成立；命名空间/幂等/审计无法被独立适配器替换 |
| A2 | “第一批端口”7 个中 `IConversationStore`/`IUserRepository`/`IPermissionStore` 仅有文档名字，无接口无实现 | 未实现 | P2 | `doc/05:40-50`；`ports.hpp:1-139` | 文档承诺的端口清单比实际接口多，后续计划以文档为准会落空 |
| A3 | `INamespaceStore`/`IIdempotencyStore`/`IAuditLogger` 是死接口：无实现、无调用、无测试；实际能力内嵌 `RuntimeSnapshot` | 未接线 + 未验证 | P2 | `ports.hpp:23-47,125-129`；grep 全仓无 `public.*INamespaceStore` 等实现（仅 `ports.hpp` 定义） | 端口层噪声；若按端口替换实现会被误以为已插拔 |
| A4 | `IFileStorage`/`FileStorage` 完全未接线：Core 无任何文件存储用例，组合根未创建 | 未接线 | P1 | `infrastructure/include/.../file_storage.hpp:14-37`；`infrastructure/tests/file_storage_test.cpp:13` 是唯一使用者；`core/app/main.cpp` 未 include | 架构基线的 `IFileStorage` 无实际能力；文件存储的 POSIX 实现无法在集成层被验证 |
| A5 | 模型 Provider 仅 OpenAI 兼容非流式；无流式、无供应商抽象/注册表、无协议转换 | 半实现（已声明未来项） | P2 | `http_model_provider.cpp:111-122,173-195`（只认 `choices[0].message.content`/`usage`）；`doc/13:26` “流式模型仍未定义”；`doc/05:74-77` 职责列表仍写“流式响应转换…其他厂商协议转换” | 只支持 OpenAI Chat Completions 形态；流式属未定义契约（P2 延期，不是缺陷） |
| A6 | 模型无自动重试/退避、不解析 `Retry-After`；429/503 仅映射错误码 | 半实现（设计选择） | P2 | `http_model_provider.cpp:14-20,160-168`；`doc/05:64` 明确不自动重试 | 插件/SDK 必须自行退避；429 语义只在错误码上 |
| A7 | 模型边界测试为 HEAD 新增，**从未执行**：本机无工具链，CI 自 2026-10-30 暂停自动触发且历史 5 次运行均未通过 | 未验证 | P1 | `infrastructure/tests/http_model_provider_test.cpp:1-330`；`doc/05:68` 自述“该测试本轮未运行”；`.github/workflows/ci.yml:9-16`（仅 workflow_dispatch）；`doc/16:588` 记录 CI 未绿 | 模型适配器唯一的边界证据不可执行；“T06b 已完成并验收”（`doc/16:207`）只能算历史自述 |
| A8 | 模型 HTTPS/TLS、真实上游、连接中断、并发竞争（RATE_LIMITED）均未测；429 `retryable=true` 未断言；503 无专门用例 | 测试缺失 | P1 | `http_model_provider_test.cpp:240-315`（仅 200/429/500/畸形/超时/超大）；`:37-40` 只断言 code | 生产 TLS/证书链/代理/真实限流形态无覆盖（T07b 未验收，`doc/16:250`） |
| A9 | 时区依赖系统 IANA zoneinfo 但无启动预检；`LoadTimeZone` 失败被归类为 `INVALID_ARGUMENT`，与“依赖故障”不可区分；每次 resolve 都读 zoneinfo 文件 | 半实现 + 未验证 | P2 | `time_zone_resolver.cpp:20-23`（失败→`INVALID_ARGUMENT` `:8-10`）；`doc/08:5` 要求系统安装 tzdata；无缓存 | 容器缺 tzdata 时任务创建全部报参数错误，运维难定位（部署层问题） |
| A10 | 快照上限（64 MiB / 1,000,000 项）无任何测试；超限后所有提交返回 `Unavailable`，无告警/指标 | 未验证 + 半实现 | P1 | `sqlite_runtime_store.cpp:12,98-100,143`；`runtime_snapshot_codec.hpp:68-73,159-162`；`doc/05:171` 称“尚未进行容量/性能认证” | 运行时状态长期增长（事件/审计/Host 幂等/任务）达到 64 MiB 后写入全部失败；缺监控退出 |
| A11 | 崩溃/半写恢复未做进程级验证：仅依赖 SQLite FULL 同步 + 默认 journal；无 kill -9/断电注入测试、无 `PRAGMA integrity_check` | 未验证 | P1 | `sqlite_runtime_store.cpp:144`（仅设置 `synchronous=FULL`）；测试仅覆盖坏 payload/删目录（`sqlite_runtime_store_test.cpp:174-190`） | `doc/05:169` “SQLite 承担崩溃原子性”是推断，无证据 |
| A12 | migrate CLI 没有进程级测试（重复迁移、锁冲突恢复、坏 schema 退出码均只在库层测）；CI 无该二进制的测试目标 | 测试缺失 | P2 | `infrastructure/CMakeLists.txt:89-90`（只建 target）；`migrate.cpp:6-28`；`doc/05:188` 自述“进程级测试覆盖…”在仓库内找不到对应测试 | T11a 的迁移验收（`doc/16:342-346`）无当前可复现证据 |
| A13 | 旧二进制/schema 回滚只有“拒绝打开”，无兼容检查、无回滚工具、无演练；备份/恢复只在 ops 脚本（未演练，T11b 未验收） | 半实现 | P1 | `sqlite_runtime_store.cpp:177-203`（version!=5→Corrupt）；`scripts/deploy/backup.sh:37-74`（VACUUM INTO + sha256；属 ops）；`doc/16:348-361`（T11b 未验收）；`doc/08:173`（人工核对 schema） | 升级后回滚依赖人工备份；默认镜像不含 `kiarabot-migrate`（`Dockerfile:12-13` 只建 dev Core；`doc/08:162` 承认） |
| A14 | 单库单 Core 只靠文档约定，代码不强制：`open` 无 PID/文件锁/`busy_timeout`；第二个 Core 能打开同一 DB，随后长期 Conflict | 未覆盖 | P2 | `sqlite_runtime_store.cpp:134-144`（无 busy timeout/无互斥文件）；`doc/08:5` “一个数据库只运行一个 Core” | 误启动双实例时表现为持续 CONFLICT/启动失败，无明确保护 |
| A15 | 备份恢复、迁移编排不在基础设施内部：store 无在线备份 API，依赖 ops 的 `sqlite3 VACUUM INTO`；无 schema 兼容性检查进入恢复流程 | 未覆盖 | P2 | `scripts/deploy/backup.sh:37-74`；`doc/08:162-173`；`doc/05:171` 末句 | 恢复正确性取决于外部脚本与人工步骤（T11b 未验收） |
| A16 | `FileStorage` 不校验目录存在性/所有权/权限（文档要求“由 Core 进程控制的目录”）；`get` 无大小上限；孤儿临时文件永不清理；无并发测试 | 半实现 | P2 | `file_storage.cpp:68`（构造函数只保存路径，无 `stat`）；`:87-101`（无界读）；`:112-145`（临时文件，崩溃即遗留）；`doc/05:104` | 目录配置错误时延迟到首次 IO 才失败；大文件 OOM 风险；长期运行会积累 `.kiarabot-*` 残留 |
| A17 | 文件键长允许 1–256 字符，但多数文件系统 `NAME_MAX=255`；256 字符键会在 `openat` 处失败为 `INTERNAL`，且无边界测试 | 半实现 | P2 | `file_storage.cpp:36-47`；`doc/05:104`；`file_storage_test.cpp:13-25`（只用短键） | 文档承诺的 256 长度不可用；错误码误导 |
| A18 | `file_storage_test.cpp` 全部用 `assert`；Release/`NDEBUG` 构建下测试恒过；符号链接创建失败也不检查（`std::filesystem::create_symlink` 返回值未断言） | 测试缺失 | P2 | `file_storage_test.cpp:18-24,42-48` | CI Debug 下有效，但任何 Release 验收会假绿 |
| A19 | 日志 sink 无文件输出/轮转/持久化；`sink_failures()` 无任何读取方（仅测试）；组合根以 `nullptr` 构造，daemon 不写 stdout（`doc/08:209` 已说明） | 半实现 | P2 | `runtime_log_sink.cpp:18-23`；`runtime_log_sink.hpp:16-21`；`core/app/main.cpp:99`；`sink_failures` 无调用（仅 `runtime_log_test.cpp:50`） | 日志只在内存/控制 API 可见，重启即失；失败计数不可观测 |
| A20 | 日志字段与 `doc/08` 通用规范不一致：缺 `timestamp`（有 `occurred_at`）、`event_id/action_id`、`message`、`error_code`；`resource` 语义模糊 | 契约不一致 | P2 | `doc/08:84-98` vs `doc/08:209`（T10b 实际字段）vs `runtime_log_sink.cpp:39-46` | 运维按 doc/08 通用字段建告警/检索会对不上 |
| A21 | 模型配置的 API Key 以进程环境变量驻留（`KIARABOT_MODEL_API_KEY`），无清零/隔离，同用户进程可读 `/proc/<pid>/environ`；`valid_config` 仅限制 header 注入 | 半实现 | P2 | `core/app/main.cpp:188`；`http_model_provider.hpp:9-13`；`http_model_provider.cpp:29-39` | 密钥不落库、不进子进程（`main.cpp:306-312` 已隔离），但 Core 进程环境暴露 |
| A22 | 基线中 `PostgresAdapter`/`RedisAdapter`/`CronSchedulerAdapter` 未实现；调度由 Core `TaskScheduler` 承担 | 未实现（延期/未排期） | P2 | `doc/18:199-209,417`；`doc/16` 无对应任务条目；`core/include/.../task_scheduler.hpp:7` | 属基线建议而非已承诺验收；当前无任务/无验收，替换数据库或调度器需改 Core 装配（A1 放大） |

补充（逐题回答，重要项展开）：

- **SQLite schema/迁移**：只有一张表 `runtime_snapshot(id CHECK(id=1), revision, payload BLOB)`（`sqlite_runtime_store.cpp:160-163`）；schema 版本唯一来源是 `PRAGMA user_version`（`:152-157,172,195,200`），当前硬编码 5 出现 3 处（编码器 `runtime_snapshot_codec.hpp:65`、打开比较 `sqlite_runtime_store.cpp:200`、迁移输出 `migrate.cpp:16`）；无 ALTER 类迁移（只有 payload 重新编码）。迁移在 `BEGIN IMMEDIATE` + 单一 `COMMIT` 内完成（`:144,208-211`），失败 `ROLLBACK`（`:148,213-218`）。`migrate.cpp` 与库共用同一实现，30 秒内对 `Conflict` 每 100ms 重试（`:11-27`）。
- **事务边界**：提交是单条原子 `UPDATE ... WHERE id=1 AND revision=?`（`:101-112`），无显式 `BEGIN`；失败不改变持久状态。`RuntimeTransaction` 没有 `rollback()`（`runtime_store.hpp:15-21`），“销毁即回滚”只是丢弃私有内存快照（`doc/05:163,169` 表述与此一致）。`begin()` 读取整份快照（`:221-238`），提交再整份重写，64 MiB 上限下一次提交就是整库写。
- **并发/锁**：同实例用 `State::mutex`（`sqlite_runtime_store.cpp:49,92,222,230`）；跨实例靠 revision CAS；`SQLITE_OPEN_FULLMUTEX`；**无 `busy_timeout`**，锁竞争立即 `SQLITE_BUSY`→Conflict（`:13-23`）；daemon 不重试，migrate 才重试 30s。`read()` 在外部 `EXCLUSIVE` 下返回 Conflict（测试 `:161-173`）。
- **损坏/半写恢复**：读时检测 payload 类型/长度/解码异常（`:55-82`）、未知枚举/重复键/尾随数据（`runtime_snapshot_codec.hpp:121-145,159-185,276-280`）；打开时对坏库拒绝（`:204-207`）。写入路径依赖 SQLite journal（默认 delete），没有 WAL、没有 `integrity_check`、没有崩溃注入测试（A11）。
- **旧版本回滚风险**：v5 payload 对旧二进制是 safe-fail（旧版本读 `user_version=5` 不在 1–4 → Corrupt，不写库，`:177-203`）；但任何回滚都需要迁移前备份，仓库只有 ops 脚本且 T11b 未验收（A13/A15）。`doc/05:118` 明确“旧二进制回滚需备份”。
- **模型 Provider**：仅非流式（`:113` `"stream":false`；协议 `proto/kiarabot/v1/plugins.proto:167-178` 无 stream 字段）；超时 = `min(Host deadline, 30s)`（`:145,151`，Host 侧 deadline 上限 300s 见 `core/include/kiarabot/core/application/host_validation.hpp:44`；`main.cpp:72-79` 的毫秒截断可能让 <1ms 剩余预算直接 TIMEOUT）；重试 = 无（按设计，A6）；429→`RATE_LIMITED` 且 `retryable=true`（`:167,18`），503/其他→`UPSTREAM_UNAVAILABLE`（`:164-168`）；畸形响应（坏 JSON、choices≠1、content 非字符串/超限、tool_calls/function_call、usage 非整数）→`UPSTREAM_UNAVAILABLE`（`:169-195`）；1 MiB 响应上限由写回调强制（`:50-61`）；密钥隔离见 §3 #16。**只有 OpenAI 兼容形态**（A5）。
- **时区**：允许 `UTC` 或含 `/` 的 IANA 名（字符白名单、拒绝前导 `/`、长度 ≤128，`time_zone_resolver.cpp:12-19`）；`absl::LoadTimeZone` 依赖系统 zoneinfo（`:20-23`），`zone.At(civil)` 只接受 `UNIQUE`（`:31-34`），因此 DST gap/fold 都拒绝；输出秒精度（`:35`）；`LocalTaskTime` 无亚秒字段（`plugins.proto:193-201`）。无启动预检、无缓存、区缺失误分类为参数错误（A9）。
- **文件存储路径/权限/清理**：构造不校验目录（A16）；每次操作 `open(directory, O_DIRECTORY|O_CLOEXEC)` 后 `openat`（`:74,79,108,117,152,157`），末级 `O_NOFOLLOW`；临时名 `.kiarabot-<pid>-<seq>`，`O_CREAT|O_EXCL|O_NOFOLLOW` 0600，`fsync` 后 `renameat` 替换（`:112-145`）；无目录 `fsync`（文档明确不承诺目录项断电持久性，`doc/05:104`）；无清理（A16）；无容量上限。
- **日志 sink 字段与轮转**：字段 `id/occurred_at(level/component/operation/outcome/resource/request_id/trace_id)`（`runtime_log_sink.cpp:39-46`），非 ASCII/控制字符被 `_` 替换、`"`/`\` 转义（`:7-16`）；缓冲区有界 4096/10 分钟（`:28-32`），`read()` 再按 10 分钟过滤（`:54-63`）；**无轮转/持久化**，轮转按 `doc/08:100` 归运维层但仓库未见实现（A19/A20）。

## 5. 与文档/台账声明的不一致

| 声明（文档/台账） | 实际（代码） | 证据 |
| --- | --- | --- |
| `doc/05:89-98`：“基础设施必须实现 Core 定义的接口：`IUnitOfWork.begin()/commit()/rollback()`、`IInboxRepository.put_if_absent`、`IOutboxRepository.append`、`IActionRepository.get_by_idempotency_key`” | 前两个接口不存在；`IUnitOfWork` 存在但零实现；实际能力由 `RuntimeSnapshot` + `HostDispatcher`/应用服务承担，无独立 Repository/UoW 端口 | `ports.hpp:1-139`；`doc/02-core.md:11` 亦称旧 `IUnitOfWork` 是“后续扩展端口” |
| `doc/05:40-50`“第一批端口”含 `IConversationStore`/`IUserRepository`/`IPermissionStore` | 全仓无这些接口/实现；`ports.hpp` 只有 8 个接口 | `ports.hpp`；grep 结果仅文档出现（`doc/05:44-46`） |
| `doc/05:165`“原 ports.hpp 的不完整 IRuntimeRepository 移除，Host UnitOfWork 直接暴露同一 RuntimeSnapshot” | `IRuntimeRepository` 确实不在；但 `INamespaceStore`/`IIdempotencyStore`/`IAuditLogger`/`IUnitOfWork(Factory)` 仍在且无实现，文档未把它们标为“未接线” | `ports.hpp:23-69,125-129` |
| `doc/16:207` T06b“已完成并验收”；`doc/16:490`“验收：fake 正常、TIMEOUT、429、503、中断、畸形/超大响应、工具调用…” | 基础设施测试是 HEAD 新增、**从未运行**；测试只覆盖 200/429/500/超时/畸形/超大/工具调用，未见“中断”和 503 专门用例；`doc/05:68` 自述“本轮未运行” | `infrastructure/tests/http_model_provider_test.cpp:240-315`；`doc/05:68`；`doc/16:588`（CI 未绿） |
| `doc/05:68` 称测试已加入“Unix 下注册 CTest `http_model_provider`，链接 `kiarabot_model`” | 属实，但仅在 `KIARABOT_ENABLE_INFRASTRUCTURE=ON` 的根构建或独立 infra 构建中注册；dev 预设（CI build-test job）不包含它 | `infrastructure/CMakeLists.txt:49-56`；`CMakeLists.txt:120-125`；`CMakePresets.json` dev 预设 |
| `doc/05:104`“`FileStorage` 使用已存在、由 Core 进程控制的目录” | 不校验目录存在/所有权/权限；目录可为符号链接；256 字符键超出 `NAME_MAX` | `file_storage.cpp:36-47,68` |
| `doc/05:185`“可输出单行 JSON 到 stdout” | 能力存在（构造函数可传 `std::ostream*`），但组合根传 `nullptr`，daemon 不输出 JSON；`doc/08:209` 已注明“daemon 默认不写阻塞 stdout”，两份文档口径不完全一致 | `runtime_log_sink.hpp:12`；`core/app/main.cpp:99`；`doc/08:209` |
| `doc/08:84-98` 通用日志字段（timestamp/event_id/action_id/message/error_code…） | sink 实际字段缺 `timestamp`/`event_id`/`action_id`/`message`/`error_code`，`doc/08:209` 的 T10b 字段清单与自身通用清单不一致 | `doc/08:84-98` vs `doc/08:209` vs `runtime_log_sink.cpp:39-46` |
| `doc/05:38`“`deploy/` 只负责编排和调用迁移命令” | 默认 `Dockerfile` 不构建 proto/infra，也不打包 `kiarabot-migrate`；compose 的 `MIGRATION_COMMAND` 需外部提供（`doc/08:162` 已承认） | `Dockerfile:12-13`；`deploy/compose.prod.yml:17`；`doc/08:162` |
| `doc/05:171` 快照上限、FULL 同步承担崩溃原子性 | 上限/同步设置属实，但无容量与崩溃测试；台账还记录一次 SQLite 单测偶发失败未定位 | `doc/16:475`“sqlite_runtime_store 报告了无行号断言失败…不能宣称已经定位根因” |
| `doc/16:468`“独立 infrastructure 构建 CTest 3/3”、`doc/16:467`“proto + infra CTest 17/17/20/20” | 当前 HEAD 独立 infra 有 **5** 个测试目标（新增 http_model_provider），历史数字为旧状态；本机无工具链无法复核，且 CI 未绿 | `infrastructure/CMakeLists.txt:49-88`；`doc/16:588` |
| `doc/13:7,9,15,36`（历史快照）称文件存储“已有”、T06b/T06c/T10b/T11a“后续已实现并本机验收”、迁移命令“已本机验收” | 文件存储实现存在但**未接线**；模型边界测试是 HEAD 新增且从未执行；migrate CLI 无测试；真实上游/T11b 未验收（`doc/16:250,348-361`） | `file_storage_test.cpp:13`（唯一使用者）；`doc/05:68`；`infrastructure/CMakeLists.txt:89-90`；`doc/16:588` |
| `doc/14:25`（历史快照）“根 CMake 未加入 infrastructure，当前需独立构建” | 已过时：根 CMake 已有 `KIARABOT_ENABLE_INFRASTRUCTURE` 可选接入 | `CMakeLists.txt:120-125`；`doc/14` 头部未标历史快照（`doc/14:1-19` 仅说明文档定位） |
| `doc/18:202-206` 列出 Postgres/Redis/CronScheduler 适配器为可选实现 | 均未实现；`doc/18:417` 附录自认；`doc/16` 无对应任务/验收 | `doc/18:199-209,417`；`doc/16` 全部任务列表无 Postgres/Redis/Cron 条目 |

补充：

- **基线声明但缺失的实现判定**：`PostgresAdapter`、`RedisAdapter` 属“基线建议、无任务、无验收”的**未排期/延期未来选项**（P2，不是 P0/P1 违约）；`CronSchedulerAdapter` 属**设计被替代**——当前调度由 Core `TaskScheduler` 承担且 `doc/18:417`、`doc/05:116`、`doc/08:5` 三方一致，不算遗漏；`IMessageBus` 在基线中隶属基础设施层，但当前由 Core `BridgeServer`/proto 承担，无对应端口，属跨模块项，待其他模块确认。基础设施真正“遗漏”的是：`IAuditLogger`/`IIdempotencyStore`/`INamespaceStore`/`IUnitOfWork` 的适配器与 `doc/05:91-98` 的 Repository 端口（A1/A3），以及 `IFileStorage` 的消费方（A4）。

## 6. 跨模块影响（只陈述，不替其他模块下结论）

- **Core 装配/健康探针**：`ControlService::readiness()` 每次 ready 探测都 `begin()+commit()` 写一次完整快照（`core/src/application/control_service.cpp:184-198`），compose 每 10s 探测（`deploy/compose.prod.yml:39-44`）；在 64 MiB 快照下产生持续整库写放大与 revision 递增，可能使其它在途事务 Conflict。待 Core/Control/ops 模块确认。
- **Core 冲突重试语义**：Core Bridge 对 retryable CONFLICT 有 20ms 重试（`doc/16:502`），依赖存储 Conflict 语义稳定；`sqlite_runtime_store.cpp:13-23` 的映射与之一致，但无跨模块契约测试（待 Core 确认）。
- **Core TaskScheduler**：调度规则、容量（1024 任务/插件）、claim fencing 在 Core（`core/src/application/task_scheduler.cpp:11-12,126`），基础设施只提供快照编码与 `TimeZoneResolver`；“调度器职责在 Core”是文档化决策（`doc/18:417`），但违反 `doc/00` “infrastructure 实现调度”的字面分工，待架构维护者确认口径。
- **运维/部署**：默认镜像不构建 proto/infra（`Dockerfile:12-13`），因此 SQLite/模型/时区/日志四类适配器在默认部署路径中全部不可达；`scripts/deploy/migrate.sh` 依赖外部 `MIGRATION_COMMAND`；备份/恢复在 `scripts/deploy/backup.sh`（ops），T11b 未验收（`doc/16:348-361`）。待 ops/T11b 模块确认。
- **CI/工具链**：基础设施测试只在 `KIARABOT_ENABLE_INFRASTRUCTURE=ON` 的构建中注册（`CMakeLists.txt:120-125`），dev 预设 CI job 不覆盖；CI 的 clang-tidy job 用 dev 预设 compile_commands（`.github/workflows/ci.yml:173-194`），infrastructure 源码不在其中；`scripts/quality/check-tidy.sh:12,40` 覆盖 infra 但 CI 不调用（`README.md:116` 自述 CI 不直接调用 `check-toolchain.sh`）。待 CI/工具链模块确认。
- **SDK/插件**：模型密钥不进子进程环境（`core/app/main.cpp:306-312`），此隔离依赖 Core 环境白名单；SDK 侧流式/重试能力由协议决定（`proto/kiarabot/v1/plugins.proto:167-178`）。待 SDK 模块确认。
- **控制 API**：`GET /logs` 消费 `main.cpp:229` 注入的回调，仅十分钟内存窗口；日志字段对齐问题（A20）会影响面板/运维检索。待 control-api/panel 模块确认。

## 7. 覆盖结论

- 一句话结论：**SQLite 快照持久化与迁移是当前实现最扎实的部分（代码+单测+多模块集成测试），但“端口层”存在成片死接口/文档虚列，文件存储与迁移命令未接入主流程，模型/时区/日志适配器只有“代码级实现、未执行或未接生产”的证据，容量与崩溃恢复没有任何验证**——整体处于“主流程可跑、可靠性未认证、可替换性未兑现”的状态。
- 覆盖度估计：**完整 25 / 60 个功能点；部分 10 / 60；未实现·未接线 16 / 60；未验证 9 / 60**（口径：按本报告 §3/§4 的显式功能点枚举，覆盖 9 个端口（A 类）+日志端口 1 + SQLite 16 + 模型 13 + 时区 5 + 文件 6 + 日志 4 + 构建/测试/部署 6 = 60；状态定义：完整=代码存在且有至少一处可复核测试/接线证据；部分=只覆盖子集或有硬编码限制；未实现·未接线=无代码或无调用链；未验证=实现存在但测试从未运行或关键行为无测试）。

## 8. 未核实事项

1. **全部构建与测试结果不可复核**：本机无 cmake/ninja/node/uv/protoc/docker，且 `doc/16` 明确“本机未运行/CI 未绿”。所有“历史通过”均引自台账（`doc/16:465-475,502,588,600`），不代表当前 HEAD 可编译。
2. **可能的编译问题（静态推断，未验证）**：`time_zone_resolver.cpp:14` 使用 `std::all_of` 但未直接 `#include <algorithm>`；`migrate.cpp:7` 使用 `std::string_view`/`std::chrono` 未直接包含头文件；`http_model_provider.cpp:67,145` 使用 `std::optional`/`std::int64_t` 依赖传递包含。若目标工具链/标准库不传递包含，可能编译失败。
3. **快照二进制编码边界**：`std::quoted` 对任意字节（NUL、换行、引号、反斜杠）的往返依赖实现；测试只验证 `a\0b`（`sqlite_runtime_store_test.cpp:106,124`），未覆盖全字节域、超长字符串、时间越界（2262 年后纳秒溢出）等。
4. **SQLite 崩溃原子性与删除场景**：A11 所述崩溃/半写未验证；`sqlite_runtime_store_test.cpp:181-190` 同时删除了数据库文件**和所在目录**，才使提交失败；若只删除文件而目录可写，SQLite 可能写进已 unlink 的 inode 并“成功提交”，此行为未测试，无法断言；`PRAGMA synchronous=FULL` 只在 open 设置一次，是否覆盖后续所有连接行为未实测。
5. **`absl::LoadTimeZone` 的数据来源**：取决于 Abseil 构建选项（系统 zoneinfo 或编译内嵌 tzdata），本机/CI 的 absl 包来源未核实；`doc/11:3` 只声明使用系统 zoneinfo。
6. **libcurl 配置**：`CURLOPT_PROXY ""` 是否在所有 7.85+ 版本完全屏蔽环境代理、TLS 证书路径（系统 CA vs `CURL_CA_BUNDLE`）在容器内的行为，均未验证。
7. **CI 实际状态**：CI 自 2026-10-30 仅手动触发且“5 次运行均未通过”（`doc/16:588,600`），无本地日志；基础设施 5 个 CTest 是否曾在 contracts job 中实际跑绿无法核实（待 CI 模块）。
8. **`doc/08:214` “非法路径退出 2”**：`migrate.cpp:7-10` 只对空串/`:memory:`/`file:` 返回 2；不存在的目录/文件路径会以 `Unavailable` 返回 1（`sqlite_runtime_store.cpp:137-141`），文档措辞是否涵盖此情形未核实。
9. **旧二进制读 v5 的精确行为**：当前 HEAD 的 `open()` 会以 `version!=5 → Corrupt` 拒绝（`sqlite_runtime_store.cpp:200-203`），但已运行的旧进程若共享同一文件的行为取决于旧版实现，无法在本仓库验证。
10. **模型真实上游**：TLS、真实供应商限流形态、超时计费、中断连接等均属 T07b（`doc/16:250`）未验收项，本报告只能静态判断代码映射。
11. **`FileStorage` 在不同文件系统（APFS/ext4/网络盘）的 `O_NOFOLLOW`、`renameat`、`NAME_MAX` 行为**未验证；测试仅在 macOS 临时目录历史运行过（`doc/16:468` 的 3/3 为旧状态）。
12. **未核对其他模块报告**：本报告不评价 Core/Control/ops/CI 模块的结论；跨模块影响均标“待确认”。不修改 `doc/18`（受保护）或其他任何文件。
