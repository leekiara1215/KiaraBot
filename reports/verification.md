# 十模块 P0/P1 缺口独立复核（task-11）

> 范围：从 10 份 `reports/modules/*.md` 各抽 2 条最关键 P0/P1（优先 P0），共 20 条，逐条回源核对证据行号、缺口类型、严重度。只读源码/契约/reports；未构建、未联网（CI 历史以 README/doc/12 自述为据）。本复核者同时是 control-panel 报告作者，该模块 2 条按源码重新核对。

## 复核表

| 模块 | 报告声明（摘要） | 复核结论 | 反证或补充证据 | 建议修正 |
|---|---|---|---|---|
| 1. contracts | G1 契约兼容门禁空转（P0） | 成立 | `scripts/ci/check-descriptor.sh:5-27` 依赖 baseline；`ci.yml:121-132` `BASE_SHA=…\|\|inputs.base_ref`（默认 `origin/main`）；`git rev-parse main origin/main HEAD` 三者同为 `8caecb1` → main 上自比恒通过；artifact 只有 `upload`（ci.yml:127-131）无任何 `download-artifact` | 保留 P0；建议拆成"脚本能力（可用）/当前运行模式（自比+不自动触发）"，并说明 PR 触发恢复后 base 用真实 base.sha |
| 2. contracts | G2 队列满 `RATE_LIMITED` retryable=false（P1） | 成立 | 实现 `adapter_ingress.cpp:26-31`（错误工厂默认 false）、`:242-245`（未传 true）；文档 `doc/01-runtime-semantics.md:119`、`doc/01-contracts.md:263` 要求"可重试 RATE_LIMITED，retry_after_ms=1000"；测试 `adapter_ingress_test.cpp:122,205` 期望 true | 行号纠正 124/203-207→122/205；与 adapter 报告 P0 分级不一致，建议统一（见"模块间矛盾"） |
| 3. core-runtime | G1 `purge_expired` 无生产调用方（P0） | 成立 | 全仓仅测试调用（`core_smoke.cpp:65`、`plugin_delivery_test.cpp:68,171,177`、`runtime_test.cpp:56,154`、`runtime_store_test.cpp:73,77`）；`core/app/main.cpp:64` 的 Runtime 仅 `--check`；审计无 erase/clear（grep 零命中）；`authorized_commands.hpp:84-86` 审计满即 `UPSTREAM_UNAVAILABLE` | 保留 P0；可把"审计无清理"与 G19 合并为单一根因 |
| 4. core-runtime | G2 共享快照无全局串行化（P1） | 成立 | 静态可证：`main.cpp:178,181,186,211,217,249` 同一 `*persistent_store` 注入 6 个服务；`sqlite_runtime_store.cpp:90-121` 仅事务内锁 + `WHERE revision=?`；无串行化装饰器；`doc/02-core.md:242` 明确要求"公共 Runtime 串行访问" | 保留；注明"静态可证、未做并发热点运行验证" |
| 5. plugin-sdk | G1 无插件发现/注册表（P1） | 成立 | `main.cpp:155-178` 由 `KIARABOT_PLUGIN_*` 构造单一 `PluginPolicy`；`plugins/` 无扫描/索引；`doc/08-operations.md:19` 自认 | 保留 |
| 6. plugin-sdk | G2 ConversationService 无线路/封装（P1） | 成立 | `doc/03-plugin-sdk.md:186-194` 列 get/append/clear；`proto/kiarabot/v1/plugins.proto:140-149` `HostRequest` 仅 7 个 oneof 操作，无会话 | 保留 |
| 7. nonebot-adapter | #1 Action conversation_id 与事件前缀矛盾（P0） | 成立 | `mapper.py:45,63` 生成 `group:`/`private:`；`actions.py:50-52` 要求纯数字；`plugins/ai-reply/ai_reply.py:34` 直接复制事件 conversation；测试仅用数字 ID（`test_actions.py:14-15`） | 机制需更正：Core 先拒——`action_router.cpp:26-32` 的 `id()` 不允许 `:`，`:119-127` 即返回 INVALID_ARGUMENT；适配器数字校验是第二道 |
| 8. nonebot-adapter | #2 背压/拒绝一律永久失败（P0） | 部分成立 | 成立：队列满 `adapter_ingress.cpp:242-245` retryable=false，`client.py:115-120` 非可重试即 `outbox.finish` 标 failed。不成立："Core 拒绝错误全部 retryable=false、无任何置 true 路径"——`authorized_commands.hpp:128-129` 对 RATE_LIMITED/UPSTREAM_UNAVAILABLE/TIMEOUT 置 true，`adapter_ingress_test.cpp:105,108` 亦期望 true | 改写为"入口自身背压 RATE_LIMITED 为 false，与 AuthorizedCommands 映射的同类错误语义不一致"；P0 可保留（doc/16:111 背压不丢事件已勾选） |
| 9. infrastructure | A1 doc/05 端口承诺未兑现（P1） | 成立 | `doc/05-infrastructure.md:89-98` 列 IUnitOfWork/IInboxRepository/IOutboxRepository/IActionRepository；`ports.hpp` 只有 `IUnitOfWork`（:52-69），无 Inbox/Outbox/ActionRepository，且 IUnitOfWork 无实现 | 保留 |
| 10. infrastructure | A10 快照 64 MiB 上限无测试（P1） | 成立 | `sqlite_runtime_store.cpp:12,98-100` 超限 commit 返回 `Unavailable`；`runtime_snapshot_codec.hpp:67,116` `remaining_items=1000000`；全仓 grep 无上限测试 | 补充"1,000,000 项上限同样无测试" |
| 11. control-api | #1 messages 频道永久 4403（P1） | 成立 | `event_stream.cpp:8-15` kEvents 无 messages；`control_crypto.cpp:125-136` 16 项权限无 `message.read`；`event_stream_test.cpp:37` 与 `test_stream.py:58-64` 把 4403 写成预期 | 与 contracts G3 同根因，建议 contracts 主责、control-api 引用 |
| 12. control-api | #3 Action 查询无生产者/无保留期（P1） | 成立 | `control_service.cpp:489` 只匹配 `owner=="control/"+user`，全仓无写该 owner；`:490-514` 无 7 天保留/ACCEPTED 过期判定 | 补充：`action_router.cpp:168-186` 的 lookup 已有 7 天保留+deadline→EXPIRED，建议 REST 查询复用它；与 contracts G6 重复 |
| 13. control-panel | G1 零管理动作、7 个已就绪端点未接线（P1） | 成立 | `panel/src/api/client.ts:26-51` 仅 login/refresh/me/revoke + 9 个 GET；无插件/会话/action 方法 | 保留；与 control-api #3 构成依赖：先修 API 再接线 |
| 14. control-panel | G7 `npm test` 不能独立运行（P1） | 成立 | `tests/live.test.ts:19` 硬编码 `../control/api/tests/.venv`；`control/api/tests/conftest.py:17,28,49` 需 build/proto 的 core 与 credentials 二进制；`doc/07:76`/`README:99-105` 未写前置 | 保留；建议补文档或缺失前置时 skip |
| 15. operations | P0-1 镜像构建的是 dev Core（P0） | 成立 | `Dockerfile:11-12` 无任何 `-D` 选项；`CMakeLists.txt:4` GENERATE_PROTO=OFF、`:120-122` INFRASTRUCTURE=OFF；`--target kiarabot-core` | 保留 |
| 16. operations | P0-4 回环绑定与 compose 拓扑冲突（P0） | 成立 | `core/src/runtime/bridge_server.cpp:527-529` 强制 `127.0.0.1:` 前缀；`control/api/src/http_server.cpp:296` 同；`compose.staging.yml:33,59,84` 用服务名 + 宿主回环映射 | 补充双向证据：`adapters/nonebot/.../client.py:29-31` 正则也强制 `127.0.0.1`，即使 Core 可达也无法跨容器；当前因 P0-3 未监听而"暂被掩盖"，应升级为跨模块决策 |
| 17. toolchain | #1 统一检查不覆盖 4 条链（P1） | 成立 | `scripts/check-toolchain.sh:99-118` 仅 cmake presets/contracts/format/tidy/openapi/asyncapi，无 npm/pytest/acceptance | 保留；doc/11 需明确"统一检查≠全量验收" |
| 18. toolchain | #3 本地与 CI clang-tidy 覆盖不一致（P1） | 成立 | `CMakeLists.txt:25,90` control/api 仅在 GENERATE_PROTO=ON 加入；`CMakePresets.json` dev 为 OFF；`ci.yml:180-195` 用 build/dev；`check-tidy.sh:12` 用 build/proto 且扫 control/api+infrastructure | 保留 |
| 19. ci-cd | #1 自动触发被关闭（P0） | 成立 | `ci.yml:3-17` 仅 `workflow_dispatch`，PR/push 注释；`doc/12-ci-cd.md:97` 自述；`doc/12` 验收"PR 自动编译测试"不成立 | 保留 P0；其余报告引用此结论即可 |
| 20. ci-cd | #2 HEAD 的修复未在远端验证（P0） | 成立 | `README.md:52` "历史 5 次未通过（4 失败 1 取消）"；`doc/12-ci-cd.md:103` "尚未在远端验证全绿" | 保留 |

## 特别交叉核对（5 条）

1. **conversation_id 矛盾**：成立；但失败点首先在 Core（`action_router.cpp:26-32` 的 id() 禁止 `:`），而非适配器判错。
2. **RATE_LIMITED retryable**：`adapter_ingress.cpp:26,242`（false）与 `adapter_ingress_test.cpp:122,205`（true）确为静态矛盾；但"所有拒绝都 false"不成立（`authorized_commands.hpp:128-129`）。两份报告的分级 P1/P0 需统一。
3. **purge_expired**：成立，仅测试调用。
4. **回环绑定**：成立，且适配器侧 `client.py:29` 正则同样强制回环，是 Core/控制面/适配器三方与 compose 的双重冲突。
5. **workflow_dispatch 与 descriptor 基线**：成立；main 上手动运行 `origin/main==HEAD` 自比恒通过，恢复 PR 触发后才用真实 base；上传 artifact 无 download 消费者。

## 模块间矛盾与重复结论

- **CI 自动触发关闭被 ≥6 份报告重复**（ci-cd #1 P0、contracts G1、plugin-sdk G6、nonebot #20、infrastructure A7、panel G8）。建议只在 ci-cd 保留主结论，其余改引用，否则同一 P0 被多模块重复计数。
- **背压 retryable 分级矛盾**：contracts G2=P1、adapter #2=P0。按"已声明验收项"口径（`doc/16:111` 背压不丢事件已勾选）应统一为 P0；adapter 的"全部 false"表述需按 row 8 改写。
- **Action 查询重复**：contracts G6 与 control-api #3 同根因、同 severity；control-api 未指出 `ActionRouter::lookup` 已实现保留期，应改为"复用 lookup 即可修复"而非"未接线"。
- **messages 频道重复**：contracts G3 与 control-api #1 同根因同 severity，结论一致（无权限授予 + 无投影），保留一处即可。
- **保留期/容量同源**：core G1(P0) 与 G19(P1) 自认同源；infrastructure A10 的 64 MiB 写死是同一"运行期无界增长→写路径死亡"风险的另一面，建议合并为一条跨模块 P0，统一处置（清理调度 + 容量告警）。
- **回环绑定属跨模块**：core BridgeServer、control/api HttpServer、adapter client 三处独立强制回环 + operations compose 冲突，单模块无法修复，需运维+Core+适配器共同决策。
- **panel G1 与 control-api #3 的依赖顺序**：actions 端点无生产者，即使面板接线也恒 404；正确顺序是先修 control-api 再接线面板。

## 统计

- 20 条：**成立 19、部分成立 1（adapter #2）、不成立 0、无法核实 0**。
- 特别交叉核对 5 条全部给出源码级结论，其中 2 条需修正表述（adapter #2、adapter #1 失败点归属）。
- 最需修订：adapter #2 的"全部 retryable=false"；contracts G2 的测试行号；adapter #1 的失败点归属（Core 先拒）。
