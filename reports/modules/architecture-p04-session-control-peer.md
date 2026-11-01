# P04源会话Control绑定类型能力

独立于Revoke与Register/Rotate。目标Core Identity持久会话类型与同事务端口契约，
无SQL/proto/crypto/CMake/旧Admin/delegation修改。

新SessionControlPeer保存subject和稳定CredentialId，SessionSecurity末尾control_peer
optional只表达缺失以供失败关闭；新建必须有效，refresh/revoke不可换。不能把可空当作
生产可省略或任意Control。valid_session_control_peer只验证两个标识非空，不认证身份。
Login必须从实际已认证Control构造，PG需同事务保存并在更新时检查不可变。

权威验证始终读该CredentialId当前head及原Control principal gate，核对签名actor链首项。
证书轮换后，只要同一Control当前身份仍有效，原会话可继续；绝不以旧leaf认证新连接。
缺值、空ID、主体错配、当前证书撤销/过期或主体禁用失败关闭，查询故障传播Failure。
父需协调Login/PG/Exchange接入；类型存在不表示这些路径已完成或可启用。

精确冻结5文件：

- core/include/kiarabot/core/domain/target/identity_types.hpp
- core/include/kiarabot/core/ports/target/identity_repository.hpp
- core/tests/architecture/session_control_peer_contract_test.cpp
- doc/02-core.md
- reports/modules/architecture-p04-session-control-peer.md

本批doc02只有源会话绑定章节，Revoke章节暂存于工作树外，未混入这次文档差异。
其余未提交Revoke文件不属于本清单。文档影响：doc02与端口注释需要同步且已同步；
线协议不传递此服务端状态，proto/API/运行语义/其他模块无字段变化，doc18只读。
验收为严格C++20纯类型测试、固定格式、边界/inner/diff；PG、Login和RPC另行验收。

实际运行：`c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Icore/include`编译上述
contract test并执行通过；固定clang-format16 dry-run通过；check-boundaries.py、
check-architecture.py --inner与git diff --check通过。未运行PG/Login/RPC，原因同上。

## PostgreSQL capability

目标 Identity PG，基线 root dc37210（a0b7f90 + bootstrap 两cap）；878dfc1 独立类型
cherry 为本分支 a70e891，端口注释和 doc02 追加冲突只合并已有说明，未移除 root 内容。
本步允许 postgres/src/identity_sessions.cpp、Identity schema/version/manifest、三个 PG
fixture、doc05/08 与本报告。没有修改 Login/Core 应用、CMake、旧 Revoke、assignment 或
收据 variant/index；父任务负责生产 Login 写入和 Core strict fixtures。

输入 SessionSecurity 必须含合法 control_peer；新建仅接受 owner 匹配的当前登记
PlatformService/Control 证书材料，缺值/空值/NUL/错误身份为 InvalidArgument。读取返回
同一 subject + stable CredentialId，不固化 credential version 或 leaf。更新 created_at 和
Control binding 均不可变，更换非空绑定为 Conflict；missing 旧字段读取/更新为 Internal，
不补默认值。任何失败沿原同物理 UoW 回滚；数据库故障映射没有变化。
认证、证书期限、主体 gate、授权仍由 Core 决定，存储不以写入成功替代当前权限检查。

Identity 空库 schema7：session_security control_subject/control_credential NOT NULL、非空
CHECK，复合 owner FK 到 credential_heads(id,subject)。稳定 ID 允许 head 证书版本轮换。
PG16.4 owned rollback descriptor 探测为 4ba90cba5394f41fe1d474c3607dfdd9，104 relations；
Runtime56/Scheduler55/Model50 的版本4与 descriptor 不变。严格 schema_valid 未放宽，旧库
拒绝，不自动迁移、补值、清空或 reset。

PG fixtures 创建实际 PlatformService/Control principal 和 certificate rows，覆盖新建/查询
及 by_access/by_refresh 保留绑定、缺值/空值/NUL、换 subject/credential 拒绝、错误 owner/
非 Control 登记拒绝、同稳定 ID 证书轮换后的 refresh、revoke 保留绑定。原普通 session
snapshot、count、CAS/双写回滚、bootstrap current 等测试继续运行。SQLSTATE23502/23514/
23503 验证新字段 NOT NULL/CHECK/复合 FK。缺值故障仅在 owned 临时集群用 migration role
刻意去掉 NOT NULL 并提交 NULL，已打开真实 writer 分别以 find 和 save 返回 Internal；
随后恢复值/NOT NULL。没有通过生产 open/validator 接受损坏 schema。

文档影响：本cap同步 doc05（PG行为）、doc08（当前schema/旧库拒绝）、本报告；doc02与
类型已由878dfc1冻结。doc01/proto/API/运行语义无字段、错误码、wire/HTTP/投递改变，
无需更新；其他模块职责与配置不变，doc18只读未改。测试不写生产TLS或本机配置。

### Verification

工具根 `/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain`，固定 PG16.4、
pqxx7.10、clang-format16、clang-tidy22。以下命令均在独立 infrastructure worktree 执行：

- `sdk/python/.venv/bin/python build/p03/verify_build.py`：C++20 Wall/Wextra/Wpedantic/
  Werror 三目标 initializer、database_test、database_test_sanitized 构建通过。
- 同一 Python：`infrastructure/postgres/tests/run_database_test.py --pg-bin
  <工具根>/build/pg-16.4-install/bin --openssl <工具根>/build/openssl-3.5.0-install/bin/openssl
  --test build/p03/database_test --initializer build/p03/kiarabot-pg-initialize`，另以
  database_test_sanitized 替换 --test。真实 TLS verify-full/channel binding 四库测试结果
  在下方最终记录，不把静态编译当作运行通过。
- 四个改动 cpp TU：clang-tidy22 `--checks=clang-analyzer-*,readability-braces-around-statements
  --warnings-as-errors=*` 配合固定依赖和 macOS SDK，零诊断通过。
- 七个改动 C++/schema：clang-format16 `--dry-run --Werror` 通过；git diff --check 通过。
- `c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Icore/include
  core/tests/architecture/session_control_peer_contract_test.cpp` 编译执行通过。
- 未运行 root CMake/CTest、全App/crypto/RPC fixture：Login 实际构造及其他 strictfixture
  由父并行实现，当前未接入该父cap；PG不修改 Core 应用以伪造默认绑定或启用新 Revoke。
  父需成组运行最终门禁。本cap不声称整个 P04 完成。

最终运行结果：normal 与 ASan+UBSan 两个 owned PG16.4 四库 fixture 完整通过，分别确认
临时集群与私密材料已清除。UTF8/末尾换行/受保护文档无diff检查通过；最终 diff --check
通过。所有新增绑定正向/拒绝/损坏路径均为真实 PG，不使用假成功或内存库替代。

## Stored binding corruption follow-up

父review发现旧字段空字符串会先走与incoming不同的CAS Conflict，未与已冻结的损坏
Internal一致。独立fix在任何CAS比较前显式验证已存subject/credential非NULL且valid_id；
不补值、不修复旧行，也不修改schema/version或类型。本步仅改PG session实现、真实PG测试
与本报告。doc05/08已明确损坏Internal，因实现对齐既有契约无需再改；doc01/02/proto/API/
其他模块与doc18无变更。新测试在owned集群migration角色暂时移除两个字段CHECK/owner FK
及subject NOT NULL，分别注入NULL subject、空subject、空credential；每种以独立真实UoW
验证find和save均Internal并回滚，随后恢复原值与catalog原始约束名/定义。严格生产validator
不放宽，最终四库检查仍验证恢复后的descriptor。构建、测试命令与工具版本同上。

Fix最终检查：三目标 -Werror 构建、normal+ASan/UBSan真实四库完整运行全部通过；两TU
clang-tidy22 analyzer+braces零诊断、format16 dry-run、UTF8/换行/protected-doc/diff通过。
未重复root CMake/CTest及App/RPC：仍由父成组验证，原因同上；无schema/外部契约新增。
