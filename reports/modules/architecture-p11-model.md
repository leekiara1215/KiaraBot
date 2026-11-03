# P11 Model 管理面（P11d）模块报告

## 范围

P11d 只补 Control → Model 管理面的 REST 转发、契约 target 翻转与证据，不重写 Model 业务规则。通用 Control
owner gateway/委托/错误映射沿用 P12c；本任务只新增 Model owner 端口与 `/api/v1/models/*` 路由。

## 交付

- `control/api/{include,src}/target/owner_gateway.*`：新增 `ModelManagementPort` 与 8 个 Model 路由
  （Update/Disable/Enable/RotateProviderSecret/GetModelConfiguration/GetCallStatus/ResolveModelCall/
  GetOperation 的 model owner 分支）。REST↔typed RPC 转换、snake_case 投影、单模型 scope 校验、稳定错误
  映射；密钥永不回显；`secret_version` 作为独立字段返回（target schema 同步补列）。
- `owner_clients.*` / `identity_auth_client.*` / `https_endpoint.cpp`：可选 `control_api.model_endpoint`
  的真实 mTLS Model stub 与 8 个 owner 调用；未配置时 Model 路由 503。
- `service_config.*`：`ControlApiMaterial.model_endpoint` 可选配置、校验与解析（与 runtime_endpoint 同风格）。
- `control/api/tests/owner_gateway_test.cpp`：Model 管理面单测（成功/委托权限与单 scope/密钥不回显/
  CONFLICT→409/UNKNOWN 查询/Resolve/GetOperation/未注入 owner→503）。
- 契约：ModelService 11 方法 target status 翻 implemented；25 messages、84 fields、14 enum values、
  7 semantic rules 按 P06 证据翻转；OpenAPI 新增 disable/enable/getModelCallStatus/resolveModelCall
  4 个 target REST operation（仍 not_implemented 登记）、model_management 策略与 secret_version schema。
- 文档：doc/01 状态表、doc/06 Control Model 管理面、doc/08 model_endpoint 运维说明、doc/22 事务边界。

## 验证

- full-locked 全量构建成功；`ctest -R 'p11|p12|control|p04_control|p02'` 通过（p02_service_config 与
  p04_control_auth_https 在并发负载下各自首跑出现一次时序 flake，单独重跑通过；非断言缺陷）。
- contract 门禁待随最终 commit 一起跑（check_contracts/target_contract/target_api/redocly/asyncapi）。
- clang-format 16 与 git diff --check。

## 与 P12c 的差异

- 复用 P12c `OwnerGateway` 路由框架、`DelegationPort`、owner error mapping、HTTPS boundary 和测试结构；
  未新增通用网关层。
- P12c 只有 Runtime Action 查询；P11d 扩展 Model owner 端口和 8 条 Model 路由，并在 `OwnerGateway`
  构造器可选取注入 Model 端口（既有 P12c 调用保持源码兼容）。
- Control 配置新增可选 `control_api.model_endpoint`；P12c 的 runtime_endpoint 语义不变。
- REST operations 的契约状态仍为 not_implemented（与 P12c Action REST 一致）；P11 **owner RPC/消息/字段/
  语义** 已翻转，待 P12c-2 统一 REST 翻转时再登记 operations。

## 未完成

- Model 元数据 GET/List REST 转发（GetModelMetadata/ListModelMetadata）未接线，仍返回 503；
- GetOperation 只支持 `owner=model`，Runtime/Scheduler operation 路由保持 CAPABILITY_NOT_SUPPORTED；
- 未做 Control 进程级 TLS+Identity fake 的端到端 REST 夹具；本次为 gateway 单测 + 既有 Control HTTPS 测试；
- 未运行真实 Provider 外部联调（与 P11b 记录一致）。

## P11d-2 Model metadata/list、统一 operation 与真实 Control e2e

- Control gateway 新增 `GET /api/v1/models/{modelId}`（GetModelMetadata，model.read + 单模型 scope）与
  `GET /api/v1/models`（ListModelMetadata，要求一个或多个 RESOURCE_MODEL scope）；响应只含脱敏 metadata。
- `/api/v1/operations/{owner}/{id}` 统一 owner 分支：`model` 转发 ModelService.GetOperation；其他 owner
  的 Control 适配器未接线时稳定返回 `CAPABILITY_NOT_SUPPORTED`，并在所有 owner 上先做 GET/HEAD 方法校验。
- 新增真实 Control 进程 REST e2e：`tests/architecture/p11_control_model_rest.py` 复用 P04 的临时 PKI，
  启动真实 Control + Identity fixture + Model fixture（mTLS），覆盖 metadata/list/update/disable/enable/
  secret rotation（密钥不回显）/configuration/call UNKNOWN/resolve/operation，以及未接线 owner 的 503。
- target 翻转：9 个 Model REST operations 标 target implemented（证据含上述 e2e）；`operations.getOperation`
  仍 not_implemented（只有 model owner 分支接线，runtime/scheduler 未接线）。
- 未翻转：插件 `HostRequest.model_chat` 的 target 消息（ModelChatRequest/Response/ModelMessage）仍
  not_implemented——当前无真实 PluginBridge+Model+Identity target fixture 覆盖该 ingress，P11c 的插件
  chat 证据来自 reference 测试，不足以按 P06 规则认证 target；待后续 bridge fixture。

## P11f Model 退出证据矩阵（2026-11-02）

- 基线：worktree `.worktrees/p11f`，branch `codex/p11f`，HEAD `95495c2`（本地 main；origin/main 记录为 `a056942`，本次未切换基线）。**不修改实现/契约**；本段只记录实测、状态核对与缺口。
- 构建：main build `build/full-locked` 增量重建成功（`cmake --build build/full-locked --parallel 3`，exit 0）；无 C++ 源码改动。

### 已实测命令与结果

| 命令 | 结果 |
|---|---|
| `ctest -R 'p11' -j1 --output-on-failure`（p11_model_gateway、p11_model_gateway_application、p11_model_http_provider、p11_model_secret_crypto、p11_model_identity_client、p11_runtime_model_client、p11_model_bridge、p11_model_rpc、p11_control_model_rest） | **9/9 passed**（总 29.7s；control_model_rest 15.3s，bridge 3.5s，其余 0.01–2.8s） |
| `ctest -R 'p03_postgres|p03_service_database' -j1 --output-on-failure` | **2/2 passed**（p03_postgres_database 25.0s、p03_service_database 19.2s；真 PG16.4 fixture） |
| `sdk/python/.venv/bin/python -m pytest -q sdk/python/tests` | **141 passed, 23 skipped**（12.2s） |
| `... pytest -q sdk/python/tests/test_model.py sdk/python/tests/test_model_sdk.py -rs` | **7 passed, 19 skipped**；skip 原因 `deferred: target Model e2e needs P06a Bridge handler + P08 managed Host`（test_model.py 的 target Model/Provider e2e 参数化用例） |
| `python3 proto/tests/check_implementation_status.py` | **PASS**（target production 状态/字段/枚举/operation 与 doc/01/证据核对一致） |
| `ctest -R p01_target_roundtrip` | **FAIL（与本任务无关的契约镜像缺陷）**：`AssertionError: ('ListAudit', ['RESOURCE_CATALOG'], [])`，当前 main 的 P12c-6 ListAudit scope_kinds 与 target_spec 不一致；未修改 proto/target_spec，已上报 Lead。 |

### target/evidence 状态核对

- ModelService 11 个 owner RPC 在 target_spec 中为 `implemented`；P11 全套测试覆盖 gateway/application/http provider/secret crypto/identity client/runtime client/bridge/rpc/control REST。
- `ModelChatRequest/Response/Message`、`HostRequest.model_chat`、`HostResponse.model_chat` 及字段级证据由 P11e bridge fixture 覆盖（doc/01 既有行）。
- Model 管理消息/字段（metadata、configuration、call record、disable/enable、resolve、getOperation 等）在 **target production effective status** 中为 implemented（`check_implementation_status.py` PASS），但 proto 内联 `CONTRACT_STATUS` 仍保留历史 `not_implemented` 注释；这是 registry/doc override 与源注释的历史差异，不改变本次验收结论。
- **OpenAPI Model REST 不一致**：`operations.{getModelMetadata, updateModel, rotateProviderSecret, getModelConfiguration, disableModel, enableModel, getModelCallStatus, resolveModelCall, listModels}` 在 `api/openapi.yaml` 仍标 `x-contract-status: not_implemented`，而 P11d-2 e2e 已证明 Control 转发可用（`p11_control_model_rest` 通过）。本次不修改契约；列为需 P12c/P11 契约清理复核项，不能据 REST inline 状态判定 owner 实现不存在。
- `operations.getOperation`：Control 只接线 `owner=model` 分支；runtime/scheduler 未接线（稳定 CAPABILITY_NOT_SUPPORTED/503）。OpenAPI 仍 not_implemented；多 owner 统一 operation 未验收。

### 未翻项/不验收项与理由

- **ModelChatDisposition/ModelCallDisposition 无线上暴露**：disposition 目前是 owner 内部/管理投影事实，**没有插件/SDK 可消费的线上字段或真实 Provider 场景证据**；不把枚举值翻为客户端可见能力，也不以单元枚举断言替代线上暴露验收。
- **streaming 明确不做**：HTTP provider 固定 `stream=false`，proto/model API 无流式字段；流式 Provider、增量响应、取消/背压均未实现，列 P11 外。
- **GetOperation 多 owner**：只完成 `owner=model`；Runtime/Scheduler operation 路由未接线，`CAPABILITY_NOT_SUPPORTED` 不是完整 GetOperation 能力。
- **插件 `HostRequest.model_chat` target 证据**：P11e fixture 已覆盖 bridge ingress；但真实插件→Runtime→Model→Provider 的**单链路**未合入（P05b 委托语义待用户确认），不能把分段证据当作单链路验收。
- **模型管理 REST inline 状态**：见上，OpenAPI 标记与 e2e 不一致，待契约清理，不在本任务改契约。

### 剩余缺口（已实测/待环境）

- 已实测：Model owner RPC/应用/PG/HTTP provider/secret AEAD/Identity caller/Runtime client/PluginBridge fixture/Control Model REST；真 PG16.4 p03 fixture；SDK 非 target model 单测。
- 待环境：真实外部 Provider 付费联调（T07b/用户环境，未执行每次调用与计费核对）；真实插件→Runtime→Model→Provider 单链路 e2e（P05b 委托审批/用户确认）；readiness composition 级真实 TLS harness（当前为组件/单进程 fixture）；目标六服务栈真实 live（本机未启动全目标栈）。
- 需 Lead/契约 owner 复核：P12c-6 ListAudit target_spec 与 proto scope_kinds 不一致导致 p01_target_roundtrip 红；OpenAPI Model REST 的 inline not_implemented 与 P11d-2 e2e 证据不一致。

### P05b-4 单链路准备与负例 e2e（2026-11-03）

- 真实本地 HTTP Provider fixture（`tests/architecture/p05b_provider_fixture.py`，CTest
  `p05b_provider_fixture`）：鉴权、stream=false、usage、400/401/404/429(Retry-After)/5xx/
  timeout/非 JSON/坏 choices/tool_calls 等 14 modes，self-test 16 checks 通过。
- Runtime→Model delegation provider（sealed parent file → Exchange hops2 → Model）已实现；
  HostBridge/PluginBridge 通过 `host.model.v1` feature advertise 解锁 model_chat。
- **真实负例 e2e**（`p05b_model_chain_fail_closed_e2e`）：真实 Identity/Runtime/Model + 真实
  SDK plugin over TLS PluginBridge + 真 Provider fixture；无 sealed 父委托时 plugin 收
  UNAUTHENTICATED(2)，Provider requests=0（通过）。
- 正例 e2e（managed Host+SDK plugin+真实 Provider 成功/撤销/过期/跨实例/错误证书）在
  P05b-6 sealed 文件 writer/刷新合入前 **gated skip**；不把负例/分段证据当单链路验收。
- 发现契约缺口：Model `UpdateModel`/`RotateProviderSecret` 缺 proto `scope_kinds`，真实
  Control 管理 RPC 无法通过 Identity CheckAuthorization；e2e 暂用 test-only seed helper 写入
  真实 Model DB，契约缺口上报。
