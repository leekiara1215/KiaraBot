# P04 complete ordinary public result codec

在Assignment状态PG eacf42d后独立能力。精确Core RegisteredPrincipalSnapshot13=15fd119、
ExactAssignment14=0bce522先接（本地62978fa/462430e），保持0..12和SessionMetadata12。
冲突只保留既有variants并追加两种类型/doc原文；没有引入其他工作树未提交内容。
允许PG schema/shared record_codec及其头、真实fixtures、doc05/08与本报告；不改Core应用、
crypto、CMake、wire/API或doc18，也不启用未接线Register/Set/Revoke路径。

13使用result_registrations保存原Principal的id/kind/optional binding/version/state及原
initial_credential ref/subject/binding/status/created_at/optional expiry/checked_at。14使用
result_exact_assignments及positioned role/permission children保存完整原id/owner/version和
exact tuples，roles XOR permissions，按原顺序还原；parent计数/连续position检测缺失子行。
快照表只FK replay_results，不FK或查询mutable live authority，不从metadata逆拼，不把
SecretReplayPayload/token/proof/verifier存到普通收据。七天普通保留与24小时秘密收据不混用。

新输入坏text/NUL/公共shape或invalid exact tuple返回InvalidArgument；读到缺表行/子行、
position/count/enum/typed shape损坏为Internal，依赖故障Failure。valid_text(ReplayResult)
移除内部noexcept以使新增领域shape验证的分配异常正常传播同UoW Failure，不触发terminate；
旧纯文本overloads不变。重复原fingerprint返回原完整结果与retention，忽略新proposed结果，
不以当前状态/改写后的proposed补齐旧成功。Unknown必须重连按原键查收据/权威当前状态。

父review要求的另一私有防御一并修复：load_live Grant在key[0]前验证key.size()==1，空span
或两段key直接静态异常，经CommonWork转换Internal并回滚；常规typedrepo仍传1段key。
负向真实SqlWork测试覆盖两种shape，无UB、无新业务语义或协议变化，不重写先前状态cap历史。

## Four fresh schemas

共享kind扩展0..14和八个新增table/index关系要求四库提升，不自动升级/清空旧库。固定PG16.4
owned marker+data_directory验证后的rollback catalog probe产生：

| owner | version | relations | descriptor |
| --- | --- | --- | --- |
| Identity | 10 | 113 | abe04dcae86c5b5ace3cdd1408098ae5 |
| Runtime | 5 | 64 | 22b898cd0ec7332932b7ed82e9e23411 |
| Scheduler | 5 | 63 | 525e69fbdf63d0437b2d7cc6e82c6408 |
| Model | 5 | 58 | b15cfb7c638ddb1a37159b66fdc0633b |

生产schema_valid仍检查完整descriptor/ownership/permission；未放宽验证或编辑生成物。

## Real verification

四库generic快照测试覆盖全部0..14，uint64 max、signed64 UTC纳秒端点、原顺序explicit
permission矩形、固定role精确scope；13覆盖principal binding有/无、credential expiry有/无、
完整状态/原created/checked。重复同键时故意改proposed版本/时间或清空tuple，仍读到原结果。
嵌套NUL、owner错配/empty exact shape拒绝；实际删13父快照或14位置子行均Internal且回滚。
独立backend终止后以13/14 receipt find分别为首条失败SQL验证UpstreamUnavailable。真正proxy
丢COMMIT回执使Unknown，重连读到原registration1/Pending而live credential2/Active、原exact1
完整role scope而live assignment2/Revoked，证明未回查当前拼历史结果。

工具根 `/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain`，固定PG16.4/pqxx7.10/
clang-format16/clang-tidy22。实际命令：

- `sdk/python/.venv/bin/python build/p03/verify_build.py`：initializer、database_test、
  database_test_sanitized，C++20 -Wall -Wextra -Wpedantic -Werror。
- 同Python `infrastructure/postgres/tests/run_database_test.py --pg-bin
  <工具根>/build/pg-16.4-install/bin --openssl <工具根>/build/openssl-3.5.0-install/bin/openssl
  --test build/p03/database_test --initializer build/p03/kiarabot-pg-initialize`，另一命令替换
  --test为database_test_sanitized。各自owned TLS verify-full/channel-binding真实四库。
- 三修改cpp TU clang-tidy22 analyzer+braces warnings-as-errors；既有故意非法Service负测一处
  定点NOLINT保留，生产无抑制。八个C++/schema文件clang-format16 dry-run。
- 两个Core frozen snapshot tests分别C++20 -Wall/-Wextra/-Wpedantic/-Werror编译执行通过；
  明确0..14 index/count及SecretReplayPayload不可构造为ordinary result。
- UTF8/末尾换行、protected-doc无diff、git diff --check。

文档影响：Core cap更新doc02及registration模块；PG cap同步doc05、doc08（四库当前空schema、
快照行为/故障）和本报告。proto/API/doc01/运行语义/其他模块没有wire、稳定错误码、投递、
配置或职责变化无需改，doc18只读。未跑root CMake/CTest、全App/RPC：父并行集成Register/
Set/Revoke及其他cap，此分支未取父应用/组合改动，不通过修改Core伪造全链路通过。

最终结果在提交前记录。

最终结果：三目标最终-Werror构建通过；normal与ASan+UBSan各自完整真实四库通过并清除
owned集群/私密材料。三TU tidy、八文件format、两Core snapshot test、UTF8/换行/受保护
文档无diff及git diff --check全部通过；只有既有故意非法Service负测一处定点抑制。
