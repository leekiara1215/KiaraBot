# 基础设施适配器设计

T06a schema v4 在 RuntimeSnapshot 中增加 namespaces（身份→键→值/过期时间）和 host_receipts（身份→幂等键→fingerprint/响应/保留期限）。旧 v1/v2/v3 在同一事务迁移，新增集合初始为空；原有事件、Action、插件、审计保留。运行时和桥接的其他状态更新必须保留这两组集合。Host 存储通过现有 RuntimeTransaction 与审计、幂等记录共同提交，不另建独立提交的 namespace 数据库。

T04b RuntimeSnapshot 的 ActionRecord 增加 owner、serialized_envelope、fingerprint，ActionEntry 增加 dispatched。SQLite schema v3 在同一事务从 v1/v2 解码旧字段后重编码；旧记录缺少所有者，不能通过新的插件查询获得。迁移失败整体回滚，新版本拒绝未知 schema。此处只保存 Core 定义的数据，不执行权限或平台规则。

## 责任

基础设施适配器把 Core 定义的端口接口连接到具体数据库、模型、缓存、文件系统和调度器。

它们不实现消息路由、插件权限或控制面板逻辑。

## 目录

```text
infrastructure/
  sqlite/
    migrations/
    include/
    src/
    tests/
  model/
    http/
    providers/
    tests/
  scheduler/
    src/
    tests/
  audit/
    src/
    tests/
  migrate/
    src/
    tests/
  fake/
```

上图为目标目录；当前实现位于 `infrastructure/src/` 和 `infrastructure/include/`，SQLite 迁移在 `sqlite_runtime_store.cpp` 中实现，`migrate.cpp` 提供命令入口。`deploy/` 只负责编排和调用迁移命令。

## 第一批端口

```text
IModelProvider
IConversationStore
IUserRepository
IPermissionStore
IScheduler
IFileStorage
IAuditLogger
```

## Core 端口

Core 内部接口位于 `core/include/kiarabot/core/ports/ports.hpp`，协议类型和稳定错误使用生成的 `kiarabot.v1` 类型。所有调用必须区分协议 `Error` 与成功值；数据库故障不能伪装为重复或未找到。事务、任务领取、时区转换、文件和模型实现必须遵守端口及 `doc/01-contracts.md`、`doc/01-runtime-semantics.md` 的语义。接口的存在不表示适配器或插件 Host 服务已经接入。

## 模型适配器

### T06b 非流式 HTTP Provider

实现 IModelProvider，使用 libcurl（最低 7.85）向配置的完整 Chat Completions URL 发送 model/messages/stream=false，读取 choices[0].message.content 和可选 usage.prompt_tokens/completion_tokens。字段参考 [Chat Completions 官方契约](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create)。仅支持纯文本，工具调用、流式和非文本结果拒绝为 UPSTREAM_UNAVAILABLE。

端点仅接受 HTTPS 或 127.0.0.1 的 HTTP 测试服务；禁止 URL 内嵌凭据、查询、fragment、重定向和环境代理，TLS 证书验证保持开启。API Key 只进入基础设施 Authorization 请求头；错误、审计和插件响应不包含上游正文、URL 或密钥。配置单个上游模型，插件使用 default 别名或该模型名；其他模型 CAPABILITY_NOT_SUPPORTED。请求最多 64 KiB、128 条消息，响应最多 1 MiB，文本最多 64 KiB；超大请求 INVALID_ARGUMENT，上游畸形/超大响应 UPSTREAM_UNAVAILABLE。

每个 Provider 最多一个在途请求，竞争返回 RATE_LIMITED。调用最长 30 秒且不超过 Host deadline；HTTP 429 映射 RATE_LIMITED（retryable=true），超时 TIMEOUT，其余 HTTP/网络/解析失败 UPSTREAM_UNAVAILABLE。超时及不确定的上游错误 retryable=false，不自动重试。成功但无 usage 时 token 数为 0，不代表计费为零。

Core 使用可信插件身份检查 model.chat，持久化 STARTED 审计成功后才调用网络；调用不持有数据库事务或 Host 存储锁。结果另写一条仅含稳定码的审计。启动审计失败时不发请求；结果审计失败时返回存储错误，此时外部调用可能已经完成。模型不提供幂等回放，断线/崩溃后不能保证未计费，插件不得自动重发。中断连接不会取消已开始的同步 HTTP 调用，但会在上述期限内结束。

实现位于 `infrastructure/src/http_model_provider.cpp`。本轮新增 `infrastructure/tests/http_model_provider_test.cpp`，已加入独立基础设施构建的测试目标（Unix 下注册 CTest `http_model_provider`，链接 `kiarabot_model`）：覆盖 Provider 配置校验边界（非法 scheme/非回环 HTTP、URL userinfo/query/fragment、API key 非法字符）以及本机回环 HTTP 假服务的响应映射，不连接外部模型。本机缺少构建工具链，该测试本轮未运行，具体结果以实际运行记录为准；真实上游模型仍未联调（T07b 真实环境未提供）。

模型适配器负责：

- Provider 和模型选择；
- API Key 管理；
- 请求超时和重试；
- 流式响应转换；
- 用量和错误记录；
- OpenAI-compatible 或其他厂商协议转换。

插件只能使用 `ModelService`，不能获得 HTTP 客户端和 API Key。

## 存储适配器

第一版建议使用 SQLite。Core 定义迁移版本和事务边界，基础设施执行迁移和连接管理；插件只能使用命名空间存储：

```text
plugin:<plugin_id>:<key>
```

插件不得创建或修改 Core 表，不得保存未经过 Schema 校验的任意大对象。

基础设施必须实现 Core 定义的接口：

```text
IUnitOfWork.begin() / commit() / rollback()
IInboxRepository.put_if_absent(event_id)
IOutboxRepository.append(action)
IActionRepository.get_by_idempotency_key(key)
```

独立的 `kiarabot-migrate` 命令使用 SQLite `BEGIN IMMEDIATE` 互斥并在事务内迁移快照，成功后写入 `PRAGMA user_version`；锁等待超过 30 秒失败。当前单快照适配器使用 FULL 同步与 SQLite 默认 journal；没有关系表外键。运行时提交遇到冲突立即报告 CONFLICT。

## 文件存储

`FileStorage` 使用已存在、由 Core 进程控制的目录；逻辑键仅允许 1–255 字节的 ASCII 字母、数字、`-`、`_`（POSIX 单级文件名 `NAME_MAX=255`；256 字节键在接触文件系统前就被拒绝为 INVALID_ARGUMENT），不接受路径。读取不跟随末级符号链接；写入使用同目录临时文件和原子替换，删除不存在的键成功。文件权限为 0600；调用方不得直接向插件暴露目录或文件描述符。该实现针对 POSIX 平台，不承诺断电后的目录项持久性。`FakeFileStorage` 支持下一次操作故障注入。

独立构建与测试（需要 Protobuf 的 CMake 包和 protoc）：

```bash
cmake -S infrastructure -B /tmp/kiarabot-infrastructure-build
cmake --build /tmp/kiarabot-infrastructure-build
ctest --test-dir /tmp/kiarabot-infrastructure-build --output-on-failure
```

## 调度器

T06c 的规则由 Core TaskScheduler 实现，基础设施提供 SQLite schema v5 的任务快照和 Abseil 时区解析端口。沿用 Protobuf/gRPC 已依赖的 Abseil time，使用系统 IANA zoneinfo；不通过进程全局 TZ 环境切换时区。允许 UTC 或 IANA 地区名称，公历年份 1970–2100，拒绝非法日期、无效时区及 DST 重复/缺失时间；UTC run_at 也限制在此范围且必须晚于当前时间。启动预检与错误区分（2026-10-31 补齐）：`TimeZoneResolver::zone_info_available()` 用真实 IANA 地区名探测系统 zoneinfo（`UTC` 可能内建于 Abseil，不能作为探测依据）；`resolve()` 在 zone 数据不可用时返回 `UPSTREAM_UNAVAILABLE`，只有名称/日期/夏令时歧义等调用方错误返回 `INVALID_ARGUMENT`。Core 组合根应在监听前调用预检并拒绝启动；当前 `core/app/main.cpp` 尚未接入该预检（交接 core 分支，不属于本模块可改范围）。测试见 `infrastructure/tests/time_zone_resolver_test.cpp`（有效 UTC/IANA、非法日期/名称、DST gap/fold、注入数据不可用路径）。

每插件最多 1024 个任务（含终态），payload 最多 64 KiB，task_id 1–128 个 ASCII 字母/数字/点/横线/下划线，目标会话 1–256 字节。任务记录不自动清理，超限 RATE_LIMITED。Host 创建/取消与成功幂等响应、审计同一事务；读/取消其他插件任务 NOT_FOUND。新旧 Runtime 操作必须保留任务集合，v1–v4 数据库迁移为 v5；旧二进制回滚需备份。

调度器负责保存任务、触发任务、处理时区和重试。任务必须记录：

```text
task_id
owner_plugin_id
target_conversation
schedule
payload
status
next_run_at
```

## 审计与日志

所有管理动作、权限拒绝、插件启停、模型调用失败和高风险 Action 都必须写入结构化审计日志。

日志不应包含：

- API Key；
- OneBot Token；
- 完整的私聊内容；
- 未脱敏的用户隐私。

## 替换规则

实现 PostgreSQL、Redis 或新的模型供应商时，只新增适配器，不修改 Core 的端口和领域规则。若端口不足，应先修改 `core/ports/`；若需要改变跨进程语义，则先修改 `proto/`，再修改实现。

## 验收标准

- 可以使用 fake adapter 测试 Core；
- 模型超时、限流和错误都有稳定错误码；
- 存储事务不会泄漏到插件；
- 插件停用后任务可以暂停或取消；
- 适配器替换不会修改业务代码。

## 代码启动句

> 请只实现第一版基础设施端口和 SQLite、模型 HTTP、调度器及审计日志适配器，使用 fake Core 和 fake Plugin 完成超时、事务、重试、权限和数据隔离测试，不接入 NoneBot2 或控制面板。

## T05a 统一事务快照端口

当前公共 Runtime 通过 RuntimeStore::read/begin 和 RuntimeTransaction::snapshot/commit 访问基础设施。所有公共查询和写操作返回 StorageResult<T>（共享 Result<Value, Error> 即 std::variant）；成功的空 optional 表示不存在，StorageError 的 Unavailable/Conflict/Corrupt 明确表示依赖故障、并发冲突或数据损坏。边界服务将其分别映射到 UPSTREAM_UNAVAILABLE、CONFLICT、INTERNAL，不允许以空记录或 false 吞掉存储错误。Host PortResult 使用同一个 Result 约定及已有 Protobuf Error。

事务提供独立快照，销毁未提交事务丢弃修改；提交失败不改变持久状态。完整快照包括事件、逐插件投递、会话水位、Action、插件和投递序号。Runtime 单次调用以一个快照提交，不自动重试冲突；批量调用现在整体提交，不再部分持久化。调用方须检查外层 StorageError 后才能使用内层业务结果。公共 Runtime 串行访问；存储实现须保护不同事务之间的并发冲突。

原内存状态机移至 detail::RuntimeEngine，仅操作事务私有内存。其 bool/optional 不是数据库端口，故障由外层显式返回。原 ports.hpp 的不完整 IRuntimeRepository 移除，Host UnitOfWork 直接暴露同一 RuntimeSnapshot；Host 存储/幂等/审计组合事务已由 T06a 使用 RuntimeTransaction 实现。完整快照是首版正确性实现，会复制全部状态，规模较大时应以同样事务语义优化，不能作为吞吐性能保证。

## T05b SQLite 运行时存储

SqliteRuntimeStore::open(path) 在事务中创建 schema v5（PRAGMA user_version=5），并事务化迁移 v1/v2/v3/v4 快照，已有版本重复打开不重建；未知版本或损坏快照拒绝打开。runtime_snapshot 单行保存完整快照与 revision，提交使用带 revision 条件的原子 UPDATE。两个连接读取同一版本时只有第一个提交成功，其余返回 Conflict；SQLITE_BUSY/LOCKED 也映射 Conflict，不自动重试。FULL 同步模式下 SQLite 承担崩溃原子性；未提交事务只持有私有内存，销毁即可回滚。崩溃/半写恢复证据（2026-10-31 补齐）：`infrastructure/tests/crash_recovery_test.cpp` 让子进程持续提交 12 MiB 快照并在不同时点收到 SIGKILL，父进程重开同一数据库后校验快照可解码、`PRAGMA integrity_check` 为 `ok`、且可以继续提交；该测试不依赖真实断电，但也不能替代生产备份演练（T11b）。

私有文本编码保留二进制 envelope、UTC 纳秒时间、逐插件状态、会话、Action 和插件；数据库格式与 schema 一起版本化，不是公共线协议。快照上限 64 MiB，所有集合合计最多一百万项；超限提交失败，损坏解码返回 Corrupt。超限语义（2026-10-31 明确）：提交时编码后字节数超过 64 MiB、或所有集合合计条目超过一百万项，返回 `StorageError::Unavailable` 且持久状态不变（事务可以在缩小快照后重试）；读取/打开时发现已存载荷超过 64 MiB 返回 `StorageError::Corrupt`。`SqliteRuntimeStore::limits()` 返回编译期上限，`rejected_snapshots()` 返回逐实例单调拒绝计数（oversized_bytes / oversized_items / oversized_reads）；组合根尚未把计数导出到健康或指标端点（交接 Core/运维）。测试见 `infrastructure/tests/snapshot_limits_test.cpp`（字节上限、百万项上限、超大存量载荷、失败后重试与计数）。该实现是整库状态快照，尚未进行容量/性能认证。Host 命名空间和响应幂等已在 v4 中持久化；模型由 T06b 接线，任务由 T06c 接线；T10a 的命令审计从 v2 起持久化。SQLite 打开迁移并不代替 T11 独立迁移命令和部署演练。

独立构建使用原基础设施命令；根构建可传入 `-DKIARABOT_ENABLE_INFRASTRUCTURE=ON` 纳入测试。新增依赖为 SQLite3 开发库，版本由构建环境记录；生产打包需随 T11 固定依赖。Core 仍经端口注入，不直接访问 SQL。

## T10a 受保护的应用命令

AuthorizedCommands 是 Core 内部入口：调用方必须从已认证连接构造 CommandContext，并由服务端计算规范化请求的 SHA-256 fingerprint（64 位小写十六进制），不能相信插件自报身份、权限或 fingerprint。静态 PermissionGrant 按 subject/permission/resource 精确匹配，只有配置中的资源 `*` 表示全部资源；没有匹配规则时默认拒绝。此入口不是跨进程 SDK。

execute 的回调只能修改传入的 RuntimeSnapshot，不得执行网络/平台副作用或另启 Runtime 事务。业务修改、成功幂等记录和审计在同一提交中完成；提交失败全部不生效。幂等范围为主体+权限+资源+键，同键同 fingerprint 重放成功且不重复审计；不同 fingerprint 返回 CONFLICT。重放前重新鉴权。拒绝及回调失败记录审计，失败回调的状态变更被回滚；错误详情只保留稳定码和请求标识，不转发回调的自由文本。审计满（默认十万条）时返回 UPSTREAM_UNAVAILABLE 且不执行回调。审计/幂等保留和清理策略须后续管理任务落实，不能当作无限容量服务。

SQLite 从 v2 起保存审计和命令幂等记录，当前 v5 还保存 Action 路由和 Host 命名空间/幂等响应；模型已接线，任务由 T06c 接线。T02a 适配器桥和 T03a 插件桥已使用此入口；控制 API 已由 T08b 通过独立应用端口接入同一事务存储。

## T10b 日志 sink

BufferedRuntimeLogSink 实现 Core 的 IRuntimeLogSink，互斥保护有界环形缓冲，保留十分钟、最多 4096 条；可输出单行 JSON 到 stdout。字段仅来自 Core 的结构化记录，不接收正文或任意异常消息。输出失败返回 false，内存缓冲仍可查询；Core 统计失败次数，业务提交不回滚。日志不是审计持久层，重启清空日志；持久审计不受影响。控制 API 通过组合根注入的只读回调取得日志，不直接读取基础设施类或数据库。字段与观测口径（2026-10-31 明确）：JSON 行包含 `id、timestamp、occurred_at、level、component、operation、outcome、resource、resource_id、request_id、trace_id`；`timestamp` 与 `occurred_at` 同为 UTC ISO-8601（对齐 `doc/08` 的 timestamp），`resource_id` 是 `resource` 的别名。不输出独立的 `event_id`/`action_id`：Core `RuntimeLog` 未提供这两个字段，事件/动作标识承载在 `resource`（`event.persist` 为 event_id，`action.state` 为 action_id）。不输出 `message`/`error_code`：sink 不接收自由文本，`outcome` 即稳定码（OK/TIMEOUT/CONFLICT/DEAD_LETTER 等）。sink 自身新增 `write_failures()` 单调失败计数（与 Core 装饰器的 `sink_failures()` 并存）；组合根尚未把两个计数导出到健康或指标端点（交接 core/运维）。测试 `infrastructure/tests/runtime_log_test.cpp` 增加 timestamp/resource_id 字段与写失败计数断言。


T11a `kiarabot-migrate` 复用 SqliteRuntimeStore::open 的同事务迁移，SQLite BEGIN IMMEDIATE 为迁移锁；不重复维护 schema 或手写第二套迁移 SQL。进程级测试 `infrastructure/tests/migrate_cli_test.cpp`（CTest `migrate_cli`，2026-10-31 新增）直接调用构建产物覆盖：缺参数/`:memory:`/`file:` 退出 2、首次迁移与重复迁移退出 0、v1 旧库迁移、未知 schema 与损坏载荷退出 1 且不改写数据库、持有写锁 1.5 秒后释放时 CLI 重试成功、两个实例并发启动都成功、父目录缺失退出 1。`doc/08` 的“非法路径退出 2”仅指参数形态非法；路径不存在属于依赖故障，退出 1。
