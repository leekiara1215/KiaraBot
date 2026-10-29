# TypeScript 控制面板设计

## 责任

控制面板是 API 的客户端，负责展示状态和提交用户操作，不拥有任何核心业务规则。

## 推荐技术

第一版建议：

```text
TypeScript
Vue 3 或 React
Vite
OpenAPI 生成客户端
WebSocket 客户端
```

具体前端框架可以替换，不能改变 API 契约。

## 目录

```text
panel/
  package.json
  src/
    api/
    features/
    routes/
    components/
    websocket/
  public/
  tests/
```

`src/api/` 使用 OpenAPI/AsyncAPI 生成的客户端类型；面板不直接导入 `core/`、`infrastructure/` 或适配器代码。

## 页面分区

```text
登录
总览 Dashboard
机器人和账号
适配器连接
插件市场和插件管理
模型提供商
群组与用户权限
会话和记忆
定时任务
实时日志
审计记录
系统设置
```

## 数据规则

- 所有服务器数据来自 API；
- 前端状态只做缓存和展示；
- 表单提交前可做体验性校验，服务端仍必须再次校验；
- 不把 Token 或 API Key 写入浏览器持久化存储；
- 日志和消息内容按 API 返回的脱敏结果展示；
- 前端不直接调用 NapCat 或数据库。

## 实时状态

WebSocket 消息应包含：

```text
channel
schema_version
event_type
occurred_at
payload
```

客户端必须允许未知事件和未知字段，断线后按退避重新连接。

## 未来 C++ 控制面板

未来如果使用 C++ 编写桌面控制面板，它只需要复用：

- OpenAPI REST 接口；
- WebSocket 频道；
- 认证和错误模型。

因此 TypeScript 前端不能把关键业务逻辑写在组件内部。

## 验收标准

- 页面可以独立使用 mock API 开发；
- 插件启停、配置和日志页面不需要直接导入 Core 代码；
- 刷新页面后状态从 API 恢复；
- 断线时有明确状态提示和自动重连；
- 没有把密钥保存到 localStorage。

## 代码启动句

> 请只实现基于 `api/openapi.yaml` 的 TypeScript 控制面板骨架、认证、总览、适配器、插件、模型、权限和实时日志页面，使用 mock API 开发，不连接数据库、NapCat 或 Python 进程。
