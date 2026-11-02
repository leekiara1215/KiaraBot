# KiaraBot Control Panel

React + TypeScript + Vite 控制面板。面板只通过 `api/openapi.yaml` / `api/asyncapi.yaml` 生成的类型访问 Control API，不持有数据库、Core 或适配器内部实现。

## 命令

```bash
npm ci
npm run generate      # 从 ../../api 生成 OpenAPI/AsyncAPI 类型（仅同仓开发）
npm run check:contracts
npm run lint
npm test -- --run
npm run build
```

`live.test.ts` 需要 C++ Control 构建与 `control/api/tests/.venv`，缺失时按条件跳过；`npm run dev` 使用 mock，或在同仓代理下连接真实 `/api/v1` 与 `/ws/control`。

## 数据边界

- 认证：登录/刷新只保存在内存；401 清空会话回到登录。
- P12 管理面 live 页面消费 Control owner 投影；HTTP 200/202 是读取或受理，完成状态以 owner operation/记录为准。
- 敏感数据：密钥不渲染、不持久化；日志/审计权威源 503 显示“数据源暂不可用”，不显示空数据。
- 实例页按 P12c-5 对象路径展示 runtime/host/plugin 三类事实；非 APPLIED 不显示完成，configuration 秘密不回显。
- logs/audit 为独立分区（log.read vs audit.read），按 P12c-6 的 occurred_from/occurred_until/actor/resource 过滤与 owner 标注消费；目标栈 live 未运行，p04 owner-gateway 验收通过。
- 资源删除/恢复（P12c-7）暂以 mock + 显式待后端提供；BLOCKED 非失败、30 天恢复窗、终态前不显示完成。
- 详细页面、P12 验收清单与未接线项见 `doc/07-control-panel.md` 的 P12d-5 段。
