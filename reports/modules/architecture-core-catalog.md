# Core Catalog资源kind最小扩展

基点a7d5577，遵照已确认唯一契约0de1622；不修改wire/SQL/P03UoW接口或P04未提交文件。
内部ResourceKind末尾新增Catalog=15，原0–14不变；persistence_rules封闭switch增加case。
Catalog只是资源kind与精确ID的表达，不是通配或层级授权；不继承子资源权限。

精确文件：
- core/include/kiarabot/core/domain/target/foundation.hpp
- core/include/kiarabot/core/domain/target/persistence_rules.hpp
- core/tests/architecture/persistence_contract_test.cpp
- core/tests/architecture/persistence_rules_test.cpp
- doc/02-core.md
- reports/modules/architecture-core-catalog.md

测试新增全部旧枚举值0–14/Catalog=15的静态兼容约束；同字符串ID下Catalog与Account/
Model收据scope不碰撞，与Task交接内容不相同；Catalog非空ID结构接受、空ID拒绝。
真实目录成员/授权policy不由持久结构校验决定，P04有限目录规则后续独立实现。

文档影响：同步doc/02与本报告；不改proto/API、运维、受保护doc/18或其他模块；
infra需另行扩PG resource_kind CHECK到15，不重编号旧值。wire值16按名字映射，
不可直接cast为内部15。边界映射接线与真实PG验收不在本提交范围。

权限词表核对：0de1622原文追加的是ACCOUNT_READ、PLUGIN_READ、ADAPTER_READ、
PLUGIN_CONFIG_READ（36–39），没有INSTALLATION_READ/INSTANCE_READ。
P04未提交enum已按原文映射；后续契约有增量再更新，不自行制造别名或放行unknown。

实际检查：foundation/invariants/persistence_contract/persistence_rules/time五项相关测试及
另行未提交identity_contract按C++20 -Wall/-Wextra/-Wpedantic/-Werror重编译并执行通过；
固定format16 dry-run、check-boundaries.py、check-architecture.py --inner、git diff --check
通过。未运行PG/SQL迁移或扩授权策略，不能将结构测试当作目录授权/真实存储验收。
