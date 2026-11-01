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

## 安全 schema 实施范围

下一步新增 Identity v4 关系和约束；doc/05 与 doc/08 已补充初始化版本、安全投影、
恢复窗口与显式引导标记说明，状态均为实施中。Core 端口是唯一内部语义来源，
不改 proto/API、稳定码或保护基线。验收要求四库逐库版本/manifest 严格匹配，
凭据头与历史/证书当前查找、会话和授予复合 FK、24h 收据/refresh 约束及标记初始化。

### schema 实际验证

Identity v4 的固定 PG16.4 descriptor 为 `d1aa0822c7286c329a60802278aaf62c`，
关系对象 100；另三库的版本、对象数和 descriptor 保持原值。通过独立自有临时库的
开发 probe 获取实际描述，再以正式严格等值校验运行全部 fixture，未放宽启动验证。

已执行 `build/p03/verify_build.py` 三种固定依赖 -Werror 构建，普通及 ASan+UBSan
`run_database_test.py` 各自独立创建四库，全部通过并安全清理。
新增真实约束验证覆盖 Pending 材料/状态、head 历史 FK、当前证书 fingerprint FK、
会话 version/rotation/expiry/revoked 一致、grant 版本一致、Permission40 接受/41 拒绝、
AEAD 纳秒与00ff密文、24h收据上限、refresh原peer/key关联与期限、清理级联和初始引导标记。
clang-tidy22 对 database.cpp/database_test.cpp、clang-format16、git diff --check 通过。
文档校验检查三个已更新 Markdown 的 UTF8/换行与 P04 说明；保护基线无差异。
本步无新增翻译单元、不改 CMake；根完整集成仍由主任务/工具链任务执行。
安全仓储与生产接线仍在实施，schema 通过不代表 P04 用例已完成。

## 凭据与引导仓储（实施中）

新增私有 `src/identity_security.cpp` 和 `.hpp`，由 SqlWork 借用同一事务；尚未提供
完整的安全工厂，不能声称安全应用已接线。凭据写入先校验 Core 纯规则/文本表示，再以
expected head version 条件推进并追加独立类型历史，登录索引也使用 CAS。证书查找仅
通过唯一 current-head fingerprint，随后核对当前版本实际材料；历史证书仍可按明确版本
读取但不能解析为新的当前头。引导查询缺失标记为 Internal，消费仅 Required+expected
条件更新；继承仓储和 audit 同一原子提交边界不变。

Audience 使用 position 关系列无损保留 Core vector 的顺序与内容，避免仓储擅自重排或
去重验证材料；相应 Identity v4 descriptor 更新为 `d74a1a8ae6ec77c1178dfae43d904edc`，
对象数仍为100。本变更仍为新空库 schema，无既有库自动升级。
需同步工具链库源清单加入 `infrastructure/postgres/src/identity_security.cpp`；本任务不改 CMake。
已执行并通过固定依赖初始化 CLI/普通/ASan+UBSan 三种 -Werror 构建、两套独立真实
PG 四库 fixture，以及新仓储/公共测试两 TU 的 clang-tidy22。clang-format16 与
文档 UTF8/换行/章节检查、git diff --check 通过；保护基线无差异。
测试覆盖三种已发行材料与 Pending、纳秒、audience顺序/重复内容、同材料撤销追加、
版本历史保留、证书轮换新旧指纹、登录CAS、错误CAS、文本NUL、材料损坏Internal、
引导条件消费/回滚/错误版本/标记缺失Internal。
开发中真实测试捕获 bytea 重复hex解码，已修正为复用既有摘要bytea编码并重跑全部检查。
根构建未在本工作树运行：CMake源接线由工具链任务负责；新增源需完成该接线再跑根门禁。
