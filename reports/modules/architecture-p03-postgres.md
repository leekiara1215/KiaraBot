# P03b PostgreSQL 实施记录

## 范围与文档影响

目标模块为 infrastructure PostgreSQL；仅允许新增 `infrastructure/postgres/{include,src,schema,migrate,tests}`，
以及本文、`doc/05-infrastructure.md`、`doc/08-operations.md` 的最小追加。
不修改 Core、旧 SQLite、协议/API、CMake 或冻结 `doc/18`。

输入为 Core 独立值类型及端口；输出沿用 `Outcome`/`Failure`，不泄漏 pqxx 异常或 DSN。
连接/初始化输入为离散数据库配置，不接受任意 DSN 覆盖 TLS 与超时。
absence 成功返回空 optional；冲突、依赖故障、数据损坏分别为 Conflict、UpstreamUnavailable、Internal。
提交回执丢失必须 Unknown，不能自动重跑。

需同步 doc/05 的存储行为及 doc/08 的显式初始化/权限/启动约定。
无需同步 proto/API：本能力不改变线协议；无需修改 doc/00：依赖方向保持。
`doc/18` 只读，与其旧单体/SQLite描述的差异沿 doc/19–23 已确认方向记录，不修订原文。

## 首次端口审查（基点 13cac3a）

现有 `unit_of_work.hpp` 仅 Runtime typed UoW。Receipt 缺完整可回放结果、创建时间及容量参数；
资源引用没有 kind；outbox 确认缺 source/当前时间；inbox 保留与接收时间不充分；
Identity/Scheduler/Model 尚无 typed UoW。上述缺口交 Core 所有者，不在 infrastructure 复制接口。
先实现连接、显式空库初始化/验证和活动写入锁；Repository 等审查后的 Core 提交。

## 验收与集成

依赖固定 libpqxx 7.10.0、PostgreSQL 16.4、OpenSSL 3.5.0，C++20。
真实临时集群使用独立临时目录/marker，仅回环、SCRAM、verify-full/channel_binding=require，
不接收用户 DSN，不读取 PG* 环境，不连接生产。清理只针对自己持有的临时集群。
每个能力先执行 -Wall -Wextra -Wpedantic -Werror 构建、format16 和真实 PG 测试，再独立提交。
实际命令、结果及 CMake 接线需求随实现追加。P02统一门禁未全通过前不宣称P03阶段完成。

## 能力一：四库连接、显式初始化、schema 和单 session writer

新增源文件：

- 库 `kiarabot_postgres`：`infrastructure/postgres/src/database.cpp`；公共 include
  `infrastructure/postgres/include`，依赖 Core 内部 domain include (`core/include`) 与 `libpqxx::pqxx`。
  schema 唯一实现是 `infrastructure/postgres/schema/v1.hpp`，无需代码生成。
- CLI `kiarabot-pg-initialize`：`infrastructure/postgres/migrate/initialize_main.cpp`，链接库。
- driver `kiarabot_postgres_database_test`：`infrastructure/postgres/tests/database_test.cpp`，链接库与
  `libpqxx::pqxx`（测试直接以专属管理员部署/注入故障），POSIX process API，无额外业务依赖。
- CTest 建议 `p03_postgres_database`，labels `p03;integration;fault`，timeout 60：
  `${KIARABOT_TARGET_PYTHON} infrastructure/postgres/tests/run_database_test.py
  --pg-bin ${KIARABOT_TEST_PG_BIN} --openssl ${KIARABOT_TEST_OPENSSL}
  --test $<TARGET_FILE:kiarabot_postgres_database_test>
  --initializer $<TARGET_FILE:kiarabot-pg-initialize>`。
  Python 脚本缺工具或非16.4明确失败，无 skip。生产入口接线仍待后续服务能力。

schema v1 是存储基础标记，不含业务 Repository 表，不表示 P03 或业务 ready。
每库初始化/版本/服务 owner 独立；只接收空用户 schema，DDL+版本在同一事务。
数据库拥有者和两登录权限/角色成员关系均检查；运行登录不得修改版本或做 DDL。
运行库持有**同一个**PG session的 advisory lock，后续UoW必须借用该session，禁用连接池/重连。
check失败永久disabled。当前测试杀backend后旧对象连续失败，新对象能重新获取锁；
旧写事务不可commit的测试在typed UoW能力补齐，不把当前基础测试冒充该项已通过。

### 实际验证（2026-11-01，专属工作区）

本次确实使用工具链工作区的固定依赖，不引用别人的通过结果：

```bash
# 下列 DEP_ROOT/TCHAIN 仅为人工复现占位，不写入任何源码/CMake：
TCHAIN=/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain
DEP_ROOT=$TCHAIN/build/architecture-installed/arm64-osx
c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -Iinfrastructure/postgres/include -Icore/include -isystem "$DEP_ROOT/include" \
  infrastructure/postgres/src/database.cpp infrastructure/postgres/tests/database_test.cpp \
  -L"$DEP_ROOT/lib" -lpqxx -lpq -lpgcommon -lpgport -lssl -lcrypto \
  -framework Security -framework CoreFoundation -o build/p03/database_test
c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -Iinfrastructure/postgres/include -Icore/include -isystem "$DEP_ROOT/include" \
  infrastructure/postgres/src/database.cpp infrastructure/postgres/migrate/initialize_main.cpp \
  -L"$DEP_ROOT/lib" -lpqxx -lpq -lpgcommon -lpgport -lssl -lcrypto \
  -framework Security -framework CoreFoundation -o build/p03/kiarabot-pg-initialize
"$TCHAIN/sdk/python/.venv/bin/python" infrastructure/postgres/tests/run_database_test.py \
  --pg-bin "$TCHAIN/build/pg-16.4-install/bin" \
  --openssl "$TCHAIN/build/openssl-3.5.0-install/bin/openssl" \
  --test build/p03/database_test --initializer build/p03/kiarabot-pg-initialize
```

两条-Werror构建通过；真实fixture通过，正常关闭并删除自己临时目录/秘密。
覆盖四库显式CLI初始化、缺schema启动拒绝、重复初始化拒绝、DDL故障中途完整回滚后重试、
未知schema/缺版本记录拒绝、20项运行账户DDL/版本修改拒绝、24组跨私有库连接拒绝、
每库第二writer拒绝、销毁后重开、kill backend/旧对象永久失效/新实例恢复、
错误TLS主机名/缺CA拒绝。没有使用外部DSN、关闭TLS或memory/SQLite。
首次手工静态链接缺pgcommon/pgport失败，补全固定包传递库后通过，未改锁或第三方源码。

尚未运行根full/CTest/tidy/统一工具链：不允许本分支改CMake，等工具链分支接线；
本次手工-Werror/真实PG测试不替代这些检查，不宣称P03完成。
格式、diff和文档校验在本能力提交前运行，结果追加于下方。

提交前实际检查：format16 `--dry-run --Werror` 覆盖全部5个新增C++源/头，通过；
`git diff --check`通过；固定Python3.11 `-m py_compile` fixture通过；
Python pathlib检查上述三个文档的P03章节/结尾换行、报告内5个source路径存在，均通过。
未更新文档及理由同开头文档影响判断；未修改协议/API，因此没有新增契约解析任务。

### Fixture 安全收尾补强

后续检查发现 TemporaryDirectory 在 marker 变化或 pg_ctl 停止失败时仍会自动递归删除目录。
改为显式拥有目录：清理前验证 prefix/非符号链接/uid/marker token，停止失败或所有权不明时
保留目录，绝不自动删除可能仍活动的集群。密码文件描述符增加 close-on-exec。
无公共文档影响：生产输入输出/错误/接口均保持，doc/05、doc/08 无需重复改动。

同一固定-Werror命令重编CLI、format16和diff检查均通过；真实PG fixture本人再次通过。
增加实际CLI --help/缺参数、FIFO两秒内拒绝、符号链接拒绝、0644密码拒绝、多行密码拒绝，
均在创建集群前完成；不读取或打印秘密。文档仅本报告记录测试生命周期差异。

### 空库/启动对象检查的回归修复

自查发现 SQL `NOT LIKE 'pg_%'` 的下划线是通配符，会漏掉用户schema `pga`。
改用固定三字符前缀判断，同时启动验证拒绝多余用户schema及public对象。
这是恢复已说明的“只接受空库/匹配schema”约定，无新增公共语义，doc/05和doc/08无需变化。
真实PG四库分别创建pga：初始化拒绝、移除后可初始化；成功后再加pga启动拒绝，移除后恢复。
两条固定-Werror构建、完整真实fixture、变更C++ format16检查及diff检查通过。

## COMMIT 回执丢失真实故障夹具（尚非UoW验收）

新增测试模块 `infrastructure/postgres/tests/commit_receipt_proxy.py`，现有Python runner自动启动，
对外CTest参数/target不变。代理只在回环透传加密字节，TLS/SCRAM仍由libpq与PG端到端完成。
driver在前一条SQL已返回后用本次临时目录标记arm，并等代理确认；下一条客户端命令为COMMIT，
代理转发该命令、丢弃PG第一条响应并关闭连接。独立直连会话必须查到已提交行，
单纯断线/事务回滚不能使测试通过。

真实PG16.4已实跑通过：pqxx返回in_doubt/broken_connection，独立连接确认提交已持久化，
测试表随后在本专属fixture内清理。TLS验证与通道绑定全程开启，无SSL解密/降级。
这验证故障夹具本身，不冒充待实现Core UoW的CommitResult::Unknown映射；后者收到接口后补验。
-Werror重编driver、真实fixture、format16、Python编译、diff检查通过。
无公共文档影响：只新增测试故障能力，doc/05、doc/08约定未变。

## 能力二：公共UoW、关系结果快照与可靠交接（实施中）

已按父授权cherry-pick审核接口110cf32（本分支cbd49c6），不自行造Core接口。
输入为Core纯typed值，输出沿用Outcome/Failure/CommitResult；无新线协议或稳定错误码。
需同步doc/05短事务/版本/结果存储行为；doc/08的显式初始化原则保持，schema变化时追加版本说明。
其他模块文档无需改动，doc/18只读。

实现计划：schema扩为公共关系表及每ReplayResult分类型不可变快照；
公共UnitOfWork复用持锁session，不透明重连；2秒总预算/250ms锁等待；
完整uint64值以NUMERIC(20,0)+范围CHECK保存。Receipt/Journal容量仅可信应用参数，
当前时间样本不是租约权威，条件SQL用clock_timestamp原子重判。
实际验收须用本真实TLS代理令UoW commit返回Unknown，再用原身份查回已提交收据。

### 组合根facade声明（独立接口提交，实现随公共/typed UoW补齐）

新增 `include/kiarabot/infrastructure/postgres/factories.hpp`，声明open_identity/open_runtime/
open_scheduler/open_model(const DatabaseConfig&)，返回Core Outcome<unique_ptr<对应Factory>>。
外层创建API不暴露pqxx；每factory单session/单writer，同session许可让并发begin最多等250ms，
忙Conflict，不偷偷重连。许可持续至commit/rollback/析构；单个UoW仍单所有者。
健康采样只经同factory begin→current_time→rollback，不另建连接或并发check原始Database。
此提交仅声明，定义正在本能力实现，不把接口存在当作可链接或已完成数据库行为。
声明按format16检查、C++20 -Werror独立头语法检查和diff检查通过；无可执行行为变化，
本接口提交无需新增doc/08；doc/05已在能力二的同变更内说明。真实PG证据仍为上节已提交能力。

### 公共能力实现与版本说明

Core固定ns精度提交a7d5577已授权引入（本分支ac8405d）；代码static_assert目标duration
为纳秒。没有把Apple原生system_clock（本机实测微秒）假定为纳秒，也未在旧定义上乘1000
冒险截断TimePoint端点。曾评估timestamptz，但其微秒精度会破坏不可变envelope回放，
最终统一使用Core signed64 epoch ns BIGINT，DB clock_timestamp经精确NUMERIC产生权威UTC，
所有时间条件写在SQL重判；版本仍NUMERIC(20,0)完整uint64，与时间有符号域分开。

本能力schema为v2，仅显式空库初始化，不提供旧v1自动升级/清库；公共表及12种分类型
ReplayResult快照全部关系化（Grant scopes单独子表，Task payload受限bytea），无万能JSON/BLOB。
指纹使用数据库catalog描述MD5校验列/默认/identity、约束/validated/deferrable、索引、
domain和sequence，仅作固定PG16.4结构校验，非密码学安全机制。owner/RLS/表权限/禁止
用户触发器与函数另查；版本表表级及列级写权限都拒绝。UTF8库/客户端编码均验证，notice抑制。

CommonWork借同session，二进制semaphore许可持续至终止；并发begin等250ms后Conflict。
2秒总预算包括阻塞网络/rollback：timer只shutdown由F_DUPFD_CLOEXEC持有的相同socket，
pqxx仅在UoW所有者线程操作；复制fd防连接断开/原fd复用误关其他资源，stop/join后归还许可。
任何仓储失败标记本UoW不能提交部分结果；Unknown保留终止结果，析构/rollback不改写，
永久禁用且释放旧连接，不透明重连。COMMIT异常不泄漏pqxx正文，不自动重跑。

复用审核Core b2761da（本分支a66bc44）check_receipt_write/check_outbox_enqueue/
check_inbox_write/validate_claim_request，保留SQL唯一/短锁/CAS与DB实际UTC重检。
Receipt新写由DB设置创建时间并保证至少168小时，重放返原快照/期限；普通/控制lane容量
分开，gen_random_uuid产生claim且唯一索引兜底；lease=min(DB now+lease,valid_until)，
confirm匹配source+message+当前token/未过期租期，重复同token确认不重复处理。

### 工具链接线变更（取代能力一的最小source列表）

- `kiarabot_postgres`实际库源：`src/database.cpp`、`src/common_work.cpp`、`src/record_codec.cpp`，
  均相对于 `infrastructure/postgres/`；链接 `libpqxx::pqxx` 和 `Threads::Threads`。
- 同一CLI source/target不变，链接上述库。
- driver source增加 `infrastructure/postgres/tests/common_work_checks.cpp`（与database_test.cpp一起）。
  测试header `common_work_checks.hpp`，内部header session.hpp/record_codec.hpp，schema common.hpp/
  manifest.hpp为手写唯一实现，无生成步骤。CTest命令/labels与fixture参数保持。
- `factories.hpp`声明对应定义及typed live Repository在下一小步；本提交只使Database::begin()
  实现Core公共UnitOfWork，不以公共基类取代四个typed工厂。

实际本地构建沿用上节固定路径、-std=c++20/-Wall/-Wextra/-Wpedantic/-Werror和传递静态库；
两条命令分别以上述3库source+2driver source、3库source+CLI编译，不使用别人的产物代替自己。
真实fixture沿用相同Python命令，独立测试集群/四库/TLS/SCRAM；结束安全移除自己材料。

### 公共能力实际验收（2026-11-01，未关闭P03阶段）

固定依赖本人两条-Werror构建与完整真实PG16.4/TLS/SCRAM fixture通过（UTF8）：

- 四库operations create/absence/CAS/冲突与失败回滚，2^63/UINT64_MAX无损，更新溢出显式拒绝。
- 12种关系快照跨UoW重放，scope子表、可选fence/host、二进制00/ff、平台前导零、Unicode；
  完整operation owner/resource、137ns/731ns时间低位、TimePoint::min/max均精确保留。
- 新receipt至少168小时，满容量仍可同hash原结果重放且不续期；异hash拒绝；resource kind隔离。
- 普通/控制outbox容量隔离、同内容重复、不可信建议now不能决定领取/确认/清理。
  claim持久token/lease，实际失效后重领换token/增加attempt，旧token无法确认新claim；
  同token重复确认安全。inbox以source+message去重并保存完整原结果，改变hash拒绝。
- inbox保留至少max(接收,valid_until)+168小时，未来now不能提前清理；测试迁移身份仅在
  临时fixture把记录整体移到过去后有界清理，仍有效的7天记录保留，清理后过期新消息拒绝。
- 同UoW业务+receipt+audit+outbox后容量失败，四者全部回滚；独立真实PG session持行锁时
  250ms Timeout，之前audit也回滚。没有通过查空值掩盖故障。
- 同factory并发begin250ms Conflict；2秒deadline使旧写回滚/旧实例禁用、新实例可恢复；
  杀持锁backend后旧活动事务不能commit，新会话确认killed-write不存在。
- TLS代理现在运行的是**实际Core UnitOfWork commit**：Unknown、rollback不能改写、旧实例
  不重连；新实例按原receipt key读回UINT64_MAX完整原结果，证明PG确已提交而非仅断线。
- 原四库权限/TLS/初始化/半初始化/重启用例全部重跑；同计数下替换outbox_due索引被拒绝，
  恢复正确索引后可启动。schema descriptor最终严格启用，无跳过/回退分支。

增加 `-fsanitize=address,undefined -fno-omit-frame-pointer` 重编3库源+2driver源，
使用同一真实fixture独立再跑通过，无sanitizer报告；测试前置缺失会失败，不跳过真实PG。
固定tidy22对database/common_work/record_codec/CLI/database_test/common_work_checks共6个TU，
`--checks='clang-analyzer-*' --warnings-as-errors='*' --header-filter='infrastructure/postgres/.*'`
并显式Apple SDK/C++头，实际通过；此为本能力定向分析，不冒充全仓编译覆盖/统一tidy。
format16对所有本能力源/头dry-run通过；固定Python3.11两fixture py_compile通过；
Python pathlib文档章节/路径/换行校验、git diff --check、与13cac3a比较受保护doc/18无差异均通过。
文档实际更新doc/05、doc/08及本报告，未更新proto/API/其他模块的理由同本节开头影响判断。
根full/CTest/统一工具链仍由工具链接线后主集成执行，本分支无权修改CMake；
typed live Repository/工厂定义与完整100次派发/进程接线等后续能力未完成，不宣称P03完成。
