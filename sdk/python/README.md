# Python Plugin SDK

当前支持进程外事件插件，保持 C++ Core / Python SDK 的边界。协议从仓库 `proto/` 生成，禁止手工维护消息镜像。Python 3.11.18 与 uv.lock 固定依赖。

在仓库根目录准备环境并验收：

```bash
uv sync --project sdk/python --python 3.11.18 --locked
sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py
cmake --preset proto -DKIARABOT_ENABLE_INFRASTRUCTURE=ON
cmake --build --preset proto
sdk/python/.venv/bin/python -m pytest -q sdk/python/tests
```

测试默认使用 `build/proto/core/kiarabot-core`；可用 `CORE_TEST_BINARY` 指定另一构建。缺少二进制会失败，不跳过真实进程验收。测试使用临时数据库、本机随机端口与测试凭据，不连接真实 QQ。生成目录、虚拟环境均不提交。

插件继承 `kiarabot_sdk.Plugin`，提供异步 `on_start(context)`、`on_event(event, context)` 和 `on_stop(reason)`。event 为生成的 EventEnvelope；成功返回后发送正 ACK，异常转为脱敏 INTERNAL，协作超时转为 TIMEOUT。ACK 不表示 Action 成功；至少一次投递可能重复，插件需要使自身副作用幂等。

清单示例见 [event-observer](../../plugins/event-observer/plugin.json)。开发时将 `sdk/python/src`、`sdk/python/build/generated` 与插件目录的绝对路径加入 `PYTHONPATH`，配置 `PLUGIN_CORE_ENDPOINT=127.0.0.1:端口` 和 `PLUGIN_AUTH_TOKEN` 后执行：

```bash
sdk/python/.venv/bin/python -m kiarabot_sdk --manifest /absolute/path/to/plugin.json
```

由 Core 管理时使用 `KIARABOT_PLUGIN_EXECUTABLE`、`KIARABOT_PLUGIN_MANIFEST` 和 `KIARABOT_PLUGIN_PYTHONPATH`，并配置持久化桥接、插件身份/凭据/权限，详见 [运维说明](../../doc/08-operations.md)。宿主只传递插件所需的环境变量；插件日志目前丢弃，后续统一日志任务再接入。进程隔离不是操作系统安全沙箱，仅用于可信插件的生命周期和故障隔离。

Context.granted_permissions 在注册协商完成后可用，断开后清空。重连重新注册并核对全部权限；SDK 不缓存授权决定。已提供 submit_action/query_action/cancel_action（action.result.v1）以及 storage_get/storage_set/storage_delete（host.storage.v1）；必须协商后使用。storage_set/delete 的幂等键由插件提供，详情见 [SDK 设计](../../doc/03-plugin-sdk.md)。模型客户端 model_chat(model, messages, timeout=5.0) 已提供，需 host.model.v1 和 model.chat；返回 ModelChatResponse，超时/断线不可自动重试。调度客户端 task_create/task_get/task_cancel 已提供，需 host.tasks.v1、task.schedule；创建还需 event.timer.triggered，支持 UTC datetime 和 local_time 字典。AI 示例见 ../../plugins/ai-reply/plugin.json，插件使用 default 别名，由 Core 映射到 KIARABOT_MODEL_NAME；示例只通过 SDK，不读取密钥。

FakeHost 使用真实 gRPC 和生成类型，测试可向 outgoing 队列放入 CoreToPlugin 帧，从 acknowledgements 队列读取 ACK；放入 None 模拟连接断开。Action 请求进入 actions 队列，Host 请求进入 host_requests 队列；测试显式设置 features 并写入对应响应。它不模拟 Core 的数据库、权限策略或业务状态机。
