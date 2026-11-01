# P04 Assignment state PostgreSQL capability

目标 Identity PG；先接精确冻结Core f4caa56（本地8dae281，doc02追加冲突保留双方），
领域规则1f7d43e已在基线。允许 identity_assignments/record_codec、Identity schema/manifest、
PG真实fixtures、doc05/08与本报告。无Core应用/crypto/CMake/proto/API/doc18更改，不包含
RegisteredPrincipalSnapshot13/ExactAssignment14普通收据codec；其后独立cap负责。

输入 current(AssignmentId)/save_state(AssignmentStateRecord,expected)，原save委托Active。
只允许Active v1创建；后续同owner CAS推进1，或Active→Revoked。形状/text/version错误
InvalidArgument，损坏领域状态Internal，旧owner/version/revoked再写/复活/复用/撤销同时换
scope为Conflict。revoked_at须有限正UTC、不晚于实际DB UTC；撤销仅更新版本/state/time，
不删除或改写旧tuple。save_state独自同步base同事务，不再把重复base写入认作成功。
current包括墓碑，校验typed shape与base owner/version；无匹配空optional。for_subject及
base load只返回有效Active，store_live Grant拒绝所有已Revoked ID。没有墓碑TTL、物理删除
或空tuple撤销。任一拒绝令完整UoW回滚；Unknown不假定未提交，必须重读原状态和收据。

Identity空库schema9，105 relations，PG16.4真实owned rollback descriptor为
81ef6076f7c180c888ca705a0cba75f0；Runtime56/Scheduler55/Model50仍v4且checksum不变。
exact_assignments增加state/revoked_at及有限值CHECK，保留同base owner/version延迟FK。
严格schema_valid未放宽，无自动升级/填默认/清库。

真实PG覆盖Active create/update、current/absence、错owner/CAS/撤销换scope/未来时刻拒绝、
Revoked oldscope/time/version持久、for_subject/base过滤、save/save_state/base绕墓碑复活与
换owner拒绝、缺tuple损坏Internal、createRevoked拒绝、重复base写使撤销整UoW回滚并核对
exact/base原Active v2。真正proxy丢COMMIT回执后Unknown，重开实际PG读到原墓碑/时间/scope，
base不返回有效grant；独立真实backend终止后current和save_state分别首条失败SQL均为
UpstreamUnavailable。SQLSTATE23514拒绝state/time组合，既有权限/投递/收据测试继续运行。

文档影响：Corecap doc02冻结契约；PGcap更新doc05行为、doc08当前schema/运维及本报告。
现wire Revoke只返回版本，稳定错误码未新增，proto/API/doc01/运行语义/其他模块职责配置
无变化不需更新；doc18只读。普通成功完整typed快照后继cap另更新，不用live query拼回放。

## Checks

工具根 `/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain`，锁定PG16.4/pqxx7.10、
clang-format16、clang-tidy22。同一worktree运行：

- `sdk/python/.venv/bin/python build/p03/verify_build.py`：三目标initializer、database_test、
  database_test_sanitized，C++20 Wall/Wextra/Wpedantic/Werror。
- 同Python执行`infrastructure/postgres/tests/run_database_test.py --pg-bin
  <工具根>/build/pg-16.4-install/bin --openssl <工具根>/build/openssl-3.5.0-install/bin/openssl
  --test build/p03/database_test --initializer build/p03/kiarabot-pg-initialize`；另替换--test为
  database_test_sanitized。真实owned TLS verify-full/channel-binding四库，完成后删除owned私密材料。
- 四个修改TU：tidy22 analyzer+braces warnings-as-errors，通过，沿用by_platform故意非法枚举
  负向测试一处精确NOLINT，生产无抑制。七个C++/schema文件format16 dry-run。
- 文档UTF8/末尾换行、protected-doc无diff、git diff --check。
- 未运行root CMake/CTest、全App/RPC：父负责并行Assignment应用与其他接口成组，当前不取
  未提交实现或改Core以冒充完成。此独立存储cap不宣称P04整体关闭。

最终检查结果在交付前记录。

最终结果：最终三目标-Werror构建、normal与ASan+UBSan真实四库完整通过；四TU tidy及
最终common TU复查通过（仅既有故意非法Service负向测试定点抑制）。七文件format、文档
UTF8/换行/protected-doc/diff通过。普通完整typed快照未混入，交由下一个独立codec cap。
