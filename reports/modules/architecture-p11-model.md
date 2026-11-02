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
