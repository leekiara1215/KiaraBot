# 部署与可观测性设计

## 责任

运维层负责让各个模块可启动、可停止、可检查、可升级和可恢复。

## 目录

```text
deploy/
  compose.dev.yml
  compose.staging.yml
  compose.prod.yml
  docker/
  systemd/
scripts/
  check-toolchain.sh
  check-contracts.sh
  check-boundaries.sh
```

数据库迁移源码位于 `infrastructure/sqlite/migrations/`，运维目录只负责编排、启动、健康检查、备份和回滚。

## 进程

```text
napcat
kiarabot-adapter-nonebot
kiarabot-core
kiarabot-panel
```

开发环境可以使用 Docker Compose，单机生产环境也可以使用 systemd。无论使用何种方式，进程配置都不能改变业务协议。

## 启动顺序

1. `kiarabot-migrate` 获取迁移锁并完成数据库迁移；
2. Core 启动并检查 schema 版本；
3. Python 适配器注册并报告能力；
4. NapCatQQ 连接适配器；
5. 面板静态资源或面板服务连接 API。

## 健康检查

每个进程必须提供：

```text
/health/live     进程是否存活
/health/ready    是否可以接收请求
/health/details  依赖和版本详情
```

`live` 只检查进程；`ready` 检查数据库 schema、数据库可写、协议服务和必要运行时；`details` 必须脱敏。Core 未 ready 时 API 返回 503，适配器未 ready 时不接收新 Action。

```text
Core live       不依赖数据库
Core ready      依赖迁移完成 + 数据库可写 + RPC 监听
Adapter ready   依赖 Core 注册 + NapCat 连接
Panel ready     依赖 API 可达，不要求 QQ 在线
```

适配器还要报告 NapCat 和 Core 的连接状态。插件通过 PluginSDK 上报健康状态。

## 日志

日志必须为结构化格式，并包含：

```text
timestamp
level
component
operation
outcome
request_id
trace_id
event_id / action_id
resource_id
message
error_code
```

每个操作的成功、拒绝、重复、超时、重试、死信、依赖故障和状态转换都必须产生至少一条结构化记录，并通过 `request_id`、`trace_id`、`event_id` 或 `action_id` 关联。日志中不得记录 Token、API Key、消息正文和未经脱敏的私聊内容。日志端口和 sink 在后续实现任务中落地，运维层负责 JSON sink、轮转、集中收集、保留期和告警。腾讯 [BqLog](https://github.com/Tencent/BqLog) 是候选 sink，接入前需要单独完成许可证、性能、格式和跨平台评估。

## 备份和升级

- `.env` 和密钥不进入 Git；
- 数据库在迁移前备份；
- 插件升级前记录版本和校验值；
- 协议升级先做兼容测试；
- NapCat、适配器和 Core 分别记录版本；
- 失败升级应可以回滚到上一个版本。
- 迁移命令必须可单独、可重复运行，失败时阻止后续服务启动；
- 备份必须包含 schema 版本和校验值。

## 不属于本层的内容

- 不修改业务数据绕过 API；
- 不在启动脚本中实现权限判断；
- 不把第三方源码复制进 Core；
- 不通过日志系统传递业务命令。

## 验收标准

- 任一进程退出后可被单独重启；
- Core 和适配器断线可以恢复；
- 健康检查能区分存活和就绪；
- 日志可以按 trace_id 关联一条消息链路；
- 数据库和插件配置有备份方案。

## 代码启动句

> 请只实现 KiaraBot 各进程的 Docker Compose/systemd 启动配置、健康检查、结构化日志、重启策略和本地开发环境，不修改 Core、插件或适配器业务代码。
