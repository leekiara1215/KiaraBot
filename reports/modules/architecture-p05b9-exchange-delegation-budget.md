# P05b-9：production Runtime ExchangeDelegation budget 根因修复（2026-11-03）

- 基线：main=`b0386bb`；worktree `.worktrees/p05b9`。
- 症状（build-deploy P05b-4）：production Runtime 用 sealed model envelope 调
  `ExchangeDelegation(audience=model, RESOURCE_MODEL, model.chat)` 返回 gRPC 16
  UNAUTHENTICATED / 内层 InvalidArgument；同一 sealed parent + 同一 Runtime 叶由测试侧直连成功。
- 复现材料：`/tmp/p05b9-handoff`（sealed envelope、runtime.json、runtime/control leaf、Identity
  audit、e2e failure log、runtime provider trace）；失败 stack 在
  `/Users/betta/Workspace/KiaraBot/.worktrees/p05b4`（codex/p05b4）。

## 1. 排除项（实测）

Identity RPC 前置诊断（临时插桩，未提交）显示 production 请求：
- `peer_cert == request.client_certificate_sha256`（相等=1），peer 为 Runtime platform peer；
- audience=model、resources=1、permissions=1、budget_ok=1，context request/trace/idempotency
  均合法、deadline 存在；
- Identity `audit_entries` 无 ExchangeDelegation 记录，说明请求在 audited application 之前被拒。

因此 wire、sealed envelope、client 证书/SAN/链均不是根因。

## 2. Root cause

Runtime `RuntimeIdentityClient` 同时承载热路径 `CheckAuthorization` 和
`ExchangeDelegation`。生产 creation 使用热路径预算 **500ms/最大1s**；`exchange_delegation`
沿用了该预算。Identity ExchangeDelegation 需要两次在线授权（parent/source + child/final）及签名，
在 production 负载下第二次授权时请求 deadline 已耗尽；Identity RPC 在进入 application 前拒绝
（InvalidArgument/UNAUTHENTICATED，且不写 ExchangeDelegation audit）。测试侧直连给了长 deadline
（20s），因此稳定成功。

build-deploy 临时把 exchange 预算单独放宽到 2s/5s 后，single-chat 与完整
`p05b_model_chain_e2e` 正向/负向矩阵立即通过，确认预算即为根因。

## 3. 修复

`core/include/kiarabot/core/runtime/platform/runtime_identity_client.hpp`：

```cpp
std::chrono::milliseconds delegation_budget{2000};
std::chrono::milliseconds delegation_maximum_budget{5000};
```

- `exchange_delegation` 的 gRPC deadline 改用 `delegation_*`；`CheckAuthorization` 保持
  `budget/maximum_budget`（500ms/1s）热路径预算。
- `create()` 校验两个 delegation 预算均 >0、`delegation_budget <= delegation_maximum_budget`、
  maximum <=30s；非法配置 fail-closed。
- 无 proto/RPC/权限/配置字段变更；生产 composition 不需要额外配置，默认值生效。

## 4. 测试/证据

- `p05_runtime_identity_client`：fake Identity Exchange 延迟 700ms（>500ms 热路径预算）仍成功；
  0/倒置/60s delegation 预算创建失败；既有 CheckAuthorization/Exchange wire 断言保持。
- build-deploy 验证版（同一设计）：`p05b_model_chain_e2e` 正向 matrix（success + provider
  401/429/500/timeout/invalid_json + restored success）与 fail-closed fixture 通过，输出
  “P05b model single chain: plugin->Runtime->Model->real HTTP provider passed”。
- 回归门禁：全量构建；`ctest -R 'p05|p10|p11|p12|p03_postgres'`；contract 门禁；format16。

## 5. 解锁

- P05b-4 可 rebase 本提交后跑 managed Host+SDK plugin 正/负矩阵、激活 CTest 并完成报告。
- Runtime 生产 Identity client 的 Exchange 超时不再受 500ms 热路径预算影响；CheckAuthorization
  热路径预算/失败语义不变。
