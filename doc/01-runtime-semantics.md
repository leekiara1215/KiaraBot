# 事件与 Action 运行时语义

字段定义在 `proto/kiarabot/v1/`，本文件冻结传递、重试、顺序和确认规则。

## 当前 reference runtime

`core/include/kiarabot/core/runtime.h` 提供与本语义对应的可替换状态机。`RuntimeStore` 是持久化端口，保存 inbox、Action 和插件状态；`InMemoryRuntimeStore` 只用于测试。Core 运行时不直接访问数据库，基础设施实现该端口后，Core 重启仍可恢复去重、重试、死信和 Action 幂等状态。

每个运行时操作都必须通过未来的日志端口记录 `operation`、`outcome` 以及相关事件、Action 或插件标识。日志不会替代状态存储，也不会承载业务命令。日志实现可以评估腾讯 [BqLog](https://github.com/Tencent/BqLog)，但当前 Core 不直接依赖它。

## 事件投递

- Core 与适配器使用至少一次投递；
- 适配器只有在 Core 持久化 inbox 记录后才认为事件已确认；
- 事件可能重复，插件必须用 `event_id` 幂等；
- Core 默认保留已确认事件和幂等键 7 天，配置不得低于 24 小时；
- 过期事件不自动重放，人工重放生成新的 `trace_id`。

## 顺序和背压

- 不提供全局顺序；
- 同一 `(adapter_id, account_id, conversation_id)` 内按 `sequence` 单调递增；
- 不同会话可以并行；
- 缺少 sequence 或跳号只影响该会话，并报告 `DEGRADED`；
- 插件声明 `ordered=true` 时，Core 对该插件按会话串行投递；
- 队列达到上限时暂停读取非关键事件并返回 `RATE_LIMITED`。

## 确认和重试

```text
RECEIVED → PERSISTED → DISPATCHED → ACKED
                         └────────→ RETRYING → DEAD_LETTER
```

- 插件 ACK 表示接受事件，不表示 Action 成功；
- 默认最多重试 5 次，延迟 1s、2s、4s、8s、16s，并加入 20% 抖动；
- 非重试错误直接进入死信；
- 死信可由控制面板人工重放；
- 同一事件不会向同一插件并发投递。

## Action 语义

- Action 必须带 `idempotency_key`；
- Core 保存 Action 结果 7 天，重复请求返回原结果；
- 适配器可先返回 `ACCEPTED`，最终返回 `SUCCEEDED` 或 `FAILED`；
- 到达 `deadline` 后标记 `EXPIRED`，适配器不得再执行；
- 网络超时不等于平台执行失败，必须通过幂等键查询最终结果。
- 查询接口返回 `ACCEPTED` 表示尚未终结；`SUCCEEDED`、`FAILED`、`EXPIRED`、`CANCELLED` 是终结状态。终结结果保留 7 天；查询超时不改变 Action 状态。按幂等键查询受已认证调用方的授权范围限制。

## 插件重连

- 插件断开后进入 `UNAVAILABLE`，Core 暂停投递并保留未确认事件；
- 重新注册且 API 版本和权限一致后恢复；
- 超过 5 分钟未恢复则为 `FAILED`，由策略决定是否重启。

## 控制面板流恢复

控制流使用 AsyncAPI 中的 `SESSION_READY` 会话 ID 和已处理的 `sequence` 恢复；服务端保留 10 分钟事件。服务端返回 `RESYNC_REQUIRED` 时，客户端丢弃旧会话，通过 `hello` 建立新会话并重新拉取 REST 快照。事件可能与快照重叠，客户端按事件触发刷新，不能将流视为业务状态的唯一来源。

## 必须测试

重复、乱序、sequence 跳号、Core 重启后的持久化去重、插件重连、Action 超时、适配器断线、队列满、重复幂等键、指数退避、死信、保留期清理和结构化日志路径。

## 代码启动句

> 请只实现事件 inbox、event_id 去重、会话顺序、指数退避重试、死信、Action 幂等与插件重连状态机，使用 fake adapter/plugin 编写可重复的时间控制测试，不接入真实 QQ、数据库或模型。
