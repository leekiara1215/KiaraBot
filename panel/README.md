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
- logs/audit 为独立分区（log.read vs audit.read）；P12c-6 未接线前显示“数据源暂不可用（503）”，分页/过滤/owner 映射已准备，待 P12c-6 后真实复验。
- 详细页面与契约/未接线清单见 `doc/07-control-panel.md` 的 P12d-4 段。
