# P04 CURRENT platform credential locator

目标仅 Identity PG 小查询。冻结 Core 接口0933dab（本地cherry fba5170）已先接入；解决
端口和staticassert追加冲突时保留既有bootstrap能力。PG cap只含 identity_security.cpp、
Identity schema/index/manifest、真实PG测试、doc05/08与本报告。先前Session cap3d33与
stored-corruption fix155c845保持独立，不包含Assignment/receipt variant或未提交Core。

输入 PlatformPeer {service,subject}；封闭service及非空/NUL-safe subject，否则
InvalidArgument。使用 credential_heads(subject,id,version)索引，通过当前id/version关联
certificate、精确platform_service；不查历史，不把全部主体/记录载入内存。零条成功空；
多个匹配Internal，不任选或LIMIT1。当前材料缺失会通过LEFT JOIN缺失分支进入损坏检测，
不是伪不存在；当前head材料/leaf与实际证书/subject/service错配Internal。数据库故障沿
原Failure映射及同一UoW回滚。返回Revoked/过期也只是定位；Core继续校验当前主体/owner/
head/证书/SAN/service/有限期限，不证明TLS possession或替换链上游actor身份。

Identity仅显式空库schema8，105 relations，PG16.4 owned rollback descriptor为
 d7dc9fe735f715e9ea800362cdd32ba1。Runtime56/Scheduler55/Model50仍v4且descriptor不变。
新增索引非唯一，明确允许检测歧义；生产schema_valid未放宽，旧库拒绝，不自动迁移/清库。

真实测试覆盖当前owner/head、service不符/缺subject空optional、错service枚举/空/NUL拒绝、
证书轮换只读最新且历史leaf不定位、当前Revoked仍返回、两个当前平台证书Internal、当前
certificate缺失或head指纹错配Internal及整UoW回滚。独立重开writer并真正terminate backend，
以by_platform为第一条失败SQL验证UpstreamUnavailable，保留bootstrap/session独立故障路径。

文档影响：doc02由Core cap冻结端口；本cap同步doc05实现行为、doc08当前schema与旧库拒绝、
本报告。doc01/proto/API/运行语义及其他模块无wire、错误码、投递、配置或责任变化无需改。
doc18只读。生成物、build脚本、秘密与集群材料不提交。

## Verification

工具根 `/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain`，与Session报告相同
锁定PG16.4/pqxx7.10/clang-format16/clang-tidy22。验收命令：

- `sdk/python/.venv/bin/python build/p03/verify_build.py`：initializer/normal/sanitized，
  C++20 -Wall -Wextra -Wpedantic -Werror。
- 同Python执行 `infrastructure/postgres/tests/run_database_test.py --pg-bin
  <工具根>/build/pg-16.4-install/bin --openssl <工具根>/build/openssl-3.5.0-install/bin/openssl
  --test build/p03/database_test --initializer build/p03/kiarabot-pg-initialize`，另替换
  --test为build/p03/database_test_sanitized。独立owned TLS verify-full/channel-binding四库。
- 三个修改cpp TU以clang-tidy22 `--checks=clang-analyzer-*,readability-braces-around-statements
  --warnings-as-errors=*`，固定system依赖与SDK；六文件clang-format16 dry-run。
- Core identity_contract_test以C++20 -Werror -UNDEBUG独立编译执行通过，bootstrap和platform
  两个staticassert均保留。文档UTF8/末尾换行、protected-doc无diff及git diff --check。
- 未运行完整root CMake/CTest/App/RPC：父负责Login/Check/Exchange/其他strictfixture成组集成；
  本分支未取父Login081dbb5，不能通过伪造默认Control或改Core来冒充全链路验收。

最终命令结果在交付前记录。

最终结果：三个目标-Werror构建通过；normal与ASan+UBSan真实四库完整通过，两fixture
均清除owned集群及私密材料。六文件format dry-run、UTF8/换行/protected-doc/diff通过。
三个TU tidy首轮仅因测试刻意构造非法Service枚举6而报EnumCastOutOfRange；该负向测试
行添加精确NOLINTNEXTLINE和原因后重跑通过（仅抑制这一个已知测试诊断，生产无抑制）。
Core契约测试通过。未运行门禁及原因保持如上，不声称P04全链路完成。
