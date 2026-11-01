# P04 外层 RPC 装配

目标为Core runtime/platform外层生命周期支撑。允许修改service_rpc、service_process
头/实现、既有composition driver/Python测试和RPC client；不修改内层业务、PG、协议、
API或doc18。输入为受审C++组合根回调和本owner适配器，输出为受现有mTLS保护的RPC表；
错误为本地InvalidArgument/NotAccepted/CorrectInput或组合失败退出1，异常正文脱敏。

文档影响判断：外层公共装配与生命周期变化同步doc02/08；无新增业务方法、字段、
HTTP/WS、业务状态或错误码，proto/API/doc01无需更新；doc18只读。本变更不将任何
方法登记为implemented，也不因handler存在把readiness设true。

ServiceRpc构造时固定owner，add_service在start之前转移适配器所有权，拒绝空值/错
owner/启动后登记。run_service先验证依赖、执行RPC组合，再创建探针/业务监听。
没有自定义服务时继续注册现有generated拒绝表；自定义服务未覆盖方法默认拒绝。
服务退出先Shutdown/Wait，适配器析构先于application与依赖；原无回调构造兼容。

实际验证使用固定full-locked前缀、C++20与-Werror，完整构建六个入口及两个测试driver
通过。扩展p03_service_composition实际7.89秒通过：原CLI/材料/依赖失效/信号与析构
测试保留，新增空RPC/错owner/异常监听前拒绝；自定义Identity Login handler固定拒绝，
真实mTLS客户端必须收到PERMISSION_DENIED，UNIMPLEMENTED不能通过该断言；无客户端
证书仍拒绝。退出验证RPC适配器析构时application仍存活且先于其析构。它是生命周期
fixture，不是假认证实现。p02_core_process原五owner/桥接/证书/重启测试20.12秒通过。

四个改动TU同compile DB使用tidy22 clang-analyzer-*与warnings-as-errors通过，六个C++
文件format16、Python语法、边界/inner和diff通过。CMake临时仅加driver的proto依赖和
既有CTest第二个client参数用于验证，提交前恢复，最终由构建线程独立接线。
完整统一门禁由主集成负责；本项不替代真实Identity/Control认证、安全仓储或跨主机验收。
