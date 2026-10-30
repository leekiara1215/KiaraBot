# 本机闭环验收

保持 C++ Core、Python SDK 与主仓库规划的 NoneBot 适配器结构（适配器当前暂存于同级检出，待迁入 `adapters/`）。先按 sdk/python/README.md 创建锁定环境，构建 proto + infrastructure，然后执行 `bash tests/acceptance/run.sh`。入口不跳过缺少的 Core 或 Python 环境，使用临时 SQLite、本机随机端口和测试凭据，不需要真实账号或模型。

默认执行 Core CTest、Ruby 契约一致性检查（需要 ruby）与全部 SDK 测试。test_closed_loop.py 通过真实 gRPC/SQLite/SDK 覆盖事件到 Action 结果、权限拒绝无平台调用、重复事件、结果丢失后的断线和 Core 重启；测试平台保留执行日志，断线重放同一 action_id 不重复执行。test_model.py 覆盖本机 HTTP Provider，test_tasks.py 覆盖任务取消与重启投递。这是协议 fake 平台验收，不代表真实 QQ/NapCat 联调。

可选设置 KIARABOT_ADAPTER_DIR 为适配器检出（当前为同级 `../kiarabot-adapter-nonebot/`，迁入后为仓库内 `adapters/nonebot/`）的绝对路径，同时运行它的全部测试（包含真实 NoneBot 子进程和 C++/SDK/适配器闭环）。迁移前该目录及其虚拟环境不复制进本仓库；设置后环境缺失即失败。CORE_TEST_BINARY 由入口设置，也可通过 KIARABOT_ACCEPTANCE_BUILD 指定构建目录，KIARABOT_ACCEPTANCE_PYTHON 指定 SDK Python。CI 在 pinned vcpkg 构建中运行默认入口；适配器测试由对应检出提供，远端 CI 是否执行不能由本地通过推定。

T08b–T11a 增加控制面环境：先执行 `uv sync --project control/api/tests --locked --python 3.11.18`。统一入口必须运行此环境的全部测试（mock/真实端口同套 OpenAPI 响应校验、HTTP 会话/权限/审计/分页、真实 WebSocket、日志脱敏、健康探针及迁移锁）；环境缺失直接失败。也可单独运行 `control/api/tests/.venv/bin/python -m pytest -q control/api/tests`。完成 Core 构建后，`cd panel && npm test` 同时运行 UI mock 和 TypeScript→真实 C++ 服务集成。

配置下发验收已纳入 control/api/tests/test_config_delivery.py：真实 HTTP → C++ Core/SQLite → Python SDK → ACK → HTTP delivery 查询，覆盖离线保存、热更新、幂等重放、回调拒绝/超时、Host RPC、停用、SDK/Core 重启及恢复。执行前生成 SDK 协议代码（run.sh 自动执行）；不依赖 QQ、模型服务或 Docker。SDK 单测还覆盖回调串行、快照副本、重复帧和能力协商；Core 单测覆盖旧连接/迟到 ACK、合并版本、事件门控与存储读取失败。
