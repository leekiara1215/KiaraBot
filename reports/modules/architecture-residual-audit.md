# P00o：全仓 TODO/not_implemented 残余缺口终审 + 证据一致性抽查

- 基线：main@`23aa6ab`（final CTest gate 148/148）；工作树 `codex/p00o`。
- 性质：只读审计 + 报告；**A 类真实遗漏为 0**，因此本任务未改实现/测试，也未运行全量构建（任务
  允许“仅 A 类修复时构建”）；文档验证仅做本报告相对链接检查。
- 扫描面：`core/ control/ infrastructure/ adapters/ sdk/ panel/ scripts/ deploy/` 源码与脚本；
  排除 build/.worktrees/node_modules/生成物、`core/reference/` 兼容层、冻结 doc/18、`proto/` 中
  `CONTRACT_STATUS: not_implemented` 的契约声明（归 B 类）、已记录的环境/用户决策项。

## 1. 扫描方法与原始命中

- `TODO|FIXME|XXX|HACK`：全仓目标目录共 **20 条**，全部为 `mktemp ...XXXXXX` 模板/锁文件
  校验串（core 1、infrastructure 6、scripts 12、panel 1 为 package-lock 哈希），**D 误报**，无真实 TODO。
- `not implemented|unimplemented|未实现|未接线|placeholder|stub|deferred|unsupported` 逐目录
  过滤后（排除 gRPC `Stub`/`stub_` 类型名、临时目录 XXX、生成/契约字符串）得到：
  - B 契约声明未实现：`proto/tests/contract_status.json` target 未翻项仍占多数（消息 228、
    字段 753、枚举值 115、REST operations 59、channels 8、stream events 7、host_operations 7 等）；
    对应 proto 注释、OpenAPI `x-contract-status` 与 doc/01 均已有说明，不属遗漏。
  - C 环境/决策待办：Docker/Compose 真机、两主机/断网、远端 CI、GHCR/release 签名、真实
    QQ/Provider、KMS/备份离站、P05b 实例委托、P13 生产授权/participant、P12 host operation/
    bootstrap_credential、目标 Control WS/streaming。
  - D 误报/历史文案：gRPC `Stub` 变量名、adapter `_deferred_reports` 业务重放缓存、panel
    `placeholder` HTML 属性/契约预览页、P02 `make_application()` 未组合时 placeholder（有单测
    锁定 fail-closed）、host bridge 头文件两条 P08b-3 旧注释（功能已实现，描述过期，见 §3）。
- **A 真实遗漏：0**。没有发现“应实现/测试但被 TODO/占位掩盖”的代码路径；所有未实现能力均能
  对应已声明的 target 契约（B）或已记录的部署/用户决策（C）。

## 2. 分类统计

| 分类 | 数量/范围 | 代表项 |
|---|---|---|
| A 真实遗漏 | **0** | — |
| B 契约已声明未实现 | target 未翻：messages 228、fields 753、operations 59、channels 8、stream_events 7、host_operations 7、enum_values 115、semantic_rules 18；OpenAPI 按 owner-rpc 规则保持 not_implemented 的 REST 同样属 B | `listActionMetadata`、host operation owner、`bootstrap_credential` 字段级、P05b instance delegation、WS 频道、runtime storage unary 等；见 doc/01、doc/06、P05f/P12f/P13f 报告 |
| C 环境/用户决策 | 约 20 组 | Docker/Compose、两主机与断网、远端 CI、release/GHCR/签名、真实 QQ/NapCat、真实 Provider、KMS/保留期/离站、P13 生产 participant 授权、P12 host operation、P05b 委托语义 |
| D 误报/无害 | 20 条 TODO 字面 + 约 40 条占位/Stub/预览命中 | `mktemp XXXXXX`、`stub_`、`_deferred_reports`、panel `placeholder`、P02 未组合 placeholder、host bridge 旧注释、panel 契约预览/not_implemented 文案 |

## 3. 残余缺口与建议

1. **host bridge 头文件旧注释（D，建议后续清理）**：`core/include/kiarabot/core/runtime/platform/host_bridge_server.hpp:25`
   仍写“P08b-3 command/receipt/observation pipelines are not implemented yet”，P08b-3/P08b-3a/
   P08b-3b 已合入并接线（runtime_composition 设置 HostBridgePersistence）；属过期注释，不是功能
   遗漏。同一文件 line 65 的 `until P08b-2c PG wiring lands` 也需按实际实现核对措辞。
2. **Panel 历史文案（D）**：`panel/src/views/p12-live.tsx`/`actions.tsx`/`App.tsx` 仍有
   “P12c 后端未实现/action 查询尚未接线”等旧句；其中 action REST OpenAPI marker 仍按 owner-rpc
   规则 not_implemented，但后端转发已实现。属 UI 文案与契约 marker 分层，建议由 Panel 任务
   统一核对，不属本审计的 A 类实现遗漏（且 panel 不在本任务 write scope）。
3. **SDK 文档历史声明（D/C）**：`sdk/python/README.md`/`sdk/PLUGIN_SDK_DEPENDENCIES.md` 列出的
   Host 受控启动、native/wasm、SDK 动态订阅等仍是未实现能力；对应阶段/决策已记录，保持 C。
4. 建议后续任务：清理上述 D 类过期注释/文案；B 类按契约任务逐项翻转；C 类等待用户环境/决策。

## 4. doc/23 验证点抽样 10 项与证据一致性

证据核查方式：本机 main@23aa6ab 的 CTest 名称清单（148 tests）与 `reports/modules/*` 文件存在性；
未改任何人文档。结论：8 项一致，2 项存在**口径夸大/遗漏**（见下表“判定”）。

| # | doc/23 验证点（摘要） | 声称证据 | 实际证据 | 判定 |
|---|---|---|---|---|
| 1 | P04 真实 Identity DB/刷新/撤销/密钥/证书/依赖分区 | 报告 p04 + p04_identity_* | `p04_identity_application`、`p04_identity_*`（28 项）注册存在；`architecture-p04-identity.md` 存在 | 一致（并发压力仅部分测试覆盖，报告已列本机证据边界） |
| 2 | P05 真实 Identity 授权链/两进程 mTLS/PG | p05_runtime_identity_e2e | CTest 存在；P05f 记录真实双进程矩阵 | 一致 |
| 3 | P06 外部 Bridge/ACK/租约/重投 | p06_bridge_acceptance | CTest 存在；报告存在 | 一致 |
| 4 | P07 配额/幂等/执行证据/迟到结果 | p07_postgres_action_records | CTest 存在；报告存在 | 一致 |
| 5 | P08 Host 注册/命令/receipt/观察 | p08_postgres_host_registration、p08_host_* | CTest 存在（另有 p08_managed_host_e2e/p08_host_bridge_e2e）；报告含未退出项 | 一致（P08 报告明确未退出） |
| 6 | P09 PG CAS/配额/TTL/共享 namespace | p09_postgres_storage_records | CTest 存在；报告存在 | 一致 |
| 7 | P10 timer 崩溃/丢确认/取消/DST/重复恢复 | p10_timer_e2e | CTest 存在；报告存在 | 一致 |
| 8 | P11 Model 两事务/UNKNOWN/密钥/本地 HTTP provider | p11_model_*、p11_control_model_rest | CTest 存在；报告列 P05b 单链路缺口 | 一致（单链路缺口已声明） |
| 9 | P12 真实服务 HTTP/**WS** + Panel live 覆盖所有批准写入口/断流 | p12j_panel_live、P12f 矩阵 | `p12j_panel_live` 存在且通过；但 target Control v1 **无 WS**（P12a-2 延期）、Panel live 为 native HTTP matrix 非浏览器 WS；`listActionMetadata`/部分写入口仍 not_implemented | **部分夸大/遗漏**：doc/23 P12 验证句需限定 REST/HTTP，WS 与“所有批准写入口”不成立 |
| 10 | P14 两独立 Linux 主机网络分区/TLS/撤销/重连 + RPO24h/RTO4h 实测 | P14f native、P14c 演练 | `p14_native_single_host` 存在；P14f/P14d/P14c 明确两主机延期、P14c 仅本地小数据（RPO=0s/RTO≈4.6–12.1s）；final 148/148 是 CTest | **遗漏/未达成**：doc/23 P14 退出条件与 §6 “跨主机及协调备份恢复演练有实测证据”仍不成立，需继续记录为环境项 |

补充：§6 最终验收清单中 “tidy/边界/API 等完整覆盖” 与 “跨主机/协调备份” 未在本机 148/148
CTest 中全部覆盖；本报告只记录差异，不改 doc/23/其它人文档。P13 退出在 P13f 已明确“未通过”并
列用户决策项，不存在夸大。

## 5. 需 Lead/用户决策项

- P13 生产授权语义（A/B/C 备选，P13f 已列）与是否接受 C test-only 证据。
- P05b Runtime 实例委托语义（决定 P11 单链路/P13 participant 生产接线）。
- P12 host operation owner、`bootstrap_credential` 字段级是否继续保持 not_implemented。
- doc/23 P12/P14 验证口径是否修订为“REST/HTTP + 本地 CTest”并保留 WS/两主机为环境项。
- 是否安排 D 类过期注释/Panel/SDK 文案清理任务。
