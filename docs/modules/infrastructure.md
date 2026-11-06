# 基础设施模块设计

本文描述 `infrastructure/` 作为最外层适配器，如何实现 `core/include/kiarabot/core/ports/target/` 定义的端口：四个服务的 PostgreSQL 私有库与 typed 工作单元、空库初始化 CLI、Host 本地 SQLite 日志、模型 HTTP/SSE Provider、身份与秘密密码学、文件存储、日志 sink 与时区解析。端口契约以 `core/include/kiarabot/core/ports/target/` 为唯一来源；本文在整体架构中的位置见[架构总览](../architecture.md)。

## 1 模块定位与职责

基础设施只实现 Core 定义的端口，不决定业务规则、不接收插件直接调用、不访问控制面板页面。主要职责：

- `infrastructure/postgres/`：Identity、Runtime、Scheduler、Model 四个 owner 私有逻辑库的 schema、typed Repository/UoW、显式空库初始化与迁移 CLI；
- `infrastructure/identity_crypto/`：密码、token、收据、委托、模型密钥、证书校验与密钥文件装载等密码学适配；
- `infrastructure/include/` + `infrastructure/src/`：Host 本地 SQLite 日志与维护存储、SQLite 运行时快照（reference）、目标模型 HTTP/SSE Provider、文件存储、运行时日志 sink、时区解析、消息投影等；
- 测试替身与故障夹具：`FakeFileStorage`、disposable PG16.4 夹具 `infrastructure/postgres/tests/run_database_test.py`。

目录内同时存在目标实现与 reference 实现（如 `SqliteRuntimeStore`、`HttpModelProvider`），不能把共享目录整体当作历史代码；目标服务组合根位于 `core/app/`（如 `core/app/runtime_composition.cpp`、`core/app/model_composition.cpp`）。

## 2 边界与依赖

- 依赖方向：`core/include/kiarabot/core/ports/target/` ← infrastructure；内层不直接依赖 SQL、OpenSSL、libcurl 或 HTTP 细节。
- 外部依赖（版本由锁文件与 CMake 固定）：PostgreSQL 16.4 + libpqxx 7.10.0（`infrastructure/postgres/CMakeLists.txt`）、libsodium（`unofficial-sodium`）、jwt-cpp 0.7.0、OpenSSL 3.5、nlohmann_json 3.11.3、Abseil time、SQLite3、libcurl（CMake 最低 7.85，锁文件为 8.13.0）、Protobuf/gRPC 生成类型，见 `dependencies/architecture/dependencies.lock.json`。
- 四库隔离：每个服务只读写自己的逻辑库，不跨库 SQL；跨服务资源只用类型化引用，不建跨库外键。
- Host/Adapter 的本地 SQLite 只保存其执行/可靠发送责任；SQLite 运行时快照是 reference 单机存储。
- 不实现消息路由、插件权限、控制面板逻辑；端口不足时应先改 Core 端口，跨进程语义变化应先改 `proto/`。
- 模型与密码学不读取请求中的密钥/证书路径；可信根、根密钥和私钥信息均由部署配置注入。

## 3 进程/组件结构

| 位置 | 组件 |
| --- | --- |
| `infrastructure/postgres/` | `src/database.cpp`（连接/初始化/会话）、`src/factories.cpp`（四库 typed factory）、`infrastructure/postgres/schema/`（DDL 与 manifest）、`infrastructure/postgres/src/`（各领域仓储）、`infrastructure/postgres/migrate/initialize_main.cpp`（`kiarabot-pg-initialize`） |
| `infrastructure/identity_crypto/` | password/token/receipt/delegation/peer certificate/key files/verification keys/model secret/SHA-256 适配器 |
| `infrastructure/include/` | host journal、host maintenance、SQLite runtime store、模型 Provider、文件存储、日志 sink、时区解析、快照上限 |
| `infrastructure/src/` | 上述实现的参考/目标源码；`migrate.cpp` 为 reference `kiarabot-migrate` |
| `infrastructure/tests/` | 无外部依赖的单元测试（文件/快照/日志/时区/Provider/迁移 CLI 等） |
| `infrastructure/postgres/tests/` | 真实 PG16.4/TLS 夹具驱动、typed work 检查与种子程序 |

目标 PG 库由 `kiarabot-identity`/`kiarabot-runtime`/`kiarabot-scheduler`/`kiarabot-model` 组合根链接 `kiarabot_postgres` 并在启动时打开；每个库一个显式 writer session，不允许第二连接。

## 4 数据模型与持久化

空库 schema 版本、relation 数量与 descriptor（`infrastructure/postgres/schema/v1.hpp`、`infrastructure/postgres/schema/manifest.hpp`；descriptor 是固定 PG16.4 catalog 渲染的 md5，仅作校验、不是安全原语）：

| 服务 | version | relation_count | descriptor |
| --- | --- | --- | --- |
| Identity | 18 | 141 | `1ff26df36e31a5468948032954dbbe8a` |
| Runtime | 25 | 173 | `ea6b63294371f79c4f2060cb26b35df3` |
| Scheduler | 14 | 97 | `82deb703aa774cd987361be76154e443` |
| Model | 11 | 85 | `e556cdf66c22626e4a5b26f621fde42e` |

共享关系（`infrastructure/postgres/schema/common.hpp`、`infrastructure/postgres/schema/services.hpp`）：`schema_versions`、`replay_results`/`result_*` 快照、`command_receipts`、`audit_entries`、`operations`/`operation_participants`、`outbox`/`inbox`、`grant_scopes`、观察日志。owner 私有关系按服务划分：Identity 的凭据/会话/精确授权/失效流；Runtime 的账号/安装/实例/适配器绑定、事件与投递、Action 与配额策略、namespace、Host 连接/命令/观察、bootstrap 交付、生命周期墓碑；Scheduler 的 tasks/occurrences/quota；Model 的 model_configs/encrypted_provider_secrets/model_intents。文件级清单见 `infrastructure/postgres/schema/`。

时间与数值：时间列为 signed 64 位 UTC epoch 纳秒；`uint64` 用 `kiarabot.uint64`（NUMERIC(20,0)）无损保存，禁止经 double/signed bigint 截断；到期/租约/容量以数据库 `clock_timestamp()` 在条件 SQL 内裁决。

版本策略：只支持显式空库初始化，不自动迁移、不自动创建或清空已有库；旧 schema 拒绝打开。每次 schema 变化必须同次更新 `v1.hpp`/`manifest.hpp` 并用真实 PG16.4 重新实测 descriptor/relation_count。Host SQLite 本地 journal 与 maintenance 使用独立 schema 与版本；SQLite 运行时快照为私有格式（schema v6，上限 64 MiB/100 万项）。

## 5 关键流程/状态机

空库初始化：`kiarabot-pg-initialize --initialize-empty SERVICE HOST PORT DATABASE MIGRATION_ROLE RUNTIME_ROLE CA_FILE PASSWORD_FILE`。只接受无用户对象的数据库；DDL 与版本标记在同一事务提交；重复初始化、未知/半初始化状态拒绝；密码文件必须为常规非符号链接 0600 文件（≤4096 字节、无 CR/NUL）。退出码：成功 0；参数/秘密文件/端口非法 2；初始化被拒或提交未知 1 且输出 `INITIALIZATION_REJECTED`/`INITIALIZATION_UNKNOWN`。

打开与事务：`Database::open(DatabaseConfig)` 校验服务与私有库/schema/角色后持有唯一 writer session 与库内 session advisory lock；`begin()` 在 250ms 有界等待内返回单 owner UoW，忙则 Conflict；连接/锁丢失或 Unknown commit 使该实例永久禁用，必须新进程重新验证，不自动重连。UoW 总预算 2 秒、锁等待 250ms，仓储借用同一物理事务、不自行 commit；失败使整个 UoW 不可提交部分结果。

提交语义：收据、业务状态、审计、outbox/交接在同一事务原子提交；事务内不调用远端、不跑密码计算/用户回调；同内容重放返回原快照与保留期，不同内容 Conflict。Runtime 事件投递、Action 派发与 namespace 写都在调用方 Runtime UoW 内完成（`infrastructure/postgres/src/runtime_events.cpp`、`action_records.cpp`、`storage_records.cpp`）。

维护与清理：`host_maintenance_sqlite`/operation journal 按固定锁序与有界批次推进 low-water；P13a 逻辑删除后非秘密内容 30 天可恢复、墓碑/必要审计 180 天、ID 不复用，清理每 60 秒有界执行，未决 receipt/inbox/outbox 不提前清理。

模型 Provider：`infrastructure/include/kiarabot/infrastructure/model_http_provider.hpp` 实现 `ModelProviderPort`，以 libcurl 调 OpenAI-compatible Chat Completions；streaming 由 `model_http_stream_provider.cpp` + `model_sse_parser.cpp` 处理 SSE，`cmake/model-streaming.cmake` 只链接固定锁的 C parser（MIT），禁止上游 client/transport。Provider 失败不自动重试；dispatch 后未知结果保守 `UNKNOWN`/`may_have_billed`。

密码学与密钥装载：密码 Argon2id13（v1 固定 opslimit=2、memlimit=64MiB）；token 为用途隔离 HMAC-SHA256；收据/模型密钥为 XChaCha20-Poly1305（AAD 绑定 scope/版本/根密钥版本）；委托为 Ed25519 JWT（标准 claims + 封闭 delegation 对象）；证书链按部署根 CA 严格验证。`key_files.hpp` 以 `O_NOFOLLOW|O_CLOEXEC` 打开、fstat 校验 regular/owner/权限后读取，密钥材料只在安全内存。

## 6 接口与契约

- 端口：`core/include/kiarabot/core/ports/target/` 下 `unit_of_work.hpp`、`host_registration_ports.hpp`、`host_command_ports.hpp`、`host_maintenance.hpp`、`runtime_secrets.hpp`、`model_ports.hpp`、`model_stream.hpp`、`namespace_ports.hpp`、`observation_feed_ports.hpp`、`operation_observation_ports.hpp`、`runtime_bootstrap_deliveries.hpp`、`runtime_authorization.hpp`、`identity_crypto.hpp` 等；基础设施只提供实现，不新增业务端口。
- `DatabaseConfig{service, host, port, database, user, password, root_certificate, migration_role, runtime_role}`：不接受任意 DSN，调用方不能覆盖 TLS/认证/超时策略（`infrastructure/postgres/include/kiarabot/infrastructure/postgres/database.hpp`）。
- 组合 facade：`open_identity`、`open_runtime`、`open_runtime_with_host_allowlist`、`open_scheduler`、`open_model`、`open_identity_security`（`infrastructure/postgres/include/kiarabot/infrastructure/postgres/factories.hpp`、`security_factory.hpp`）；服务不匹配返回 `InvalidArgument`，并发打开/事务串行化。
- CLI：`kiarabot-pg-initialize --initialize-empty ...`（`infrastructure/postgres/migrate/initialize_main.cpp`）；reference SQLite 迁移命令为 `kiarabot-migrate`（`infrastructure/src/migrate.cpp`）。
- 本地存储接口：`SqliteRuntimeStore::open/read/begin/limits/rejected_snapshots`（`infrastructure/include/kiarabot/infrastructure/sqlite_runtime_store.hpp`）；Host 日志用 `make_sqlite_host_journal`（`infrastructure/include/kiarabot/infrastructure/host/host_journal.hpp`），Host 维护用 `make_sqlite_host_maintenance`（`infrastructure/include/kiarabot/infrastructure/host/host_maintenance_sqlite.hpp`）。
- 错误映射契约：SQLSTATE 53/58 及非取消类 57 → `UpstreamUnavailable`；查询取消/锁等待 → `Timeout`；关系结果损坏/无法解码 → `Internal`；unique/FK 冲突 → `Conflict`；容量 → `RateLimited`。仓储不把故障伪造成空结果或成功。

## 7 权限与安全

- 数据库连接强制 `sslmode=verify-full channel_binding=require gssencmode=disable connect_timeout=5`，并设置 `statement_timeout=3000`、`lock_timeout=1000`、`idle_in_transaction_session_timeout=5000`、`search_path=pg_catalog`、`timezone=UTC`、`application_name=kiarabot`（`infrastructure/postgres/src/database.cpp`）。
- 迁移身份与运行身份分离：初始化使用 migration role；运行角色只按 schema 授予 USAGE + SELECT/INSERT/UPDATE/DELETE 与序列使用，不能建表/建 schema/临时表或访问其他业务库。
- 模型根密钥由部署以 0600 文件与单调 `key_version` 注入，不进数据库、不与密文同表、不写日志；AAD 绑定 `model_id|secret_version|root_key_version|aead`，解密失败 fail closed。
- 委托 sealed 文件（task/model 两类）要求 0600 常规非符号链接、同主机共享卷、原子写入；读取后仍必须在线 Exchange/CheckAuthorization，文件本身不是信任根。公钥文件也要求 owner 正确且 group/other 无写权限。
- PG TEXT 绑定前拒绝嵌入 NUL；诊断不回显 DSN、凭据或 pqxx 异常正文；数据库 notice 不作为应用诊断输出。
- 模型 Provider 只允许 HTTPS 或 127.0.0.1 HTTP，禁止 URL userinfo/query/fragment/重定向/环境代理，TLS 证书与主机名验证保持开启；API Key 只进内存 Authorization 头。
- 不提供弱随机降级；密码/token/证明等秘密只驻留安全内存，失败与错误不回显秘密或原始异常文本。

## 8 错误/失败/恢复语义

- 启动 fail-closed：schema 版本/descriptor/对象描述不符、writer advisory lock 被占、数据库不可达时拒绝开始业务；daemon 不自动 DDL、不自动重连、不降级为内存存储。
- UoW 失败毒化：任何仓储失败后整笔事务不可提交；提交失败不改变持久状态；Unknown commit 必须用原身份重读状态/收据恢复，不能假定未提交。
- 幂等与重放：同键同 fingerprint 重放成功且不重复审计；不同 fingerprint 返回 `CONFLICT`；收据保留期与结果快照独立于业务行，不被清理提前删除。
- 模型错误映射（非流式与流式一致）：400/422→`INVALID_ARGUMENT`；401/403→`UPSTREAM_UNAVAILABLE`；404/405/410→`CAPABILITY_NOT_SUPPORTED`；429→`RATE_LIMITED`（Retry-After 有界透传）；5xx/超时/TLS/连接/畸形/超大响应→`UNKNOWN`（`may_have_billed=true`），不自动重试。
- SQLite 运行时快照：revision 条件更新冲突返回 Conflict，BUSY/LOCKED 映射 Conflict 且不自动重试；提交超过 64 MiB 或 100 万项返回 `Unavailable` 且持久状态不变，已存超大载荷读取返回 `Corrupt`；SIGKILL 恢复测试验证可重开并继续提交。
- reference 迁移 CLI：缺参数/`:memory:`/`file:` 退出 2；首次/重复迁移成功退出 0；未知 schema 或损坏载荷退出 1 且不改写数据库；父目录缺失属于依赖故障退出 1。

## 9 可观测性与运维

- 服务 readiness 依赖数据库 schema：`core/app/postgres_service.hpp` 的 `ObservedApplication` 把 `Dependency::DatabaseSchema` 置为可用/不可用并参与 ready 判定；健康采样复用 factory 的 `begin/current_time/rollback`，不建第二持锁连接。
- 运行时日志 `BufferedRuntimeLogSink` 为互斥有界环形缓冲（最多 4096 条、10 分钟），输出单行 JSON 字段 `id/timestamp/occurred_at/level/component/operation/outcome/resource/resource_id/request_id/trace_id`；不写自由文本、正文或 error message，写失败计数通过 `write_failures()` 观测。
- 容量观测：`SnapshotRejections{oversized_bytes, oversized_items, oversized_reads}`；operation journal/owner feed journal 的 head/low-water/epoch 可查询；清理每 60 秒有界批次，单项失败只告警。
- 运维入口：初始化只用 `kiarabot-pg-initialize`（由部署编排调用）；reference 用 `kiarabot-migrate`；数据库/角色创建由部署完成，不属初始化 CLI。验证命令：`ctest --preset full-locked -R '<suite>' --no-tests=error --output-on-failure`、`python3 scripts/quality/check-dependency-lock.py`。
- 部署约束：sealed 委托文件只能走同主机/同用户共享卷，禁止 HTTP 暴露；多主机共享卷暂不支持；容器需禁用 core dump/限制 ptrace 以约束驻留进程内的委托 JWT。

## 10 测试与验收证据

- PG schema/仓储（真实 PG16.4 + TLS 夹具，`infrastructure/postgres/CMakeLists.txt` 注册）：`p03_postgres_database`、`p03_service_database`、`p04_identity_application`、`p05_postgres_bootstrap_delivery`、`p06_postgres_runtime_events`、`p07_postgres_action_records`、`p08_postgres_host_registration`、`p09_postgres_storage_records`、`p10_postgres_scheduler`、`ws02_postgres_operation_observations`；夹具源码 `infrastructure/postgres/tests/*_test.cpp` 与 `typed_work_checks.cpp`。
- 端到端（引用真实服务的 CTest）：`p06_client_e2e`、`p06_bridge_acceptance`、`p07_adapter_e2e`、`p08_host_bridge_e2e`、`p08_managed_host_e2e`、`p13_joint_delete_e2e`、`p14_native_single_host`、`p12_*`、`p11h_managed_model_e2e`、`p11_streaming_managed_model_e2e`，注册于 `infrastructure/postgres/CMakeLists.txt`、`cmake/platform.cmake`、`cmake/model-streaming-rpc.cmake`。
- 基础设施单元测试（`infrastructure/CMakeLists.txt`、`infrastructure/identity_crypto/CMakeLists.txt`）：`time_zone_resolver`、`http_model_provider`、`file_storage`、`sqlite_runtime_store`、`snapshot_limits`、`crash_recovery`、`p08_host_journal`、`authorized_commands`、`runtime_log`、`message_projection`、`migrate_cli`、`p04_*`（password/token/receipt/peer_certificate/delegation/key_files/verification_keys/assignment_fingerprint/principal_fingerprint）、`p11_model_secret_crypto`、`p11_streaming_sse_parser`、`p11_streaming_http_provider`。
- 夹具特性：`run_database_test.py` 创建 disposable cluster（SCRAM、仅 hostssl、随机端口、临时 TLS 证书），先以 CLI 验证非法参数/秘密文件（FIFO、符号链接、宽权限、多行），再用 `commit_receipt_proxy.py` 覆盖提交回执未知路径；不接受外部 DSN 或生产连接。

## 11 已知限制与后续

- 只支持显式空库初始化，不提供旧库迁移、快照导入或双写；生产升级需要维护者另行授权。
- descriptor/relation_count 必须每次 schema 变化后用真实 PG16.4 重新实测，手工预写数值不可信。
- 生产规模、满盘、多 writer 配额横压、备份/恢复演练（T11b）与 Docker 六服务栈仍属环境验收项。
- 部分 reference 端口未接线：`FileStorage` 无 Core 调用链，`INamespaceStore`/`IIdempotencyStore`/`IAuditLogger`/`IUnitOfWork` 系列在 reference 头中无实现，审计在 `RuntimeSnapshot` 内；不要按名字假定可用。
- `StorageCompareAndSet` 等 P09 冻结声明尚无生产实现；Host journal 的 prune 调用属 P13b；指标导出（log sink 失败、快照拒绝计数）仍待运维接线。
- SQLite 运行时快照为整库状态复制，未做容量/性能认证；多主机 sealed 委托共享卷暂不支持；真实付费外部模型 Provider 与替代 Runtime 叶证书的部署级负例未验收。
