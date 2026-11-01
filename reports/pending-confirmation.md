# 仍需裁定与未验证事项

> 核验更新：2026-11-01；代码基线：本地 main `c63ce59`。
> 仅保留待裁定、待操作或尚未取得验收证据的事项。历史执行结果见 [`execution/final-summary.md`](execution/final-summary.md)、[`execution/2026-10-31-ci-run1.md`](execution/2026-10-31-ci-run1.md) 与 [`../doc/16-development-tasks.md`](../doc/16-development-tasks.md)。本地 `origin/main` 与该基线一致，不代表已实时核验远端。
> 本次只更新本报告；不修改受保护的 `doc/18-architecture-baseline.md`，不恢复 CI 自动触发。

## 1. 待执行或待核验的操作

| 事项 | 当前证据与缺口 | 下一步及验收条件 |
| --- | --- | --- |
| **最新提交的 CI 复跑** | 首轮报告记录 main `bac6fe3` 的 10 个 job 中 8 成功、format/secret scan 失败，包含 contracts 与 full-build 成功；修复及 W5–W7 已在其后提交，不能用首轮结果证明当前 HEAD 全绿。本次未在线独立核验运行记录。 | 手动 Run workflow，选择包含最新改动的 main，记录实际 SHA、run 链接及所有 job 结果；失败后读取日志并修复。`base_ref=origin/main` 在 main 上可能与自身比较，只能验证当前门禁；证明跨版本兼容须指定可审计的旧提交，或用 `baseline_release` 比较上一发布版 descriptor。 |
| **Docker / 部署实机验收（T11b）** | 当前 PATH 无 Docker；镜像构建、Compose 起栈、host 网络、健康依赖链及备份恢复/回滚仍缺实机验收证据。 | 优先在 Linux + Docker Engine/Compose 环境按 `doc/08-operations.md` 执行 Compose config、构建/启动、迁移、健康/冒烟、备份恢复与指定 digest 回滚；仅配置解析成功不能勾选 T11b。 |
| **真实 QQ/NapCat/模型联调（T07b）** | 现有隔离测试及 fake 平台/模型不等于真实服务验收；仍需隔离账号、平台、模型服务与目标会话配置。 | 按 `tests/acceptance/real-environment.md` 准备环境并留存真实收发、错误分类、限流与恢复结果。 |
| **分支保护 / required checks** | 仓库文件不能证明 GitHub 设置已生效，本次未查询设置。 | 管理员核验 main 的必需检查与绕过规则；按 Actions 实际 check 名称配置。自动触发暂停期间还须确认检查不会永久 pending，不能声称已自动阻止不兼容合并。 |
| **临时工作树与 build 清理（可选）** | `git worktree list` 仍列出多份 `/private/tmp/kb-*`；尚未逐一确认其未提交改动、独有提交与占用情况。 | 清理前逐一核对状态、提交是否已合并及是否仍有任务使用；确认后用 `git worktree remove` 清理。`build/` 需先确认没有仍需保留的日志或验证产物。本报告不授权或执行删除。 |

CI 当前仍仅 `workflow_dispatch`，`pull_request`/`push` 保持注释。恢复自动触发不作为本轮操作；如后续需要恢复，须另行确认，并先取得最新提交全部 job 通过的证据。

## 2. 仍需人类维护者裁定

| 事项 | 已核实的当前边界 | 待裁定问题 |
| --- | --- | --- |
| **受保护基线与实现差异** | `doc/18` 附录 A 及适配器说明仍写适配器未入仓，实际已在 `adapters/nonebot/`。面板目标范围与现有管理接口仍有差距：机器人账号、模型 API Key、群白名单、用户角色写接口尚缺。 | 由人类维护者决定是否修订基线或排期补接口；AI 只记录差异。未来 C++ 组件未实现/已延期是目标与完成度的区别，不单独作为必须修订基线的问题。 |
| **命令幂等清理** | 命令记录无时间戳，仍按数量上限确定性淘汰；当前快照/schema 已是 v6。 | v1 是否接受数量淘汰；如需时间型过期，另行设计时间戳、格式版本及迁移，不能再描述为“升级到 v6”。 |
| **多插件/多适配器装配** | 组合根以单组适配器身份和单个受管插件配置为主，尚无完整的配置化多实例装配。 | 是否维持单实例 v1，或排期多实例配置。 |
| **完整 RBAC 与内容策略** | 已有多种 Action 权限、出站速率限制、每日预算、会话白名单与审计路径；不能再按“只有 message.send、没有危险 Action 门禁”排期。 | 明确完整 RBAC、内容策略及其余安全控制的范围和优先级；按 `doc/17-account-ban-risk.md` 建议与当前实现逐项对照，避免重复实现已完成的门禁。 |
| **FileStorage** | `FileStorage`/`FakeFileStorage` 已实现并有测试，Core 无真实用例、组合根未创建，不能承诺文件存储能力。 | 继续保留未接线状态，还是由 Core 定义真实用例后接线，或废弃实现。 |
| **ACK 超时默认值** | `KIARABOT_ACK_TIMEOUT_MS` 默认 30000 ms，可配置；未 ACK 超时计作一次可重试失败，随后按退避与重试上限处理，并非保证只重投一次。 | 是否接受 30s 默认值；长回调需结合插件处理时间与重复投递语义评估。 |
| **传输层契约登记** | AsyncAPI 已登记慢消费者关闭码 1013，OpenAPI 已登记多处 429；405、OPTIONS、HEAD、CORS、请求追踪头等仍有登记缺口。 | 是否安排 contracts 变更补齐遗漏，并在登记与验证后视为稳定契约；无需重复登记已有条目。 |
| **部署网络模型** | staging/prod Compose 均使用 host 网络以配合回环监听。Docker Desktop 4.34+ 可显式启用 host networking，但不能据此认定本仓库栈已通过 Desktop 验收。 | 是否接受 Linux 单机 host 网络作为 v1 部署目标，或另行设计受控监听地址/共享网络命名空间及对应安全契约。 |
| **工具链验收口径** | 本机与 CI 的 Python、LLVM、Protobuf/gRPC 版本存在差异，详见下表。 | 是否以 CI 固定版本为验收标准并记录本地差异；如需本机完全对齐，另行安排安装。 |

Docker Desktop 支持范围参考 [Docker 官方 host network 文档](https://docs.docker.com/engine/network/drivers/host/)；支持该功能不替代 T11b 实机验收。

## 3. 工具链差异与验证缺口

| 项 | 本机证据 | CI 与待验证事项 |
| --- | --- | --- |
| Python | SDK/适配器虚拟环境配置为 3.11.17；默认系统 `python3` 为 3.9.6，不能笼统写“本机 Python 是 3.11.17”。 | workflow 指定 3.11.18；最新提交须在该环境复跑。 |
| clang-format | PATH 为 Homebrew 23.1.4；首轮修复及 W6/W7 台账记录使用 16.0.6 验证，不能再写“只在 23 下通过”。 | CI 使用 clang-format-16；既有本地通过记录不能替代最新 HEAD 的 format job。 |
| Protobuf / gRPC | protoc 实测 36.2；此前报告记录 gRPC 1.84.1，本次未重新核验其安装版本。 | 固定 vcpkg registry 对应 Protobuf 29.3 / gRPC 1.71.0；本机构建不替代固定版本验收。 |
| clang-tidy | 当前 PATH 未找到 clang-tidy/clang-tidy-16；完整本地 `scripts/check-toolchain.sh` 缺少该项验证。 | CI 使用 clang-tidy-16；首轮报告已有 full-build 成功记录，但最新提交仍待复跑。 |
| Docker / CD | PATH 无 Docker，缺部署实机验收。 | 当前 CI 无 Docker job，也未接通镜像发布/部署；不能宣称镜像或部署已验证。 |
| 依赖审计 | 本地验证不能证明所有依赖均已覆盖。 | panel/Python 审计为 report-only；vcpkg C++ 依赖不在现有漏洞审计范围。 |

## 4. 各模块剩余验收与验证缺口

以下区分既有记录与未完成验收；不把历史“未运行”直接沿用为最新事实。

- **contracts**：旧 reader 本地默认 opt-in，但 W6/W7 台账已有启用 legacy reader 的门禁通过记录，CI 也强制启用；最新提交的远端复跑仍待确认。上一发布版 descriptor 是否存在并实际用于跨版本比较尚未核验，自身比较不能替代发布兼容验证。
- **core**：运行时环境变量尚缺真实部署组合验收；维护循环性能未压测。新增 Action 授权/配额路径与会话键迁移已有隔离测试记录，仍需固定 CI 工具链和部署环境验证。
- **infrastructure**：SIGKILL 恢复测试不等于断电验证；容量拒绝计数尚未接监控告警，迁移锁重试及容量/性能仍需实际部署验证。
- **adapter**：真实 NapCat/QQ 联调待 T07b；首轮 CI 报告已有包含 acceptance 入口的 contracts job 成功记录，不能再写“全量入口从未跑”。备份仅恢复一侧导致 `out_of_order` 的恢复场景仍需演练；outbox 1→2 迁移需在部署中验证。
- **plugin-sdk**：现有闭环不等于任意新插件集成已验收；新插件与真实 Core 的端到端验证、插件发现/多实例及日志/健康、会话、模型流、权限查询等剩余范围需逐项明确并验收。
- **control-api**：历史全量 pytest 曾有时序偶发失败（单测复跑通过），长期稳定性仍待验证；限流默认值未按生产负载校准，TLS 部署未验收。
- **control-panel**：live 测试依赖已构建 Core/测试环境，缺前置时可能 skip；需核对最新 CI 实际执行与跳过数，不能用固定旧测试数量作为验收。axe/i18n/服务端过滤与视觉走查仍待完成。
- **operations**：T11b 仍缺镜像/起栈、迁移失败阻断、健康、备份恢复及回滚演练；`EXPECTED_SCHEMA_VERSION` 当前为 6，后续 schema 变更仍须同步。真实 digest、TLS 反代、集中日志与告警缺少部署验收。
- **ci-cd**：最新 HEAD 的全量 job 结果未确认；自动触发继续暂停。首轮报告已记录 contracts/full-build 成功，不能再写“full-build 从未在远端执行”；也不能由首轮部分成功推出当前 HEAD 全绿。
