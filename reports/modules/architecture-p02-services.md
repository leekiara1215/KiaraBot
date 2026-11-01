# P02 五服务与公共进程支撑

基点 d20b54b，分支 codex/p02-services，限定新 Core target/platform 源、五个入口、
新测试、doc/02、doc/08；不改 CMake、Control 内部、proto/API、旧源码或保护基线。
文档影响：readiness/进程接口与新运行方式同步 doc/02、08 和本报告；
线协议沿 P01，不新增协议/API 字段。doc/19–23 只读依照，不改协调记录。

## 能力一：公共接口

ports/target/service_readiness.hpp 是纯 C++20 端口，Service 沿用既有六服务枚举。
结果含依赖状态与 ready；P02 应用必须含 BusinessImplementation/NotImplemented，
且 ready=false。端口线程安全、查询不执行 I/O，与 live/listen 分开。
runtime/platform/service_process.hpp 声明 kiarabot::core::platform::run_service，
借用端口至全部线程/socket/RPC 停止；CLI/退出码遵守协调记录。
后续五工厂统一签名 std::unique_ptr<target::ServiceReadinessPort> make_<name>_application()，
命名空间 kiarabot::core::target；头/源为 application/target/<name>_application.hpp/.cpp，
name 为 identity/runtime/host/scheduler/model。Control 可独立实现相同端口。

能力一验证：临时独立 C++20 -Wall/-Wextra/-Wpedantic/-Werror 头编译及执行、
clang-format --dry-run --Werror、git diff --check 均通过；未运行进程测试，runner 仅声明。

## 能力二：五个独立 application 与入口

新增 application/target 的 identity/runtime/host/scheduler/model 工厂和五个 *_main.cpp；
无共享可变全局/旧快照/协议依赖。依赖均 Unverified，BusinessImplementation 保持
NotImplemented，任何配置不能令它 ready。主 CMake 接线由构建任务拥有，本轮临时
编译五源与 service_applications_test.cpp 验证服务身份和不可绕过的未实现 gate。
文档更新 doc/02 与本报告，运维行为未超出能力一约定；proto/API 无变更。

能力二实际检查：五个 application 源和测试以 C++20 -Wall/-Wextra/-Wpedantic/-Werror
独立编译并执行通过；clang-format、边界检查、git diff --check 通过。入口链接待 runner。
