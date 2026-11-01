# 旧行为参考闭环验收

本入口只运行隔离的旧Core/Control/SDK/Adapter行为断言，属于reference闭环，**不是六服务业务验收**。
新架构P02进程/证书/探针/故障隔离入口为[`tests/architecture/run.py`](../architecture/run.py)。
reference通过不表示新Identity/Runtime/Host/Scheduler/Model业务已实现，也不关闭P03及后续阶段。

`KIARABOT_ACCEPTANCE_BUILD`始终输入**构建根**（默认build/proto），不要直接设成reference子目录：

| 使用方 | 路径/行为 |
| --- | --- |
| SDK及已启用Adapter的CORE_TEST_BINARY | 构建根/reference/core/kiarabot-core；不回退旧core位置 |
| Control pytest的KIARABOT_ACCEPTANCE_BUILD | 仅对子进程设为构建根/reference，查找其中core和control/api夹具 |
| CTest | 从构建根运行，限制reference标签，保留--no-tests=error |
| Ruby CONTROL_POLICY_BINARY | 构建根/kiarabot-control-application-test |

调用示例：`KIARABOT_ACCEPTANCE_BUILD="$BUILD_ROOT" bash tests/acceptance/run.sh`。
可执行路径或锁定Python环境缺失即失败；旧位置残留二进制不会替代reference构建。


保持 C++ Core、Python SDK 与仓库内 `adapters/nonebot/` NoneBot 适配器。先按 sdk/python/README.md 创建锁定环境，构建包含reference目标的proto + infrastructure，然后执行 `bash tests/acceptance/run.sh`。入口不跳过缺少的 Core 或 Python 环境，使用临时 SQLite、本机随机端口和测试凭据，不需要真实账号或模型。

默认执行reference标签的Core CTest、Ruby 契约一致性检查（需要 ruby）与全部 SDK 测试。test_closed_loop.py 通过真实 gRPC/SQLite/SDK 覆盖事件到 Action 结果、权限拒绝无平台调用、重复事件、结果丢失后的断线和 Core 重启；测试平台保留执行日志，断线重放同一 action_id 不重复执行。test_model.py 覆盖本机 HTTP Provider，test_tasks.py 覆盖任务取消与重启投递。这是协议 fake 平台验收，不代表真实 QQ/NapCat 联调。

可选设置 KIARABOT_ADAPTER_DIR 为适配器目录 `adapters/nonebot/`的绝对路径，同时运行它的全部测试（包含真实 NoneBot 子进程和 C++/SDK/适配器闭环）。适配器的虚拟环境和生成物不入库；设置后环境缺失即失败，并由入口重新生成适配器协议代码。CORE_TEST_BINARY 由入口设置，也可通过 KIARABOT_ACCEPTANCE_BUILD 指定构建根目录（不含reference后缀），KIARABOT_ACCEPTANCE_PYTHON 指定 SDK Python。CI 在 pinned vcpkg 构建中运行默认入口；适配器测试由该目录提供，远端 CI 是否执行不能由本地通过推定。

T08b–T11a 增加控制面环境：先执行 `uv sync --project control/api/tests --locked --python 3.11.18`。统一入口必须运行此环境的全部测试（mock/真实端口同套 OpenAPI 响应校验、HTTP 会话/权限/审计/分页、真实 WebSocket、日志脱敏、健康探针及迁移锁）；环境缺失直接失败。也可单独运行 `control/api/tests/.venv/bin/python -m pytest -q control/api/tests`。完成 Core 构建后，`cd panel && npm test` 同时运行 UI mock 和 TypeScript→真实 C++ 服务集成。

配置下发验收已纳入 control/api/tests/test_config_delivery.py：真实 HTTP → C++ Core/SQLite → Python SDK → ACK → HTTP delivery 查询，覆盖离线保存、热更新、幂等重放、回调拒绝/超时、Host RPC、停用、SDK/Core 重启及恢复。执行前生成 SDK 协议代码（run.sh 自动执行）；不依赖 QQ、模型服务或 Docker。SDK 单测还覆盖回调串行、快照副本、重复帧和能力协商；Core 单测覆盖旧连接/迟到 ACK、合并版本、事件门控与存储读取失败。


wrapper路由自测：`python3 tests/architecture/acceptance_wrapper_test.py -v`。
它仅在tmp复制shell入口并记录临时命令夹具的argv/env/cwd，验证上述路径、带空格路径、
既有Adapter显式启用方式、Python覆盖、缺依赖失败和每阶段非零传播；不执行真实SDK/Control/
Adapter业务测试，不得把此自测标成旧闭环或六服务验收通过。所有既有真实测试调用均保留。
