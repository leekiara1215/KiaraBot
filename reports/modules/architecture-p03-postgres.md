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
