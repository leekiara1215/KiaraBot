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
