# P05b-4：Runtime → Model 实例委托 provider 接线设计（待 P05b-2 合入后实现）

- 日期：2026-11-03
- 状态：**完成**。P05b-7/P05b-8 + P05b-9 已合入/rebase；真实 managed Host + 受管 SDK plugin
  正链与 Provider 错误矩阵、fail-closed e2e 均通过；CTest `p05b_model_chain_e2e` 已激活。
- 基线：main=`ea318d5`；关联：`reports/analysis/p05b-instance-delegation-proposal.md`
  §7 P05b-4、`reports/modules/architecture-p11-model.md`、`architecture-p05-runtime.md`。
- 非目标：本文不改实现在线行为；proto/权限不变；P05b-2 的签发接口语义由 task-144 决定。

## 1. 当前链路与缺口（代码事实）

1. 插件经真实 PluginBridge 发送 `HostRequest.model_chat`（`runtime_bridge.cpp` model_chat
   帧），Runtime 记录 instance/installation；
2. Runtime 生产 composition 创建 `RuntimeModelClient`，但注入
   `platform::unavailable_model_delegation()`（`core/app/runtime_composition.cpp:435-446`），
   `chat()` 在调用前取 proof 失败即返回 UNAUTHENTICATED（`runtime_model_client.cpp:98-105`）；
3. `RuntimeModelClientOptions::delegation_provider` 当前是**无参**
   `std::function<Outcome<std::string>()>`；`RuntimeModelChatRequest` 已带 `instance` 字段
   （`core/include/kiarabot/core/ports/target/runtime_model.hpp:15-21`），因此 provider 必须能从
   请求拿到 instance，现接口需扩展；
4. 冻结下游要求：Model audience=model、`business_hops=2`、actor_chain=[Control,Runtime]、
   direct_actor=Runtime、subject=插件实例 principal、client_certificate=Runtime 真实叶、
   ≤56/60s TTL，再在线 `CheckAuthorization(/Chat,{Model,model_id})`，owner=subject
   （P11 契约/`model_identity_client.cpp`）；Runtime 侧只能通过 Identity
   `ExchangeDelegation`（现有 RPC，父委托 audience=runtime/hops=1）获得该子委托；
5. `ExchangeDelegation` 要求：调用方真实 Runtime platform peer；`client_certificate_sha256`
   与已登记 Runtime 叶指纹逐字节一致；parent proof 仅可见 ASCII；请求 permissions 是父集合
   子集且原 subject 仍有精确授权（`identity_rpc.cpp:1408+` / `identity_exchange_application.cpp`）。
6. P05b-2 未合入前不存在 subject=插件实例的父委托；当前 fail-closed 是正确行为。

## 2. 目标组件（P05b-4 实现范围）

### 2.1 RuntimeModelClient 接口扩展

`RuntimeModelClientOptions::delegation_provider` 改为：

```cpp
using ModelDelegationProvider =
    std::function<target::Outcome<std::string>(const target::RuntimeModelChatRequest&)>;
```

- `chat()` 调用 `options_.delegation_provider(request)`，保留现有 deadline/冲突语义；
- `unavailable_model_delegation()` 返回同签名 fail-closed 函数，现有测试/组合根默认路径不变；
- 不把 proof 写入日志/审计/outbox/operation；仅请求内存（P05 约定）。

### 2.2 父委托来源端口（依赖 P05b-2 契约）

新增 Runtime 组合内部端口（不跨服务协议）：

```cpp
struct RuntimeInstanceDelegationContext {
  target::InstanceId instance;
  target::InstallationId installation;  // 从已认证 Bridge session 派生，不信任 wire
  std::string model_id;
  target::TimePoint deadline;
};
struct RuntimeParentDelegation {
  std::string proof;          // 短期父委托，≤56s
  target::TimePoint expires_at;
};
class RuntimeInstanceDelegationSource {
 public:
  virtual bool available() const noexcept = 0;
  virtual target::Outcome<RuntimeParentDelegation> acquire(
      const RuntimeInstanceDelegationContext&) = 0;
};
```

- P05b-2 合入后提供实现（B1：Control 管理员触发签发；具体是 Control 下发、Identity
  受控签发接口、或 sealed 部署材料由 task-144 定稿）。Runtime **不自行伪造 subject**、
  不接受 wire 自报 instance；
- 若 task-144 以 Identity 受控签发暴露给 Runtime 平台 peer，则 source 用 mTLS 调
  Identity；若以 Control 下发材料暴露，则 source 只读取受控短期材料并做完整性/绑定校验。
  两种实现都必须保证：subject=插件实例 principal、audience=runtime、scope 包含
  model resource id（供 exchange 收窄）与 installation+instance（跨安装拒绝依赖）。

### 2.3 ModelDelegationProvider（core/src/runtime/platform）

新类（建议 `instance_model_delegation_provider.{hpp,cpp}`）：

1. `delegation(request)`：
   - 以 `(instance, model_id)` 为 key 查缓存；缓存项含 child proof/expires_at；
   - 缓存有效且离到期还有 `refresh_margin`（建议 8s，且 child TTL≤56s）→ 直接返回；
   - 否则 single-flight 获取：`source.acquire(context)` → 校验 parent 非空/未过期/
     scope 含 Model 与 Installation+Instance → 调 Identity `ExchangeDelegation`：
     context 用 UUID request/trace/deadline，`parent_proof`、audience=model、
     scope={RESOURCE_MODEL, model_id}、permissions={PERMISSION_MODEL_CHAT}、
     `client_certificate_sha256`=本进程 TLS 叶 DER 的 SHA-256（启动时从证书计算）；
   - 校验 response proof 非空/≤32KiB/audience=model/hops=2/client_certificate=Runtime 叶/
     expires_at>now；缓存后返回；
   - 失败：不写入缓存；返回稳定失败（Identity 不可达=UPSTREAM_UNAVAILABLE；拒绝=
     PERMISSION_DENIED；无 proof=UNAUTHENTICATED），不重试出界，不调用 Model。
2. 缓存：仅内存、按实例/模型分 key、容量有界（如 256）、过期清理；**不落盘**、
   不写日志；撤销/过期依赖短 TTL + Model 在线 CheckAuthorization；父委托撤销在下一次
   refresh 生效。
3. 并发：单飞锁避免 per-request 抖动；deadline 取请求 deadline 与预算较小者；
   Runtime 停机时清空。

### 2.4 生产 composition

- 配置新增（示例，最终字段以 task-144 契约为准）：
  ```json
  {
    "identity_endpoint": "...",
    "model_endpoint": "...",
    "instance_delegation": {
      "source": "control_mediated|identity_issued",
      "refresh_margin_seconds": 8,
      "cache_capacity": 256
    }
  }
  ```
  秘密材料只经部署文件/受控 API，不进入 argv/日志；非法配置启动失败（与现有配置语义一致）。
- `RuntimeModelClientOptions::delegation_provider` 接 `ModelDelegationProvider` 的
  `delegation`；缺 `identity_endpoint`/source/不得启动 provider，保持
  `unavailable_model_delegation()` fail-closed。
- readiness：Identity channel READY 且 source available 才把对应依赖标记 Available；
  provider 获取失败不得把 Runtime 标记 ready 的“模型调用可用”（保持现有
  `model_client` 注入才构造 bridge 的语义）。
- 配置/运维文档：doc/08 增补 `instance_delegation` 配置、撤销/轮换、指标（issue/refresh
  失败、provider fail-closed 计数、缓存命中/过期），不写 proof/secret。
- 无 proto/权限变更；Target status 不翻转，除非 e2e 证据通过。

## 3. 失败语义（必须测试）

| 场景 | Runtime bridge 返回 | Provider/Model 行为 |
| --- | --- | --- |
| P05b-2 未合并/父委托不可得 | UNAUTHENTICATED（稳定、不可重试） | 不调用 Model；Provider fixture requests=0 |
| Identity 不可达/Channel 未 READY | UPSTREAM_UNAVAILABLE | 不调用 Model；可重试语义 |
| Exchange 拒绝（权限/scope/证书） | PERMISSION_DENIED | 不调用 Model；记录稳定原因分类 |
| 父/子委托过期 | UNAUTHENTICATED（下次刷新前） | 不调用 Model；刷新成功后恢复 |
| 跨实例请求 | NOT_FOUND/PERMISSION_DENIED | 不调用 Model；instance 派生自 bridge session |
| Model 返回 UNAUTHENTICATED/PERMISSION_DENIED | 原样稳定投影 | 不泄漏 Identity 细节 |
| Provider 401/403/404/400/429/5xx/timeout/非 JSON | Model 现有映射保持（P11b） | 重试/UNKNOWN 语义不变 |
| Runtime/Identity 日志 | 无 delegation proof/secret | 仅稳定码/计数 |

## 4. 测试设计

1. **C++ 单测**（`core/tests/platform/model_delegation_provider_test.cpp`）：
   - fake Identity Exchange/CheckAuthorization gRPC server + fake source；
   - 正例：parent→exchange→child 缓存、TTL/refresh margin、单飞并发、deadline；
   - 负例：无 parent、parent 过期、错误 scope/permission、证书不匹配、Identity 不可达、
     exchange 拒绝、缓存过期后刷新、不得调用 Model。
2. **Provider fixture**（本轮已准备）：`tests/architecture/p05b_provider_fixture.py`，
   本地真实 HTTP server，覆盖鉴权/stream=false/usage/400/401/404/429/5xx/timeout/非 JSON；
   `--self-test` 已可运行（CTest `p05b_provider_fixture`，label `p05b;p11;unit`）。
3. **P11 单链路 e2e**（P05b-2 合入后）：`tests/architecture/p05b_model_chain_e2e.py`
   真实 Identity/Runtime/Model + 真实 managed plugin（P08e harness）+ 真实 Provider fixture；
   断言正向结果、call record/usage owner=实例、撤销/过期/跨实例/错误证书/Identity 离线/
   Provider 错误矩阵；不把 fake port 计入验收。
4. **回归**：`p11_model_bridge`、`p11_model_http_provider`、`p11_control_model_rest`、
   `p05_runtime_identity_e2e`、`p08_managed_host_e2e`、`p03_postgres` 全绿。

## 5. 实施步骤（P05b-4 正式阶段，待 Lead ping）

1. 基于 P05b-2 合入后的 main rebase `.worktrees/p05b4`；
2. 实现 §2 组件与 composition/config/readiness；
3. 补 C++ 单测 + provider fixture e2e + P11 单链路 e2e（真实 managed plugin/Provider fixture）；
4. `p11` 标签/CTest、全量构建、`ctest -R 'p11|p05|p08|p06|p03_postgres'`、contract 门禁、
   format16、doc/03/05/08/22 + P11/P05 报告；
5. 3–5 commit，报告 SHA/e2e 矩阵/未完成项。

## 6. 依赖状态（截至本准备 commit）

- **P05b-2 / task-144：未合入**（ref-infra 并行）。因此：
  - 正向链/production composition 未接线，禁止跑/断言正向生产链；
  - e2e harness 已注册但 `expect-fail-closed` 模式在依赖缺失时 exit 77（CTest skipped，
    `SKIP_RETURN_CODE 77`），不会产生 fake pass；
  - provider fixture 自测与 fail-closed 断言已就绪，作为本轮可运行证据。
- P05b-4 正式实现开始条件：P05b-2 合入 + Lead ping。

## 7. 实现进展（2026-11-03，用户选 B / P05b-6 前）

用户选择 **B：sealed 短期文件 + Control/部署刷新**（无 proto 变更）；P05b-6 由 ref-infra 并行
实现 writer/刷新。本分支已完成：

- `InstanceParentDelegationSource` sealed-file reader（0600/属主/lstat 非符号链接/≤256KiB/
  schema/instance/subject/TTL≤56s/过期拒绝）；
- `InstanceModelExchangePort`（真实 Identity ExchangeDelegation mTLS；Runtime 叶指纹；
  audience=model/hops2 校验）；
- `InstanceModelDelegationProvider`（有界缓存/refresh margin/single-flight/fail-closed）；
- RuntimeModelClient per-request delegation provider 签名；
- 生产 composition `instance_delegation` 配置接线、`host.model.v1` feature advertise；
- test-only Model DB seed helper（因 `UpdateModel`/`RotateProviderSecret` 缺 scope_kinds，
  真实 Control 管理链路仍不可用，该缺口已上报，未改 proto）；
- **Identity 最小修复**：纯 Model 权限实例父委托允许无 RESOURCE_INSTANCE binding（之前
  scope×permissions 交叉积使 model-only 父委托不可签发；subject 仍由 instance_id 唯一 plugin
  principal 解析；Scheduler 父委托要求不变），需 P05b-2 owner/契约文档复核。

**e2e 状态**：负例 `p05b_model_chain_fail_closed_e2e` 已通过（无 sealed 父委托 →
UNAUTHENTICATED(2)、Provider requests=0）。正例保持 gated skip（`p05b_model_chain_e2e`，
SKIP_RETURN_CODE 77），等 P05b-6 合入后去掉 gate 并跑 managed Host+SDK plugin+真实 Provider
成功/撤销/过期/跨实例/错误证书矩阵；不伪跑正向链。


## 9. 当前实现状态（2026-11-03 13:15，WIP）

- 分支 codex/p05b4 已 rebase main d875837；删除本分支重复 Model sealed reader/provider，
  生产 composition 使用共享 `SealedInstanceDelegationSource` +
  `RuntimeInstanceDelegationProvider::model_chat_delegation`。
- 真实正链已到 PluginBridge→Runtime→Model→HTTP Provider（Provider 记录 plugin 请求）；
  修复 SDK model_chat 响应误读、Model.Chat trace/idempotency、插件 profile installation 权威
  来源三个真实缺陷。
- 阻塞：Runtime 进程发起 Identity.ExchangeDelegation 时 Identity 返回 InvalidArgument
  （gRPC UNAUTHENTICATED；audience=model、单 RESOURCE_MODEL、model.chat），Identity
  audit 无 ExchangeDelegation 记录；同一 sealed envelope + 同一 runtime leaf 由测试直连则
  成功。最小复现、envelope、证书指纹、配置与 Identity audit 已交 P05b-9/core-target。
- 待 P05b-9 修复合入后：rebase、managed Host+SDK plugin 正/负矩阵、激活 CTest 证据、
  doc/03/05/08/22+P11 报告、全量构建与 p11/p05/p08/p06/p03_postgres 门禁。


## 10. 完成状态（2026-11-03）

- rebase main b0386bb，cherry-pick P05b-9 66587e9（ExchangeDelegation 独立 2s/5s 预算）
  与 ddf643d；生产 composition 重接模型 sealed provider、per-request delegation、`host.model.v1`。
- e2e 已使用真实 `kiarabot-plugin-host` 管理 SDK plugin：Host start command receipt 与
  SQLite journal 断言通过后执行 model_chat；success（Model call record owner=实例 principal、
  无 provider secret/usage 正确）+ Provider 400/401/404/429/500/503/timeout/invalid_json +
  expired/cross-instance/forged-signature/revoked-parent + Identity/Runtime 不可达 fail-closed
  + restored success 全绿；无 sealed 父委托 fail-closed code=UNAUTHENTICATED、Provider 0 请求。
  未覆盖：替代 Runtime 叶证书的错误证书场景（fixture 单 Runtime peer，已覆盖 forged
  signature/错误 origin 的在线拒绝）。
- CTest：`p05b_model_chain_e2e`、`p05b_model_chain_fail_closed_e2e`、`p05b_provider_fixture`
  全 PASS；未覆盖付费外部 Provider/目标六服务栈 live（环境项）。
