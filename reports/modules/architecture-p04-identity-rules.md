# P04可执行纯规则能力

基点a11716c；只提交两个domain头、一个纯测试、doc/02与本报告。不含应用声明/实现、
PG或生产crypto，不修改P03UoW。状态规则基于权威输入值，不维护第二套内存数据库。

精确文件：
- core/include/kiarabot/core/domain/target/identity_policy.hpp
- core/include/kiarabot/core/domain/target/identity_state_rules.hpp
- core/tests/architecture/identity_rules_test.cpp
- doc/02-core.md
- reports/modules/architecture-p04-identity-rules.md

能力：40项封闭词表与固定角色allowlist；same-binding exact授权，拒绝角色×scope跨行
提权、Catalog继承及默认敏感MessageRead；服务/插件不继承human role。G08 Spec XOR、
非空/去重及human-only，更新版本/主体固定、old+new scope并集授权。授权水位候选只
+1与dirty=true，实际变更必须同事务CAS，uint64不可wrap。

每AssignmentId公开投影安全：role必须相同，explicit每scope相同完整permission product，
坏数据/混basis/同查询重复ID/跨主体Internal。project_assignment只能输出可无损映射的
单role或explicit permissions/scope，不重组权限。纯测试明确一个非product pair不能
产生扩大授权的AssignmentRecord。

self-session专用规则：ListSessions以本人Principal UUID；Revoke以目标Session UUID且
真实owner是当前human。跨主体要求exact grant，admin@Principal不自动revoke其Session。
不创造非UUID catalog或空subject全局发现。

当前principal/credential head/active/finite expiry、session当前轮次/期限/撤销总是先于
旧receipt。秘密恢复绑定/24h/稳定scope与异内容Conflict；旧refresh原key恢复、新key拒绝，
后续刷新/credential换代/撤销/过期都不能被旧成功结果绕过。结构ciphertext仅为状态测试
占位，未伪装AEAD或TokenCrypto；真正raw token验证和解密仍必须使用成熟adapter。

delegation显式来源current检查独立于authversion；hops1/[Control]到hops2/[Control,Runtime]，
禁止0/第三边、aud不变、权限扩大、origin替换或移除、期限延长。IdentityRPC/TLS不计跳。
Bootstrap marker只有Required->Consumed且条件版本，后续一次消费Conflict。

输入输出为内部类型的Outcome/Status/typed决策，无SQL/Proto/JSON/密码算法。依赖故障
由应用传入前传播，不可将错误查询当成nullptr/空集合；本能力不执行查询/事务/crypto。
文档同步doc/02与本报告；线协议已有G08，不改proto/API/其他模块/保护doc18。

测试覆盖正常、拒绝、精确时间边界、冲突、旧receipt优先级、源撤销而版本不变、完整
product投影、自有与跨主体会话、授权水位overflow及源/hops收窄。代码不含恒真验签替身。
生产应用和注入型unit tests随后继续；应用查询须显式传播Failure，并按当前源状态检查。

实际验证（2026-11-01）：
- C++20 `-Wall -Wextra -Wpedantic -Werror` 编译并执行 identity_rules_test：通过。
- 同测试 AddressSanitizer + UndefinedBehaviorSanitizer：通过。
- 固定 clang-format16 对两个头和测试 `--dry-run --Werror`：通过。
- `python3 scripts/quality/check-boundaries.py`：通过。
- `python3 scripts/quality/check-architecture.py --inner`：通过。
- `git diff --check`：通过（包括doc/02与本报告的空白检查）。
未运行全仓构建/PG验收：本能力是无依赖纯头规则，尚未纳入父任务CMake；不把局部
编译或架构gate声称为生产应用、真实数据库或RPC验收。
无需更新proto/API：本能力不改变已有线协议字段、错误码或调用选项；无需更新其他模块
文档及doc18：没有模块责任变化，doc18保持只读。
