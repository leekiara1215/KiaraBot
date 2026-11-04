# P05 Runtime 五项持久回执实施记录

日期：2026-11-04。工作树：`.worktrees/p05-receipt-impl`，分支 `codex/p05-receipt-impl`，基线 `b296187`。维护者批准 Runtime 专用附属表、五项完整回执与实例 RPC 提交状态；验收限定隔离新库，原地迁移和旧部署切换另审。未 push。

## 实施与边界

目标为 Core Runtime 应用、中立 UoW 端口、RPC 转换和 PostgreSQL Runtime owner 适配器。输入仍为既有五个 RPC；输出保持既有 wire，重试返回原业务快照、原 operation 和实例提交状态。当前授权每次重验，同键异参维持 CONFLICT；不完整或未知格式回执 fail closed。BindAdapter 不持久或重放 bootstrap 明文。

新增 runtime_management_receipts 以 result_id 引用 command_receipts(result_id) ON DELETE CASCADE。格式 v1，CHECK 固定五个方法、结果类型索引、Runtime owner、Account/Instance 资源和完整状态/fence组合。共享 ReplayResult 索引保持不变。附属表与业务、原 operation、Host 命令意图、父回执和审计同 UoW 保存。逻辑过期及两条父回执清理路径保持原保留语义。

交叉审查另修复 effect 在资源已写后因纯 crypto/ID 端口失败仍提交的问题：先回滚业务 UoW，再独立短 UoW 尝试拒绝审计；审计失败或其提交未知保留原业务拒绝错误。Runtime 请求指纹清 RequestContext 后使用固定版本 protobuf deterministic 编码，防 Struct map顺序或进程hash导致同参误冲突。该指纹修复同样不得直接用于有旧活跃回执的既有部署。

## 提交

- da142dd：中立回执值/端口、Core 五项原子保存、RPC 原状态转换、Runtime schema v18及codec。
- 9a0017e：真实PG附属表故障、完整codec、过期和COMMIT回包丢失恢复测试。
- d40fa14：unit与五项mTLS重启/新requestid回放回归，公共重放文档。
- e97076a：effect失败业务回滚与独立拒绝审计。
- e6d5960：真实PG production应用+纯SecretPort失败回归。
- 9303890：protobuf map deterministic指纹及三字段逆序重建测试。

## 实测

独立Debug目录 `build/receipt`，依赖使用主工作区锁定安装：architecture-installed、PG16.4、OpenSSL3.5、Python3.11.18 SDK venv，未覆盖主build。

1. 构建 Runtime、Identity、bootstrap、initializer、Runtime application test、Postgres database test、Runtime RPC及descriptor compatibility工具通过。新增源无新增编译警告；保留既有unused function、enum switch、重复库与scalar braces等警告。
2. `ctest --test-dir build/receipt -R '^p05_runtime_resource_application$' --output-on-failure` 通过，最后一次1.39秒。包括原operation行清除后换requestid重放、撤权拒绝、同键异参、缺附属拒绝、后续修改不改变STOPPED配置原状态/原时间。
3. `ctest --test-dir build/receipt -R '^p03_postgres_database$' --output-on-failure` 最后一次48.19秒通过，真实四owner隔离PG16.4，覆盖父/子保存后故障整体回滚、Codec最大uint64/fence/ns时间、不同主体读取隔离、原子副作用、逻辑过期、过期键替换/prune级联、真实TLS中继丢COMMIT响应后的完整回执恢复。生产Runtime应用在Bind bootstrap失败、Bind hash失败、SetDesiredState Host hash失败后均无资源/operation/receipt半提交；额外撤销audit INSERT权限证明原拒绝错误不被审计故障改变。夹具只控制自身创建的临时数据库并清理全部私有材料。
4. `ctest --test-dir build/receipt -R '^(proto_descriptor_exists|p01_target_api)$' --output-on-failure` 2/2通过。
5. `scripts/ci/check-descriptor.sh b296187`（锁定protoc/include、compatibility工具、SDK Python与legacy-reader）通过：契约状态/语义、descriptor六场景、legacy reader及基线兼容通过。无wire字段变化。
6. 三个Python e2e文件 py_compile及git diff --check通过；变更C++按clang-format16格式化。

本lane本机mTLS重试因既有GetAccount/UpdateAccount等入口读写出现5秒DEADLINE或拒绝断言失败，位置随采样变化，均在新增回放分支之前；未放宽断言或增加自动重试。曾因独立工作树未生成Python包失败，随后已用SDK生成器修复。本lane停止并行e2e以避免与Lead串行gate争用；Lead已报告集成后两个mTLS gate通过，将在deterministic版本及Compose镜像再验。最终集成证据以Lead记录为准。

Runtime descriptor由owned PG16.4独立测量：schema18，4c5649131c56be99dd4e6d06af3a9aa1，154 relations。四owner initialize/open验证其余保持 Identity decc8401bef9d3ac3c92116e7608b2f4，Scheduler73411831b606f8340ab7b441401aad8e，Model f12417031e89ea2bb4f6c8d153447292。

## 文档影响

已更新 doc/01-contracts.md、doc/01-runtime-semantics.md、doc/02-core.md、doc/08-operations.md、doc/22-transactions-and-migration-design.md、proto/README.md。提案 p05-mutation-receipt-proposal.md 已在本地改为“批准存储实现与隔离新库，原地迁移另审”。提案为已有跟踪文件；本实施报告按任务要求显式加入版本控制，未包含私有配置或秘密。

API路径/schema、proto字段、SDK和适配器接口不变，无需修改api/openapi.yaml、api/asyncapi.yaml及对应客户端文档；doc/18冻结基线保持只读。中央target状态和文档登记由Lead集成处理。

## 未纳入范围

未执行既有部署库DDL、旧活跃回执回填、原地升级或逆迁移。未声称旧数据升级通过。无真实QQ/NapCat、付费Provider、生产CA/离站备份验收。本lane不承担Compose重建（Lead负责）。单写者并发begin拒绝和实际唯一约束由真实PG gate覆盖；没有新增跨进程同时同键压力测试，writer单活动进程约束保持不变。架构检查所报Core OpenSSL依赖等是b296187已有问题，本轮未扩大重构。
