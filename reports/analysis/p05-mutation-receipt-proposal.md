# P05 五项写操作的持久回执方案

2026-11-04。状态为待维护者审查。本文提出存储和升级选择，不授权执行 DDL、切换数据库或修改契约。范围为 Runtime 的 DisableAccount、BindAdapter、UnbindAdapter、SetDesiredState、UpdateConfig。

## 现有缺口和保持的契约

`core/include/kiarabot/core/domain/target/persistence.hpp` 的 CommandReceipt 保存主体、方法、资源、幂等键、指纹、ReplayResult 和保留期。`runtime_resource_application.cpp` 的 mutate() 重放业务快照后，使用新请求的 request_id 查询 operation。真实 Runtime、Identity、PG 测试确认，同键换 request_id 会缺少原 operation；同资源的另一操作 ID 还可能成为错误引用。资源归属检查已补齐，但不能由此还原原命令身份。

`runtime_rpc.cpp` 的 UpdateConfig 又用配置版本作为资源版本，并填写 Active、Running；已停止的实例因此可能收到错误状态。当前结果没有提交时完整的实例状态。事后读取最新实例、按时间猜测审计或重执行命令，都无法恢复原结果。

保持现有 wire 字段、权限、operation ID 分配规则和幂等键作用域。每次重试先重新校验当前身份与权限。成功回执恢复原始业务结果及 operation 引用，operation 的最新执行进度仍通过 GetOperation 查询。新请求的传输诊断 ID 不参与业务结果比较。同键异参维持既有 CONFLICT 映射。UpdateAccount 无 operation 输出，已验证原账号快照重放，本方案不扩大到该方法。

## 建议的 Runtime 专用存储

推荐为 Runtime 增加版本化的结果附属表，暂名 runtime_management_receipts。以 command_receipts.result_id 为主键及外键，并以父回执清理为生命周期边界。保持现有 ReplayResult variant 的索引和其他三个 owner 的编码方式。该方案比扩展共享 ReplayResult 多一种类型更局部，但仍属于待批准的存储模型变更。

表保存 format_version=1、方法/结果类型辨别信息、原始 OperationRef 的 owner/operation_id/resource_kind/resource_id，以及必要的提交时状态。原资源快照仍保留在现有 replay_results 及对应结果表，附属表只补缺失信息。约束 operation owner 必须为 Runtime，引用资源必须匹配原结果和回执资源。五项成功写入必须同时存在父回执和完整附属记录。实现时需由 Runtime 专用仓储验证字段与方法组合，不能将任意 blob 视为合法回执。

实例两项的状态快照完整覆盖 ResourceState 中实际输出的字段，包括 resource、version、lifecycle、desired、fence（可空）、registered、desired_config_version、applied_config_version、process_outcome、observed_at 和 source_service。Fence 保存原始各部分，空值保持空值。配置回执同时保留原 InstanceConfiguration；SetDesiredState 同时保留原 PluginInstance。两者的观察时间取原事务权威时间，重放不刷新观察时间，也不从当前行拼装。

Core 增加 Runtime 专用中立回执值类型和仓储端口。ConfigMutation 提供已保存的 ResourceState，RPC 仅做协议转换。基础设施实现关系与 codec，不能决定状态。具体列类型、检查约束、实际 schema 版本和 descriptor 由实现前的 schema diff 固定，本文不预占版本号。

BindAdapter 首次可能返回 bootstrap 明文。该明文只在首次成功响应中交付，普通回执不保存它，也不使重试再次交付。操作引用和绑定快照可以持久重放。若首次明文丢失，沿用既有凭据生命周期处理，秘密恢复能力另行设计。

## 单次事务和竞争

1. 按既有路径核验身份、权限和 deadline。进入 Runtime UoW 后，用完整 ReceiptKey 查询父回执。
2. 已有回执必须先核对 fingerprint。新格式回执读取不可变结果和原引用，不查询以本次 request_id 命名的 operation。记录不完整、版本未知或外键/资源不一致时拒绝继续，不补写猜测结果。
3. 新命令依照现有 CAS 修改业务行，创建原 operation、Host 命令等已有副作用意图；同一 UoW 内保存业务快照、附属回执和审计。
4. 只有提交确认成功才回成功。提交结果未知沿用 UNKNOWN/按原身份查询的现有语义；不得用新键重试副作用。并发同键由数据库唯一键和事务冲突保证一个提交，竞争失败者重新读取原回执。

附属表不依赖可清理的 operation 当前行来重放引用。operation 进度表、Host 命令、投递和其他未决义务按各自保留条件清理。父回执过期时附属行一并清理，不能因新增表让原回执提前删除。下游数据库故障、容量上限和编码失败必须让事务整体失败。

## 升级方案与旧数据

当前 `doc/22-transactions-and-migration-design.md` 第 11 节采用正式首次发布前的空库初始化，服务启动不自动 DDL。为已有数据提供原地升级工具属于新增工作，需要随本方案单独批准。

推荐本地验收采用新的隔离 Runtime 空库。原库保持封存，所有实例、绑定、凭据和旧进程不得被新环境自动复用。重建所需测试资源经获准的登记流程供给，不能把换库视为真实部署数据迁移。该路径可快速验证新存储，但不能声明已有业务数据升级通过。

如必须保留原业务库，采用停写、预检、显式离线迁移，拒绝混跑旧 writer。

1. 备份四库与相关 Host/Adapter 日志，记录 schema descriptor、水位和未决义务。停止五项入口及会调用它们的后台 writer，保留其他服务的边界。旧实例不得在切换期间继续写旧格式。
2. 只读预检枚举五项仍在保留期内的旧回执。现有记录无法可靠还原原操作和提交状态，默认不做回填。存在此类回执就阻止切换，列出数量和最晚 retain_until，不输出密钥或配置正文。
3. 保守路径等待所有受影响旧回执分别达到 retain_until，再通过既有 owner 清理流程清除可清理记录。至少七天是最低保留期，实际等待依据存储期限。期间旧版服务只能提供其原有行为，其重试缺原 operation 的问题仍存在，不能承诺完整重放。需要连续完整重放的线上环境应暂停此升级并另审数据恢复方案。
4. 确认清理不破坏未决义务，且没有旧 writer 后，用迁移身份在显式事务中新增 Runtime 附属表、约束和 schema 标识。服务运行身份没有 DDL 权限。迁移需检查准确旧版本和对象集合，失败回滚；重试只接受已完整迁移状态或准确旧状态，部分状态拒绝。
5. 启动新 Runtime，校验 descriptor、权限和健康，再运行无外部副作用的验收。满足门禁后开放入口。其他三库不应因本地表新增被执行 DDL，但共享 schema 生成器是否改变其 descriptor 必须实测检查。

回滚分两段。新格式首次写入前可停服务并在核对 schema 后回到旧部署。首次新格式写入后，旧版不能正确读取完整回执，禁止直接降级并开放 writer。此后优先前向修复；若恢复备份，必须协调业务数据和 Host/Adapter 日志的水位，并处理备份后已发生的副作用，不能只恢复一张表后重放命令。清空数据库或删除旧回执不属于本方案授权。

## 验收和批准范围

五项分别验证首次成功、同键同参换 request_id/trace_id、重启后重放、同键异参、不同主体、已撤权、CAS 冲突、资源被后续命令修改、配置在 STOPPED 实例提交后重放。绑定重放秘密字段必须为空。原 operation 引用保持一致，错误资源/owner 引用不可用。

真实 PG 故障测试覆盖唯一键竞争、写父回执后故障、写附属表后故障、COMMIT 响应丢失、进程终止和重启。需要证明无孤儿回执、无重复业务版本、无重复 Host 命令。迁移测试覆盖旧活跃回执拒绝、到期但未清理记录、未决义务、超期清理后的切换、错误 schema、DDL 失败重试以及首次写入前后的回滚门禁。

待批准的具体选择为 Runtime 专用附属表、五项原子保存完整回执、实例 RPC 消费原始状态，以及升级采用隔离新库验收还是额外实现保留数据的离线迁移工具。建议先批准存储实现与隔离空库验收，原地迁移作为独立任务评审。

批准后需同步 doc/01、doc/02、doc/22、doc/08、proto/README 中直接受影响的重放说明及测试登记。proto wire 与 API schema 预计无需改动，仍须运行 descriptor/旧读取器检查验证；doc/18 由维护者持有。本提案仅增加说明文档，未运行 DDL 或切换数据库。
