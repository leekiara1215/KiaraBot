# 基础设施与其他模块的交接事项

## 当前状态

当前是阶段性交付，不是可部署的完整机器人。已有 POSIX 文件存储、SQLite 运行时、插件 SDK/启动器、Action 纯文本闭环和 Host 命名空间存储。模型/任务端口及消息仍需实际实现，不能因协议存在就声称可用。逐项验收以 doc/16-development-tasks.md 为准。

T05a/T05b 已提供统一事务快照端口；T10a 实现命令事务审计；T03b 实现进程启动与 SDK；T04b/T06a 将 Action 路由、Host 存储/幂等响应加入 schema v4，并完成真实 C++/Python 故障验收。仍缺模型 Provider、Scheduler、通用日志接线和部署演练。

## 需要其他模块完成的工作

### Core / 插件宿主

- T06a 已在 PluginBridge 处理存储 HostRequest，关联 request_id 并校验身份、权限、deadline、TTL 和容量；模型/任务仍需扩展。
- T06a 已使用 RuntimeTransaction 原子提交存储写入、七天幂等响应和审计，同键不同内容 CONFLICT；未来扩展应保持相同原子性。
- T05a 已将公共 Runtime 改为 StorageResult，桥接业务实现需将 Unavailable/Conflict/Corrupt 映射到 UPSTREAM_UNAVAILABLE/CONFLICT/INTERNAL；不能对未检查的 variant 取业务结果。
- 核对 `ports.hpp` 在实际应用服务中的事务所有权、Scheduler 领取与 fencing、错误和关闭语义，并以契约测试验证；当前只是接口草案，尚无完整实现。
- T06a 已将 validate_host_request 接入存储入口，覆盖 UTC deadline、TTL、权限、限额、存储故障和幂等；模型/任务仍需补齐其各自验证，包括 IANA 时区与夏令时。
- RuntimeTimePoint 已为 UTC 系统时钟；T05b 已验证重启恢复，仍需在业务服务接线后验证时钟跳变和调度策略。

### Plugin SDK

- T06a 已提供生成类型驱动的插件命名空间存储；下一步实现非流式模型与一次性任务 API，不暴露数据库、Core 表或模型密钥。
- 已实现 Action/存储请求关联、deadline、断线未知结果及同键重试；任务状态和任务取消仍待 T06c。
- 流式模型仍未定义，不能将本次非流式消息描述为流式能力。

### 协议和契约维护

- 评估原有 `ActionEnvelope.schedule_task.schedule` 的语法和迁移路径。新 Host API 使用结构化一次性时间，但没有替换或重新解释旧字段；不要假定两条路径已经等价。
- 存储已有真实服务执行、结果互斥、隔离、重放、拒绝、超时与依赖故障测试；模型错误和任务状态仍待各自任务覆盖。
- 命名空间值与总量限额已执行；模型消息和任务 payload 的资源限制仍待冻结并实现。

### 运维与工具链

- 待 SQLite 迁移命令和服务接线落地后，更新启动顺序、迁移锁、健康检查、备份恢复、密钥注入及审计输出配置，并验证失败时阻止启动。
- 当前 Xcode clang-format 可以解析仓库配置，修改文件格式检查通过；clang-tidy 缺失，统一静态分析仍未执行。

## 基础设施后续验收

实现其余适配器后，至少验证 SQLite 迁移、事务提交/回滚、命名空间隔离和 SQLite/fake 一致性；模型超时、限流、重试和错误映射；调度时区、取消、重启恢复与过期领取；审计字段完整性和敏感数据排除；各适配器的 fake 和故障注入。完成前不得将本次提交标记为完整实现。
