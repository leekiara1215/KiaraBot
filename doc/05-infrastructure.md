# 基础设施适配器设计

## 责任

基础设施适配器把 Core 定义的端口接口连接到具体数据库、模型、缓存、文件系统和调度器。

它们不实现消息路由、插件权限或控制面板逻辑。

## 目录

```text
infrastructure/
  sqlite/
    migrations/
    include/
    src/
    tests/
  model/
    http/
    providers/
    tests/
  scheduler/
    src/
    tests/
  audit/
    src/
    tests/
  migrate/
    src/
    tests/
  fake/
```

迁移文件归 `infrastructure/sqlite/migrations/` 管理；`deploy/` 只负责调用迁移命令和编排进程。

## 第一批端口

```text
IModelProvider
IConversationStore
IUserRepository
IPermissionStore
IScheduler
IFileStorage
IAuditLogger
```

## 模型适配器

模型适配器负责：

- Provider 和模型选择；
- API Key 管理；
- 请求超时和重试；
- 流式响应转换；
- 用量和错误记录；
- OpenAI-compatible 或其他厂商协议转换。

插件只能使用 `ModelService`，不能获得 HTTP 客户端和 API Key。

## 存储适配器

第一版建议使用 SQLite。Core 定义迁移版本和事务边界，基础设施执行迁移和连接管理；插件只能使用命名空间存储：

```text
plugin:<plugin_id>:<key>
```

插件不得创建或修改 Core 表，不得保存未经过 Schema 校验的任意大对象。

基础设施必须实现 Core 定义的接口：

```text
IUnitOfWork.begin() / commit() / rollback()
IInboxRepository.put_if_absent(event_id)
IOutboxRepository.append(action)
IActionRepository.get_by_idempotency_key(key)
```

迁移由独立的 `kiarabot-migrate` 命令执行：先获取迁移锁，按版本顺序执行，成功后写入 migration table；锁等待超过 30 秒失败，不允许多个进程同时迁移。SQLite 必须启用 WAL、外键约束和 busy timeout。

## 调度器

调度器负责保存任务、触发任务、处理时区和重试。任务必须记录：

```text
task_id
owner_plugin_id
target_conversation
schedule
payload
status
next_run_at
```

## 审计与日志

所有管理动作、权限拒绝、插件启停、模型调用失败和高风险 Action 都必须写入结构化审计日志。

日志不应包含：

- API Key；
- OneBot Token；
- 完整的私聊内容；
- 未脱敏的用户隐私。

## 替换规则

实现 PostgreSQL、Redis 或新的模型供应商时，只新增适配器，不修改 Core 的端口和领域规则。若端口不足，应先修改 `core/ports/`；若需要改变跨进程语义，则先修改 `proto/`，再修改实现。

## 验收标准

- 可以使用 fake adapter 测试 Core；
- 模型超时、限流和错误都有稳定错误码；
- 存储事务不会泄漏到插件；
- 插件停用后任务可以暂停或取消；
- 适配器替换不会修改业务代码。

## 代码启动句

> 请只实现第一版基础设施端口和 SQLite、模型 HTTP、调度器及审计日志适配器，使用 fake Core 和 fake Plugin 完成超时、事务、重试、权限和数据隔离测试，不接入 NoneBot2 或控制面板。
