# P02 Control 独立应用与组合根

目标模块：Control；允许文件为 control/api 新 application/target、app/main.cpp、
tests/architecture/application_test.cpp、doc/06 与本报告。公共 readiness/process 端口
由 Core 任务拥有并先独立提交；CMake 接线由工具链任务拥有。

输入：不可伪造 ready 的进程本地 application 及公共 runner CLI/config。
输出：独立 Control 身份、就绪依赖快照、运行退出码。P02 所有业务仍未实现；
application 不接数据库，业务/Identity/凭据门禁不允许就绪，main 捕获异常并输出
脱敏 INTERNAL。未实现业务、配置、证书和端口错误以共享 runner 契约为准。

文档影响：同步 doc/06 和本报告；不改变外部 schema/权限/错误映射，不修改 proto/API
或实现状态登记。Core/运维由公共 runner 所有者同步，工具链/边界/进度由对应所有者
同步；其他业务模块无新行为无需更新。doc/18 只读。

验收：application 仅标准库/纯端口依赖，服务身份正确，业务与身份凭据门禁拒绝，
不持有权威数据库；修改返回快照不影响应用。独立单测和 C++ 格式检查、六服务 full
构建及 P02 进程验收分别记录；不得以单元测试替代 REST/WS/认证真实实现。

共享端口 `18c73bc` 已集成为 `f8b7290`。实际验证：使用 AppleClang/C++20，
`-Wall -Wextra -Wpedantic -Werror`，同时包含 core/include 与 control/api/include，
编译 application.cpp 和 application_test.cpp 为临时独立测试并运行通过；main.cpp
单独编译为临时 object 通过。四个手写 C++ 文件 clang-format 检查及 diff 检查通过。
独立测试不链接 Protobuf、JSON、数据库、TLS 或旧 Core；未运行进程和 HTTP/WS 验收，
因为公共 runner 实现与根构建接线尚在其他任务开发，当前 main 只完成声明级编译。
完整工具链前置已知缺 clang-tidy，统一环境由工具链任务收尾，不在这里记为通过。
