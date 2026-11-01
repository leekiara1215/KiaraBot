# P04 Identity PostgreSQL 安全持久化（实施中）

## 范围与文档影响判断

目标模块为 `infrastructure/postgres/`，仅修改该目录、`doc/05-infrastructure.md`、
`doc/08-operations.md` 和本报告；CMake 由工具链任务接线，Core 端口由 Core 任务维护。
输入为已确认的 `IdentitySecurityUnitOfWork` 及安全值类型，输出为同一物理本地事务内的
关系仓储。仓储失败阻止部分提交；错误沿用 Core 的 Failure，不引入新稳定码。

完整 P04 将同步 doc/05 的仓储行为和 doc/08 的 Identity 初始化版本、引导与恢复要求。
本次第一步仅增加私有 SQL 借用入口，无外部可观察变化，因而当前无公共文档影响：
不修改 proto、API、运行语义或模块边界；不编辑受保护的 doc/18。

## 同事务基础

私有 `DatabaseAccess` 在创建具体 CommonWork 时同时返回拥有的 Core UoW 和非拥有
SQL 借用接口，不对旧 Identity 工厂或 UoW 做向下转换，也不启动第二个事务。
借用回调仅允许基础设施的同步本地 SQL/codec，禁止远程、crypto、用户回调与提交。
它复用原事务的 2 秒总预算、250ms 锁预算、失败污染和连接失效处理。
终态后接口拒绝回调；借用生命周期严格小于拥有的 UoW。

## 验收与检查

必须以真实 PG16.4 TLS/SCRAM fixture 验证：继承仓储写入在 SQL 借用中可见；
借用返回 Failure 后继承写入整体回滚；终态不能再执行 SQL；下一事务可继续取得 permit。
已执行并通过：

- 本地固定依赖 `build/p03/verify_build.py`：初始化 CLI、普通 driver、ASan+UBSan driver，
  C++20 `-Wall -Wextra -Wpedantic -Werror`。
- `run_database_test.py --test build/p03/database_test` 与 sanitized driver：
  两次独立自有临时 PG16.4/TLS/SCRAM 四库 fixture 全部通过并清理。
- clang-tidy22 对 common_work.cpp、database.cpp、common_work_checks.cpp，
  `clang-analyzer-*` 且 warnings-as-errors，通过。
- clang-format16 对全部六个变更 C++ 文件及 `git diff --check`，通过。
- 报告文档检查：UTF8、末尾换行、范围与无文档影响理由存在；doc/18 无差异。

未重跑根完整 CMake/CTest/toolchain：本步骤未改接线，完整集成由主任务和工具链任务执行。
本地固定构建和真实数据库测试覆盖本步骤的全部变更；不将其声称为根门禁通过。
本步骤不改变 schema；后续 Identity 安全 schema 使用独立版本 4，其余三库保持版本 3。
