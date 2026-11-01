# Local reply：单机固定回复

现有 Python SDK 处理器，非新插件机制。收到群/私聊纯文本后回复 `收到：<输入>`，输出最多
4096 UTF-8 字节（含前缀），不调用模型、Provider、存储或外部网络。自身消息、空文本、
非消息和已超过原事件时间 60 秒的消息忽略。CQ 字样只放入 plain_text，不生成 CQ/媒体段。

- manifest：`plugins/local-reply/plugin.json`
- id：`local-reply`；entrypoint：`main:create`；`ordered=true`
- 仅需现有 `message.read`、`message.send` 显式授权和 `action.result.v1` 协商。
- 相同来源/event ID 使用固定 UUID5 Action ID/幂等键；先 query，已有 accepted/terminal
  不再 submit；仅 NOT_FOUND 创建，其他依赖错误上抛给 SDK。未知提交不在 hook 内重试。
- Action deadline 始终是原 `occurred_at + 60s`（保留纳秒），查询后再次检查，不重放续期。

由原 reference Core managed process 启动，不另起自建服务。原 launcher 需设置：

```text
KIARABOT_PLUGIN_ID=local-reply
KIARABOT_PLUGIN_PERMISSIONS=message.read,message.send
KIARABOT_PLUGIN_EXECUTABLE=/absolute/path/to/sdk/python/.venv/bin/python
KIARABOT_PLUGIN_MANIFEST=/absolute/path/to/plugins/local-reply/plugin.json
KIARABOT_PLUGIN_PYTHONPATH=/absolute/path/to/sdk/python/src:/absolute/path/to/sdk/python/build/generated:/absolute/path/to/plugins/local-reply
```

沿用 launcher 的插件 token、桥接监听、持久化、账号和会话白名单配置，不在 manifest/源码中
保存 token。Core 使用 `python -m kiarabot_sdk --manifest ...` 并提供原有
`PLUGIN_CORE_ENDPOINT`/`PLUGIN_AUTH_TOKEN`，无需新增插件环境变量。这里不修改 launcher。

已准备 SDK 依赖和生成协议后，在仓库根执行：

```sh
PYTHONPATH="$PWD/sdk/python/src:$PWD/sdk/python/build/generated" \
  sdk/python/.venv/bin/python -m pytest -q -c sdk/python/pyproject.toml plugins/local-reply/tests
```

这是本插件协议 fixture 测试。真实 reference Core/SDK/适配器闭环由单机 MVP 验收脚本合验；
本插件未进行真实 QQ、付费模型或外部部署调用。
