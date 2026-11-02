# P05 Runtime 资源、绑定与当前代次（第一阶段）

基点：`main`（P06/P07 契约前快照）+ P05 分支 `codex/p05-runtime`。本报告只描述本分支已实现
并实际验证的范围；未接线部分见“未完成/降级项”。受保护 doc/18 未修改。

## 目标模块与允许修改范围

- Core：`core/include/kiarabot/core/{domain,application,ports}/target/`、`core/src/application/target/`、
  `core/src/runtime/platform/`、`core/tests/architecture/`、`core/app/runtime_*`。
- 基础设施：`infrastructure/postgres/{schema,src,tests}`（Runtime 私有库）。
- 构建：`core/CMakeLists.txt`、`cmake/platform.cmake`。
- 文档：本报告、`doc/02-core.md`。契约 `proto/`、`api/` 未修改（目标状态仍为 not_implemented，
  原因见下）。

## 输入输出与责任边界

Runtime 应用层接收类型化命令上下文（认证上下文、请求/链路 ID、幂等键、fingerprint、deadline、
expected_version）与命令载荷，返回类型化记录或稳定 `Failure`。业务规则全部在
`domain/target/runtime_rules.hpp` 与 `runtime_resource_application.cpp`；PG 仓储只执行
owner 本地事务；gRPC 适配层只做协议转换、deadline/幂等键/请求 ID 校验与稳定错误映射，不接受
请求自报身份。Control/Identity 不进入 Runtime 私有库。

已实现能力：账号 Create/Update/Disable/Get/List；显式 Bind/Unbind；Adapter 注册/心跳的
应用接口（供 P06 Bridge 复用）与 Get/List；ValidateManifest、安装 Create/Get/List、实例
Create/Get/List、GetInstanceConfig、SetDesiredState、UpdateConfig；GetResourceState、
GetOperation。9 个 P06 `internal_dispatcher` unary 方法不在 P05 生产传输中实现。

## 错误与状态语义

- 预期版本不匹配不覆盖（CONFLICT/ReadCurrent）；禁用/删除资源与未注册连接 fail closed。
- RegisterAdapter 只在 Runtime epoch 下分配/递增 `connection_generation`；adapter Fence 不填
  `execution_generation`（外部 adapter 无受信进程执行，P08 才建立可信绑定）。旧
  connection_generation 后续帧 CONFLICT，旧 runtime_epoch 帧 UNAUTHENTICATED。
- BindAdapter 必须在 RegisterAdapter 之后；注册只校验声明账号存在/可用/未绑定其他 adapter，
  不自动建绑；声明账号数量有界且去重。
- 每条变更命令：在线授权（decision 的 subject/permission/exact scope/版本/新鲜度必须完全绑定）、
  与权威库时间比较 deadline、同 UoW 提交 receipt/audit/operation；幂等重放返回原快照并恢复原
  `OperationRef`（operation id 取自命令身份）。
- List 是当前权威读取：keyset（`id>after ORDER BY id LIMIT n`）+ 服务端不可变 opaque cursor，
  绑定主体/查询/授权版本/page_size/十分钟窗口；非法、跨主体、跨版本、改页大小则
  CONFLICT(CorrectInput)；依赖或授权故障整页失败，不返回空页/部分页；逐项按 exact resource
  过滤当前授权，catalog 权限不授予 child。跨页不冻结行集（与 Identity session snapshot 不同）。
- Readiness 由组合根按真实观测判定：应用已装配 + Runtime 私有库/schema writer 可用 +
  Identity 当前授权链路可用才 ready=true；未观测保持 Unverified/ready=false。单测 fake 只验证
  判定逻辑，不能使组合进程 ready。

## 持久化

Runtime 私有 PG 库新增 `adapters`、`adapter_bindings`、`hosts`、`instance_configs` 关系与
`result_*` 快照，`installations.manifest`、`instances.desired` 列；共享 `replay_results.kind`
范围扩到 0..18，`ReplayResult` 新类型追加在既有索引之后。该版本只有 v1 空库初始化、无迁移
机制，因此结构变更全部写入同一初始化 schema。使用 PG16.4 临时空库实测并写回：
Runtime descriptor `6cfedcb0c23c88b128986d041e601f44`、relation_count 81、schema version 6；
identity/scheduler/model 因共享 schema 变更同步更新 descriptor 与 relation_count，版本分别
11/6/6。

## 验证

本地固定环境（vcpkg/PG16.4/OpenSSL3.5，`full-locked` preset）：

- `cmake --build --preset full-locked --parallel 3`：通过（运行时告警仅重复静态库链接提示）。
- `ctest --preset full-locked -R 'p05|p02|p03_postgres|p04_identity_rpc|p03_service_composition'`
  ：全部通过。
- 全量 `ctest --preset full-locked`：除环境/负载项外通过。`p01_target_api` 需要
  `npm ci --prefix scripts/ci`（本 worktree 未安装 node_modules，属环境限制）；`p01_target_roundtrip`
  与 `p04_control_auth_https` 在串行全量负载下曾超时/连接计数抖动，单独重跑均通过。
- clang-format 16.0.6 对所有改动 C++ 文件执行；`git diff --check` 通过；未运行 tidy/boundaries
  全量脚本。

## 集成收尾 A：P06 外部 Bridge bootstrap 凭据（Runtime 侧）

rebase 到含 P04/P06 契约的 main 后新增：`RuntimeSecretPort`（32 字节 CSPRNG opaque + SHA-256）、
Runtime 私有 `bridge_credentials` 表（只有摘要、active 唯一索引、撤销标记）、
`BridgeCredentialRepository`/`active_bindings` 仓储。BindAdapter 创建新绑定且该 adapter
instance 无 active 凭据时生成一次并放入 `RuntimeBindAdapterResponse.bootstrap_credential`；
CreateInstance 仅 externally_managed 生成并放入 `RuntimeCreateInstanceResponse.bootstrap_credential`；
明文只在响应中出现一次，解绑最后一个绑定撤销凭据。运行时应用测试覆盖“摘要存储/不重复发放/
撤销”，PG typed 测试覆盖 active/insert-conflict/revoke；schema 重新在 PG16.4 空库实测：
Identity descriptor `0d9975965aa851b433887e949f782167`/129/v13，Runtime
`f3c187c4cb886d70e6ae5f627ffaede6`/84/v7，Scheduler/Model 保持 71/66/v6。

## 集成收尾 B：Runtime 在线授权接入真实 Identity

新增 `RuntimeIdentityClient`（`core/src/runtime/platform/runtime_identity_client.cpp`）：
Runtime→Identity 的 mTLS unary 客户端，实现 `RuntimeAuthorizationPort`（`CheckAuthorization`，
500ms/1s 预算、命令 deadline 取更早、失败 fail-closed）与 PeerAuthorizer（真实 inbound 证书叶
指纹 + 委托公开 claims 结构校验，audience=runtime、有效期≤60s、client_certificate 与叶指纹
一致；公开 claims 只作请求元数据，业务授权仍由在线 CheckAuthorization 决定）。组合根新增
Runtime-only `identity_endpoint` 配置（复用服务 TLS 作为出站 mTLS；缺失时保持 fail-closed 与
ready=false），readiness 依据 channel READY + DB/schema + 应用装配判定。delegation proof 只
驻留请求内存，不进入收据/审计/日志/operation。

测试：`p05_runtime_identity_client` C++ 单测（JWT claims 解码正/负例、fake Identity gRPC server
允许/拒绝/不可达 fail-closed）；`p02_service_config` 覆盖 identity_endpoint 正/负配置。

## 未完成 / 降级项

1. **Identity 真实授权链路未接线**：组合根当前注入 fail-closed `PeerAuthorizer` 与
   `RuntimeAuthorizationPort`（available=false，check 恒 UPSTREAM_UNAVAILABLE），Runtime
   readiness 保持 false，生产 gRPC 不会放行任何请求。需在 P04 `CheckAuthorization`/委托链可用后
   追加真实 Identity client 接线 commit（可用 fake server 验证后切真实 PG/TLS），并补 RPC 传输
   级集成测试。
2. **bootstrap_credential 未生成/返回**：`RuntimeBindAdapterResponse.bootstrap_credential` 与
   `RuntimeCreateInstanceResponse.bootstrap_credential`（externally_managed 一次性 opaque）依赖
   P06 C1 契约，当前分支不含该契约，按 Lead 指示 deferred 到 rebase 后的小 commit。
3. **契约状态未翻 target implemented**：P05 目标生产可用性要求真实 Identity 授权链路与端到端
   验收；当前只有 fake/单测与编译证据，因此 `proto/tests/contract_status.json`、
   `api/openapi.yaml` 的 target 标注保持 not_implemented，未伪造状态。rebase 集成与 Identity
   接线完成后由后续 commit 翻转已验证项。
4. **Host 登记写入口**：按裁决只建记录与仓储，RegisterHost/ReportInventory 等写流程归 P08。
5. **RPC 传输测试**：本增量验证到编译与内部应用测试；真实 mTLS peer/delegation 与 Control→Runtime
   端到端验收留到 Identity 接线后（与第 1 项同一 commit/验收）。
6. Register/heartbeat 拒绝路径当前只写成功审计；拒绝审计与 P06 Bridge 一起补（避免在 P05 单机
   驱动路径引入半套审计语义）。
