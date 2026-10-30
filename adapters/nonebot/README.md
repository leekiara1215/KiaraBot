# KiaraBot NoneBot2 适配器

T02b 的接收链路：NoneBot2 / OneBot V11 → 统一 EventEnvelope → 本地有界 outbox → Core AdapterBridge。Python 3.11–3.12；依赖以 uv.lock 为准。代码不访问 Core 数据库，不包含业务插件。T04b 已接线纯文本 send_message，声明 action.send_message.text.v1；其他 Action/消息段显式拒绝。

## 初始化与验收

```sh
uv sync --locked --python 3.11
uv run python tools/generate_proto.py ../../proto
uv run pytest
PYTHONPATH=generated:src uv run python -m kiarabot_adapter_nonebot
```

协议唯一来源是 本仓库 proto/；generated/ 为本机生成物，不提交。修改协议后重新生成。环境配置：CORE_GRPC_ENDPOINT（仅本机 127.0.0.1:端口）、CORE_AUTH_TOKEN、ADAPTER_ID、ACCOUNT_ID、ONEBOT_WS_URL（NapCat 正向 WebSocket URL）、ONEBOT_ACCESS_TOKEN、OUTBOX_PATH（本地 outbox 文件路径）。Core 预先授权对应账号且启用 T02a 持久化入口。正向 WebSocket 由 NoneBot websockets driver 建立，适配器名称及配置遵循 [NoneBot OneBot 文档](https://onebot.adapters.nonebot.dev/docs/guide/setup/)。不会启动生产账号测试。

## 输入、输出与失败

只接收 OneBot V11 群聊/私聊 message，稳定 event_id 由适配器/账号/会话/message_id/时间派生；会话 sequence 在本地 SQLite 入队时原子递增。消息段支持 text/image/at/reply；其它段转换为 UNKNOWN 且不携带原始 OneBot 对象。原始 JSON 不越过协议边界。缺少字段或非法数据返回 INVALID_ARGUMENT，账号不符 PERMISSION_DENIED，未支持事件 CAPABILITY_NOT_SUPPORTED，容量不足 RATE_LIMITED，本地存储失败 UPSTREAM_UNAVAILABLE。

outbox 使用自己的数据库保存待确认统一事件、顺序水位和失败码。PERSISTED/DUPLICATE 才将事件标为 acked；确认丢失、超时、断线或 retryable 拒绝保留 pending。永久拒绝标为 failed，保留稳定码供排查，不当作成功。重连重新注册、重绑 instance，保留其余事件字段。相同 ID 不同内容 CONFLICT。单个事件最多 1 MB，最多 10000 条记录；acked 7 天后可清理，sequence 不回退。达到上限会明确拒绝入队；OneBot 本身不提供可靠消费 ACK，平台到本地 outbox 的投递保证需在真实联调确认，不能宣称端到端零丢失。

Core 连接采用 1–30 秒退避。每次连接只发送一个待确认事件，空闲发心跳；响应不匹配时保留待发项并重连。日志只输出稳定错误码，不输出载荷、token、URL 或上游异常正文。关闭取消连接任务并关闭本地存储。

## 验收范围

测试覆盖群/私聊映射、段降级、稳定 ID、账号拒绝、SQLite 重开/序号/容量、fake gRPC 注册能力、断线重投、确认前保留及永久拒绝。Action 测试包含执行前 deadline、账号/实例校验、重复/冲突、重启去重、平台拒绝、超时/中断未知结果和取消不重连；真实 NoneBot 子进程与 fake OneBot 验证实际发送调用。真实 NapCat/QQ 尚未验证，留待 T07b。

完整跨语言验收使用 `CORE_TEST_BINARY=../../build/proto/core/kiarabot-core .venv/bin/python -m pytest -q`；Action 闭环还读取本仓库的 sdk/python/src，可用 SDK_SOURCE 覆盖。测试连接真实 C++ Core、Python SDK/适配器与 fake 平台，重启 Core 后按幂等键复用终态。

Action 执行日志位于 OUTBOX_PATH 加 .actions，独立 SQLite 文件，执行前落盘占用；每个 action_id 只调用一次平台，重投返回已知结果。同 ID 不同内容返回 CONFLICT。群/私聊文本使用 OneBot text segment，CQ 字符串不会解释为代码。超时/断线/进程崩溃可能发生在副作用之后，此时保留 ACCEPTED/未知，不重新调用平台；Core 到 deadline 后标记 EXPIRED，不能撤销已发生的副作用。最多一万条记录、64 MiB，终态保留七天，未终态不自动清理。备份需同时保存事件 outbox 与 .actions；这不是平台不支持的 exactly-once 保证。
