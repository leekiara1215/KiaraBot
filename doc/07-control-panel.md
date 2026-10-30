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

服务端以 `SESSION_READY` 返回会话 ID 和确认序号；客户端断线后按退避重连并以 `resume` 发送 access token、会话 ID 和最后处理的序号。收到 `RESYNC_REQUIRED` 时清除旧会话，以 `hello` 建立新会话并重新拉取 REST 快照。日志事件使用 `log.updated` 和 OpenAPI `LogEntry`。客户端允许未知事件和未知字段，权限页面展示服务端计算的有效权限。

## 本地运行和检查

`panel/` 使用 React、TypeScript 和 Vite。`npm ci` 根据锁文件安装依赖，并从 `api/openapi.yaml`、`api/asyncapi.yaml` 生成类型；生成目录、`node_modules/` 和 `dist/` 均不提交。默认 `npm run dev` 使用内存 mock API 和 mock WebSocket，输入任意非空用户名和密码可登录。仅在连接真实控制 API 时设置 `VITE_API_MODE=live`，可使用 `VITE_API_BASE` 和 `VITE_WS_URL` 指定端点；生产环境必须使用 HTTPS/WSS。

access token、refresh token 和流会话仅保存在内存；到期前调用 refresh 接口轮换，刷新页面后需要重新登录。列表显示服务端快照，日志事件及重新同步触发快照刷新；面板不计算权限。检查命令（在 `panel/` 下执行）：`npm ci`、`npm run check:contracts`、`npm run lint`、`npm test`、`npm run build`。

## 其他模块的后续事项

- **控制 API 服务（`control/api/`）**：实现更新后的 `Group`、`User` 和 `LogEntry` 响应，返回服务端计算的有效角色、权限和脱敏日志；继续执行默认拒绝的 RBAC 和 401/403 错误契约。现有 `GenericResource` 消费方需要适配新增必填字段。
- **控制 API 流服务**：实现首帧认证、`SESSION_READY`、绑定认证主体的秘密会话 ID、10 分钟事件保留、按序恢复和 `RESYNC_REQUIRED`；浏览器不提供自定义 WebSocket 握手 Authorization 头。`log.updated` 必须携带契约中的脱敏 `LogEntry`。
- **Core / 基础设施端口**：由相应模块负责人确认如何提供有效权限关系与脱敏结构化日志，再通过既有公开端口提供给控制 API；本变更不假设或修改它们的内部实现。
- **跨模块验收**：服务端应覆盖登录和轮换、权限允许/拒绝、日志脱敏、会话过期、断线恢复、保留期外重新同步及依赖故障，并以更新后的契约校验真实服务响应。当前面板验证使用 mock，不能证明服务端已实现这些行为。

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
