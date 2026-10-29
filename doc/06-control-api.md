# 控制面板 API 设计

## 责任

控制面板 API 是浏览器和未来 C++ 控制客户端的统一入口，负责认证、授权、查询和管理操作。

它不直接连接 NapCat、数据库或插件进程，而是调用 Core 的应用服务。

## 协议

```text
HTTP REST + JSON：配置和管理
WebSocket：实时状态、日志和事件
OpenAPI：api/openapi.yaml
AsyncAPI：api/asyncapi.yaml
```

## 目录

```text
control/api/
  src/
    auth/
    rbac/
    rest/
    websocket/
    health/
    mappers/
  tests/
```

`api/openapi.yaml` 和 `api/asyncapi.yaml` 是契约；`control/api/` 只实现服务和协议映射，不复制契约定义。

## API 分区

```text
/api/v1/auth
/api/v1/bots
/api/v1/adapters
/api/v1/plugins
/api/v1/models
/api/v1/groups
/api/v1/users
/api/v1/tasks
/api/v1/logs
/api/v1/audit
/api/v1/health
```

## 认证和权限

控制面板 API 必须实现：

- 登录和会话；
- Token 轮换；
- 管理员、运维和只读角色；
- API 操作审计；
- 插件权限和平台权限分离；
- 默认拒绝的 RBAC。

前端隐藏按钮不能代替后端授权检查。每一个写操作都必须由 API 服务再次检查权限。

认证规范：

- 密码使用 Argon2id 哈希；
- access token 默认 15 分钟，refresh token 默认 30 天；
- refresh token 采用旋转策略，旧 token 使用一次后失效；
- 管理员可以撤销会话并轮换 refresh token；
- 生产环境只允许 HTTPS/WSS；
- API 不返回或记录模型 API Key、OneBot Token 和 Core 内部凭据；
- 写请求使用 `Idempotency-Key`，重复请求返回原响应。

分页规范：使用不透明 `page_token`；默认 20 条、最大 100 条；token 绑定用户、过滤条件和资源版本；列表使用稳定的 `created_at, id` 排序。

## 插件管理接口

至少提供：

```text
GET  /plugins
POST /plugins/validate
POST /plugins/{id}/enable
POST /plugins/{id}/disable
GET  /plugins/{id}/config
PUT  /plugins/{id}/config
GET  /plugins/{id}/health
```

插件安装来源、版本、许可证和校验值必须记录在 Core 的插件状态中。

## WebSocket 频道

建议按频道隔离：

```text
/ws/health
/ws/logs
/ws/adapters
/ws/plugins
/ws/tasks
/ws/messages
```

WebSocket 只推送经过权限过滤的数据。普通面板用户不能订阅全部私聊消息或完整模型上下文。

WebSocket 服务端使用 WSS 和 bearer 认证；首帧必须是 `hello` 或带已有会话的 `resume`。服务端返回 `session_id` 和 `last_sequence`。断线后客户端携带 `last_sequence` 重连；服务端保留 10 分钟事件，无法补齐时返回 `RESYNC_REQUIRED`，客户端重新拉取 REST 快照。每个消息组件都有唯一 `messageId`，频道复用时不得把同一消息实例重复注册。

频道、首帧、消息 envelope 和重连语义以 [`api/asyncapi.yaml`](../api/asyncapi.yaml) 为唯一契约。OpenAPI 只描述 REST，不再把 WebSocket 仅作为文字建议。插件健康查询使用 `GET /plugins/{pluginId}/health`，返回统一 `Health` schema。

当前 AsyncAPI 固定为 2.6.0，以保持现有频道/操作模型和生成工具兼容；校验器对 3.x 的提示属于升级建议，不改变当前契约的有效性。

## 错误格式

所有 REST 错误使用统一结构：

```json
{
  "code": "PERMISSION_DENIED",
  "message": "permission denied",
  "request_id": "...",
  "trace_id": "...",
  "retryable": false,
  "details": {}
}
```

## 不属于本层的内容

- 不在 API 中编写 AI Prompt；
- 不在 API 中解析 OneBot；
- 不在 API 中直接访问数据库表；
- 不在 API 中实现插件业务；
- 不把管理员 Token 下发给插件。

## 验收标准

- OpenAPI 文档可生成 TypeScript 和 C++ 客户端；
- 未授权请求不能读取或修改资源；
- 所有写操作有审计记录；
- WebSocket 重连后能恢复订阅；
- API 在 Core 不可用时返回稳定错误。
- OpenAPI schema 通过 YAML 解析和 lint；
- 401/403/404/409/429/500/503 错误映射固定且有测试；
- TLS、token 轮换、分页和幂等请求有集成测试。

## 代码启动句

> 请只实现 `api/openapi.yaml` 和控制面板 API v1 的认证、RBAC、机器人、适配器、插件、配置、健康和审计接口，使用 mock Core 服务完成 REST、WebSocket、错误格式和权限测试。
