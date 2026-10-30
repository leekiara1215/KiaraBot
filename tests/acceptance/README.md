# 本机闭环验收

保持 C++ Core、Python SDK 与独立 NoneBot 适配器结构。先按 sdk/python/README.md 创建锁定环境，构建 proto + infrastructure，然后执行 `bash tests/acceptance/run.sh`。入口不跳过缺少的 Core 或 Python 环境，使用临时 SQLite、本机随机端口和测试凭据，不需要真实账号或模型。

默认执行 Core CTest、Ruby 契约一致性检查（需要 ruby）与全部 SDK 测试。test_closed_loop.py 通过真实 gRPC/SQLite/SDK 覆盖事件到 Action 结果、权限拒绝无平台调用、重复事件、结果丢失后的断线和 Core 重启；测试平台保留执行日志，断线重放同一 action_id 不重复执行。test_model.py 覆盖本机 HTTP Provider，test_tasks.py 覆盖任务取消与重启投递。这是协议 fake 平台验收，不代表真实 QQ/NapCat 联调。

可选设置 KIARABOT_ADAPTER_DIR 为独立适配器 checkout 的绝对路径，同时运行它的全部测试（包含真实 NoneBot 子进程和 C++/SDK/适配器闭环）。该目录及其虚拟环境不复制进 Core 仓库；设置后环境缺失即失败。CORE_TEST_BINARY 由入口设置，也可通过 KIARABOT_ACCEPTANCE_BUILD 指定构建目录，KIARABOT_ACCEPTANCE_PYTHON 指定 SDK Python。CI 在 pinned vcpkg 构建中运行默认入口；独立适配器测试由其自身 checkout 提供，远端 CI 是否执行不能由本地通过推定。
