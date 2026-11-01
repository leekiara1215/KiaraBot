# P02 RPC helper 进程生命周期

统一固定环境入口首次在 p02_core_process 的 host helper 退出时捕获 SIGSEGV；macOS
崩溃栈为 EVP_RAND_CTX_free → rand_delete_thread_state → init_thread_stop → pthread
TSD 清理。此前完整51项CTest通过，故这是间歇失败，不能只重跑后忽略。

仅修改 core/tests/platform/service_rpc_client.cpp：显式进程级 grpc_init/grpc_shutdown
RAII，先持有初始化引用，最后在 main 线程且全部channel/context/stub销毁后释放。
固定gRPC源码/公开grpc.h说明最后shutdown可能从worker启动分离清理；本修正使helper
有明确应用生命周期，不用废弃blocking API、不修改TLS验证或库锁。

实际检查：固定依赖增量构建helper通过；format16与diff检查通过；tidy22针对该TU
（相同compile DB、SDK、全部clang-analyzer、warnings-as-errors）通过；真实host mTLS
正反例交替60次独立helper启动/退出均通过；相同压力下原helper60次也未重现，不将
本轮无复现声称为穷尽证明。p02_core_process完整重跑通过（9.76秒）。

目标模块是Core测试辅助进程，输入输出/错误码/生产配置和行为不变。无公共文档影响；
无需更新proto/API/doc02/08，doc18只读。未重跑全套：P03新增TU等待CMake线程接线，
后续统一门禁再验证，不用当前局部通过宣布主线或P02完整通过。
