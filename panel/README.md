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

## 实时流与恢复（target scoped）

- `src/stream/target-stream.ts`：target Control WebSocket scoped 客户端（按 `api/asyncapi.yaml` 冻结帧）。每 socket 绑定一个 channel/owner/规范化的精确 scope（1–127 资源，按 kind/id 去重排序）；token 轮换通过 token provider 在新 socket 上 resume，保留同 Identity session 的 Control sequence 与 `owner_cursor`（十进制 uint64 字符串，与 Control sequence 分离，用 BigInt 比较）；channel/scope 切换销毁旧 socket，旧 socket 消息不能推进新 cursor；首帧 5 秒、断线 1s→16s 退避；4401 回登录，4403/4400/1013 停止自动重试，1011/1000 退避 resume；重复去重、跳序/畸形帧/owner cursor 不匹配/RESYNC_REQUIRED 丢弃旧会话并新 hello。
- `src/stream/snapshot-sync.ts`：权威 REST 快照同步。SESSION_READY 先记 cursor，再拉 REST；读取期间事件置 dirty 并在完成后补拉；resync/跳序提升 generation，丢弃晚到的旧快照；resume 完成后也刷新。流不可用不阻塞 REST。
- `src/stream/control-stream.ts` 仍是 reference `/ws/control` 客户端（`KNOWN_EVENTS` 只用于 reference 行为，不是 target 验收）；token 轮换用 `reauthenticate()` 保留 resume sequence。
- `views/p12-live.tsx::OperationTracker`：target `/ws/operations?owner_service=...` 的 `operation.updated` 只触发 owner `GetOperation` 复核；HTTP 200/202 不是完成，BLOCKED/UNKNOWN 保持未知，断流不重发管理写；流不可用时显示“实时不可用 + 最后确认时间”并按 10s 慢速 REST 快照回退（不再用 2s 轮询作为唯一来源）。
- `App.tsx` 不再以“首个 SESSION_READY”阻塞全部 REST 页面：流未建立/中断时按服务端独立授权展示 REST 快照，并标注实时不可用与最后确认时间；4401 仍清空会话回登录。
- `scripts/check-contracts.mjs` 额外校验 target 客户端冻结常量（close codes、RESYNC reasons、首帧 5s、scope 上限、Control sequence 界、owner cursor 服务）与 `api/asyncapi.yaml` 一致。
- 测试：`tests/target-stream.test.ts`、`tests/snapshot-sync.test.ts`、`tests/p12-live-recovery.test.tsx`、`tests/app-recovery.test.tsx` 覆盖 token 轮换、旧 socket 丢弃、首帧/断线退避、close codes、重复/跳序/replay、resync、REST fallback、snapshot race 与 tracker 恢复。目标 WSS 真实联调由 `doc/plans/2026-11-04-control-ws-plan.md` 的 WS-10 统一验收，本目录 mock 测试不替代该证据。

## 数据边界

- 认证：登录/刷新只保存在内存；401 清空会话回到登录。
- P12 管理面 live 页面消费 Control owner 投影；HTTP 200/202 是读取或受理，完成状态以 owner operation/记录为准。
- 敏感数据：密钥不渲染、不持久化；日志/审计权威源 503 显示“数据源暂不可用”，不显示空数据。
- 实例页按 P12c-5 对象路径展示 runtime/host/plugin 三类事实；非 APPLIED 不显示完成，configuration 秘密不回显。
- logs/audit 为独立分区（log.read vs audit.read），按 P12c-6 的 occurred_from/occurred_until/actor/resource 过滤与 owner 标注消费；目标栈 live 未运行，p04 owner-gateway 验收通过。
- 资源删除/恢复（P12c-7）暂以 mock + 显式待后端提供；BLOCKED 非失败、30 天恢复窗、终态前不显示完成。
- 详细页面、P12 验收清单与未接线项见 `doc/07-control-panel.md` 的 P12d-5 段。
