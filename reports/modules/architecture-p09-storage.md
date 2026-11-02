# P09a-2 Runtime Namespace/CAS/Quota Storage — Implementation Report

## Scope

Runtime 私有 storage/共享 namespace 的第二阶段：PG 四表、typed 仓储、真 PG16.4 集成、Runtime owner RPC
适配、target 契约登记。只修改 Runtime 所属模块与直接受影响的共享契约文档；未改 Schema 之外的其它服务，
未新增服务/进程/端口，未实现 PluginBridge storage 帧或对外 HTTP 操作面。

## Delivered

- `infrastructure/postgres/schema/runtime_storage.hpp`：`namespaces`（kind 0/1、私有 owner 唯一、每安装
  共享唯一）、`namespace_members`（namespace+instance 主键，整表条件替换）、`namespace_values`
  （≤64KiB、版本、expires_at、expiry 索引）、`namespace_quota_counters`（scope 0 namespace / 1
  installation 聚合）；`namespace_receipt_results` 为 `command_receipts.replay_results` 的 Runtime 私有
  typed 载荷侧表（不新增 storage_receipts 表）。
- Runtime schema v11，真 PG16.4 实测 `descriptor=09c7176f032043f3bc7ba18f8c87b281`、
  `relation_count=134`，manifest 与 `v1.hpp` 已写回；空库初始化无迁移。
- `infrastructure/postgres/src/storage_records.{hpp,cpp}`：四端口 PG 实现。私有 namespace 以稳定实例 id
  惰性建行；值 get/CAS/delete 用条件写，过期行按 DB UTC 惰性删除并同事务调整两个计数器；配额准入按
  namespace→installation 固定顺序条件更新；共享 namespace/member 管理与禁用；storage/管理写收据复用
  `command_receipts`，同键同 fingerprint 重放原结果快照且不续 TTL，容量按实例/安装计数、有效行不淘汰。
- `factories.cpp`：RuntimeWork 用真实适配器替换 fail-closed 阶段 1 默认值。
- Runtime composition/RPC：注入 `RuntimeStorageApplication`，适配 unary storage 与共享 namespace
  管理方法（未注入时 fail-closed `CAPABILITY_NOT_SUPPORTED`）；成员/配额/授权仍全在纯 application。
- 修复阶段 1 真实 PG 才暴露的缺陷：探测 UoW 在写/删 UoW 之前未释放导致 writer lock 正确返回
  CONFLICT；delete 用 `DELETE ... RETURNING` 判定影响行而不是 pqxx 空结果。
- target 契约：翻转共享 namespace 管理的 11 个 proto message 与 39 个 target-scope field；storage
  帧/语义项保持 not_implemented。
- 文档：`doc/01-contracts.md`、`doc/02-core.md`、`doc/05-infrastructure.md`、
  `doc/22-transactions-and-migration-design.md`、本报告。

## Verification

- `cmake --preset full-locked` 全量构建成功。
- `ctest -R 'p09|p03_postgres|p05|p06|p07'` **13/13 passed**，含新增
  `p09_postgres_storage_records`（owned PG16.4 集群；私有/共享 CRUD、CAS 冲突、收据重放、成员 gate、
  TTL 过期、授权拒绝、共享 namespace 管理/禁用、重启持久）。
- `proto/tests/check_implementation_status.py` 一致性通过。
- clang-format 16 dry-run 与 `git diff --check` 通过。

## Not implemented / handoff

- PluginBridge `HostRequest` storage 帧映射（当前代码库尚无 PluginBridge ingress 消费路径）与对外 HTTP
  storage 操作面；未做 quota 并发横压测（writer lock 串行矩阵外）。
- Runtime storage 相关 target message/field 未翻转；共享 namespace 的语义规则仍标 not_implemented，
  待 bridge 帧与目标测试补证后再翻转。

## P09b plugin bridge ingress

- PluginBridge 现在消费 `PluginToCore.host_request` 的 storage Get/Set/Delete/CompareAndSet 帧：
  私有 namespace 由已认证实例派生，`installation_shared` 由注册 profile 的 installation 解析；
  插件自报 `installation_shared_id` 或其他 host operation 一律拒绝（CAPABILITY_NOT_SUPPORTED）。
- 授权复用 P06 bridge 会话：ConnectionAuthorization 版本/有效期 + 注册 profile 的
  `storage.namespace` 组成受信 decision，`RuntimeStorageApplication` 校验 subject/permission/
  scope/新鲜度后才执行成员、禁用、配额、TTL 与 `command_receipts` 收据逻辑；wire 帧不能伪造。
- 真 TLS bridge + 真 PG16.4 矩阵：私有 Get/Set/Delete/CAS、同键同 payload 重放、冲突重放
  CONFLICT、stale CAS CONFLICT、TTL 过期、重放不续 TTL、namespace 配额 RATE_LIMITED、共享
  namespace condition/成员拒绝/禁用、显式 namespace id 拒绝；见
  `tests/architecture/p06_bridge_acceptance.py`。
- 同尺寸 CAS 的零配额 delta 曾触发 INVALID_ARGUMENT，已在应用层跳过零 delta 准入修复。
- target 翻转：`StorageEntry`、`StorageCondition`、`StorageCompareAndSet*` 4 个 message、26 个
  storage target-scope field、5 条 storage/namespace semantic rule；unary `Runtime*Storage*`
  message/field 与 `NamespaceScope.installation_shared_id` 保持 not_implemented。
