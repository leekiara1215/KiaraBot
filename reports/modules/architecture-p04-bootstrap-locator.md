# P04 bootstrap CURRENT locator

## 范围与文档影响（实施中）

父任务已冻结单项bootstrap当前定位，不涉及并行Credential作者的Pending allowed_services/
Register/Rotate模型。Core端口及严格mock先独立提交，PG实现/schema/真实验证随后独立提交。
目标仅Identity；允许Core端口声明、受影响strict测试、PG适配器与直接doc01/05/08/本报告。
禁止改proto/API、其他模块业务、CMake与doc18；新空库schema更新，不自动升级/清空旧数据。

输入为purpose=WorkloadBootstrap、可信key引用及Sha256Digest固定32tag；输出CURRENT
CredentialVersionRecord或成功空optional。InvalidArgument代表错误shape；SQL/损坏Failure，
不得任选多条匹配或伪不存在。lookup不授权，Core检查真实主体/状态/期限/绑定/audience。
save是current locator唯一写入者；历史同tag合法，不能在历史表全局unique。

本Core声明步同步doc01，仅更改内部typed契约，无wire/API/错误码/配置变化，doc05/08将在
PG行为/schema实现步同步。严格fake的新未用方法直接失败，避免假空成功掩盖缺实现。
本步构建/检查结果待执行后记录；不声称PG已实现或根完整门禁通过。

## Core声明步实际检查

三个纯测试均以C++20 -Wall -Wextra -Wpedantic -Werror -UNDEBUG独立编译运行通过：
identity_contract_test；identity_security_application.cpp+其test；identity_admin_application.cpp+
identity_delegation_application.cpp+admin test。新方法在三种strict fake中未使用时直接失败。

RPC strict fixture先因旧外部generated headers缺少既有wire字段失败；随后以锁定
protoc29.3/grpc_cpp_plugin在build/p04-bootstrap/generated重新生成当前13个proto（未改协议、
未提交生成物），identity_rpc_test.cpp的-Werror fsyntax-only通过。四个变更test TU的
analyzer+braces clang-tidy22、所有变更C++ clang-format16、文档UTF8/换行/diff检查通过。
RPC runtime及根CMake/CTest未运行：本步是接口与mock独立提交，生产PG实现待下一提交，
不能单独集成此纯虚端口后声称全根可构建。父应与后继PG实现成组接入。

## PG实现步与验收

允许变更仅 infrastructure/postgres 的 Identity schema/实现/真实测试、doc05/08 与本报告；
其余三个服务 schema 未变。没有修改 CMake、生产组合根、proto/API、Pending 模型、Session
新字段或受保护 doc18。输入/输出/失败语义沿用上述 Core 冻结接口，不增加稳定错误码。

Identity schema6 head 增加显式 material、bootstrap_key/tag；CHECK 覆盖各材料定位是否必须
存在/必须为空，包含 CURRENT certificate leaf 不能 NULL 跳过外键的覆盖。当前 key/tag 唯一；
head 到 versions/material 与具体 verifier 的复合延迟 FK 确保一个事务内 save 完整写入。
历史表只有 id/version/key/tag 的引用唯一键，不对历史 tag 施加全局唯一。查询不加 LIMIT，
校验定位所得当前版本及实际材料的 key/tag；不在存储层判断授权。save 负责轮换/替换定位，
冲突及损坏均令整个 UoW 失败。没有自动迁移、清库或为旧记录补造定位。

真实 PG16.4 TLS verify-full/SCRAM channel binding 私有临时集群 descriptor probe：
Identity=514a301be56f67b13387007a231c709b /104 relations；Runtime=56、Scheduler=55、
Model=50 且 checksum 全部未变。探测在 owned marker 和 data_directory 校验后执行回滚事务；
未放宽生产 schema_valid，未提交生成物或集群材料。

覆盖：Active 当前查询；Revoked 当前 v2 仍返回而不授权；v1/v2 同 tag 历史合法；v3 换 tag 后
旧 tag 无结果；另一当前 credential 同 key/tag 返回 Conflict 且无部分持久化；错误 purpose/
空或含 NUL key 拒绝；当前 locator 错配、缺失 material 为 Internal 且整事务回滚；更换材料后
bootstrap locator 清除；head 材料、字段覆盖及 FK 的 SQLSTATE 23514/23503 拒绝；证书轮换
既有测试继续覆盖。独立重开 writer 后真正 pg_terminate_backend，再以 by_bootstrap 作为第一条
失败查询验证 UpstreamUnavailable，保留原 session count 的独立故障证明。

### PG实际检查命令与结果

锁定工具根为 /Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain。

- `sdk/python/.venv/bin/python build/p03/verify_build.py`：C++20 -Wall -Wextra -Wpedantic
  -Werror 编译 initializer、database_test、ASan+UBSan database_test_sanitized。
- 同一 Python 执行 `infrastructure/postgres/tests/run_database_test.py --pg-bin
  <工具根>/build/pg-16.4-install/bin --openssl <工具根>/build/openssl-3.5.0-install/bin/openssl
  --test build/p03/database_test --initializer build/p03/kiarabot-pg-initialize`；sanitized
  使用相同命令替换 --test 为 build/p03/database_test_sanitized。结果见最终交付记录。
- clang-tidy22：三个改动 TU 使用 `--checks=clang-analyzer-*,readability-braces-around-statements
  --warnings-as-errors=*`，固定依赖/system SDK；零诊断通过。最终变更的两个 TU 重跑同检查。
- clang-format16 `--dry-run --Werror`：六个改动 C++/schema 文件通过。
- 文档影响：已更新 doc01（Core契约提交）、doc05（PG行为）、doc08（空库版本/操作）、本报告。
  doc02/03/04/06/07、runtime semantics、proto/README、API 无新增行为，未更新；无 wire、HTTP、
  WebSocket、投递、配置或模块职责改变。doc18 只读且未改。文档 UTF8/末尾换行与 git diff
  --check 将在提交前验证；无协议改动，因此不需协议生成检查。
- 未运行根 CMake/CTest、完整 RPC runtime 或真实 App/crypto fixture：此为独立 PG 能力包，
  根组合/工具链由父任务负责，最终需与 Core 2a01768 成组验证；不声称完整 P04 关闭。

最终结果：上述三目标最终 -Werror 构建通过；最终 normal 与 ASan+UBSan 均在独立 owned
PG16.4 四库集群完整通过（TLS/权限/schema/锁/重启及 typed security 检查）。三个 TU tidy
及最终两 TU 重跑均零诊断；六文件 format dry-run 通过。Python UTF8/末尾换行/受保护文档
无 diff 检查、git diff --check 通过。两次 fixture 均确认删除 owned 临时集群和私密材料。

## 主线组合门禁

主线成组接入Core端口/strict fixture与PG schema6；公钥与Exchange新增fixture也补齐
未使用by_bootstrap显式trap。正式注册Exchange纯库/测试与源Control绑定类型测试。
完整full-locked configure/build无warning/error；31/31相关P04及两组真实PG/服务数据库
测试通过（94.07秒），包含CLI、真实两生产daemon、证书到期及在途提交边界。
边界/全部TU覆盖与实际链接图会在提交前复核。接口扩展没有新wire字段，
工具链doc11同步测试入口；受保护doc18不变，不以此关闭剩余P04方法。
