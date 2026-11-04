# P05 创建与目录授权契约提案

日期为 2026-11-04。状态为待维护者审查，尚未批准实施。本提案只新增设计文档。代码、proto、OpenAPI、权限编号、数据库和 target 实现状态均未改变。

## 1. 要解决的具体缺口

当前 Runtime 的 CreateAccount、ValidateManifest、CreateInstallation、CreateInstance 和四种已接线的列表向在线授权端口传入空 scope。Identity.CheckAuthorization 要求非空 scope，且被调用方法须有 descriptor 的 scope_kinds。列表的 RPC 层只验证 catalog 具有 CATALOG 种类和非空 ID，随后丢弃该 ID。创建方法的 scope_kinds 尚未登记。上述路径不能据已有业务实现认定真实授权已完成。

列表的目录加逐对象授权、离线登记的资源族 UUID、无父子权限继承和每页重新授权已经冻结。创建对象采用哪个授权锚点、目录 UUID 的部署权威来源及请求如何携带它，属于本提案请求批准的选择。[现有设计](../../doc/21-service-contract-design.md#9-目标查询与精确管理目录补全实施定稿仍未实现)

## 2. 推荐的目录登记方式

每个独立 Runtime 部署登记五个互异的 UUIDv4，资源族为 accounts、installations、instances、adapters、hosts。每个 ID 在线协议中都是 RESOURCE_CATALOG，其资源族由 Runtime 的只读部署映射确定。ValidateManifest 和 CreateInstallation 共用 installations 目录，表示允许提交安装候选清单。清单校验本身不产生插件类型资源。

UUID 由部署初始化阶段生成并持久保存于部署配置源。禁止启动时重新生成，禁止代码内写固定通用 UUID，禁止调用方自报一个 UUID 就成为有效目录。建议给 Runtime 配置新增封闭的 resource_catalogs 对象，五个键必须齐全，值必须为规范小写 UUIDv4，重复或未知键使配置加载失败。Control 的资源族映射和 Identity 离线 enrollment 由同一部署源派生。原配置缺少映射时，允许既有 exact 方法继续运行，但受影响的创建和目录方法保持 CAPABILITY_NOT_SUPPORTED，不把服务整体置为不可启动。

Runtime 拥有资源族到目录 ID 的映射权威。Identity 继续拥有主体、Assignment 和授权版本权威。Identity 不通过跨库查询验证 Runtime 对象存在。现有 bootstrap 的 administrator_assignments、control_assignments 和 platform_peers 可表达这些精确目录 grants。现有 SetAssignment 可供有权的管理员后续增删 grant。本方案不新增 Identity RPC、目录表或跨服务事务。

任意未登记 UUID 的 Identity grant 即使被离线导入，也不能通过 Runtime 的资源族映射检查。运维工具应在生成 enrollment 前校验映射一致性。若未来需要在线目录注册、目录本身的生命周期或跨 Runtime 动态发现，须另立接口和存储提案。

目录 ID 是长期资源身份，不能用更换 UUID 代替撤权。撤权通过现有 Assignment/凭据生命周期完成。灾备恢复必须保留目录映射及 Identity grants 的一致性。变更映射会导致原请求及分页令牌失败，必须按停写配置迁移处理。

## 3. 逐方法权限与作用域

以下权限沿用现有 Permission 枚举。在线授权的调用者仍须为已登记、mTLS 验证通过的 Control 平台 peer，并携带当前用户对 Runtime 的有效委托。

1. CreateAccount 使用 ACCOUNT_MANAGE，scope 恰为 accounts 目录。成功返回新 AccountRecord。它允许在该目录创建账号，不授予新账号 ACCOUNT_READ、ACCOUNT_MANAGE 或其他 exact 权限。
2. ValidateManifest 使用 PLUGIN_MANAGE，scope 恰为 installations 目录。返回 valid 和现有 violations。解析、大小及现有受支持的语义校验照常执行。通过校验不授予清单声明的能力，不保证安装后可执行。
3. CreateInstallation 使用 PLUGIN_MANAGE，scope 恰为 installations 目录。成功返回新 InstallationRecord。持久资源及收据归 Runtime，不创建 Identity assignment。
4. CreateInstance 使用 PLUGIN_MANAGE，scope 恰为请求 installation_id 对应的 RESOURCE_INSTALLATION。此方法不使用目录 scope。仅在父安装存在且生命周期允许创建时提交。指定 host_id 只是选择部署位置，不能由父安装授权推导出 HOST_READ 或其他 Host 管理权限。
5. ListAccounts 首先核验 accounts 目录的 ACCOUNT_READ，再对每个候选 RESOURCE_ACCOUNT 独立核验 ACCOUNT_READ。
6. ListInstallations 首先核验 installations 目录的 PLUGIN_READ，再对每个候选 RESOURCE_INSTALLATION 独立核验 PLUGIN_READ。
7. ListInstances 首先核验 instances 目录的 PLUGIN_READ，再对每个候选 RESOURCE_INSTANCE 独立核验 PLUGIN_READ。父安装 grant 不代替实例 grant。
8. ListAdapters 首先核验 adapters 目录的 ADAPTER_READ，再对每个候选 RESOURCE_ADAPTER 独立核验 ADAPTER_READ。
9. ListHosts 首先核验 hosts 目录的 HOST_READ，再对每个候选 RESOURCE_HOST 独立核验 HOST_READ。当前生产 typed Host 查询与持久注册接线仍有独立缺口，本提案批准授权设计不会使其自动完成。

列表的两次授权调用各自只携带一个资源引用。禁止将整个目录和所有子对象拼成一个授权要求，也禁止用任意一个 child grant 反推目录权限。目录权限存在、当前可见 child 集合为空时返回合法空集合。目录没有权限时整体拒绝，不能返回可见 child 的部分结果。

为支持这两次检查，五个 List 方法的 descriptor scope_kinds 必须分别含 RESOURCE_CATALOG 和该方法唯一 child kind。CreateAccount、ValidateManifest、CreateInstallation 只含 RESOURCE_CATALOG；CreateInstance 只含 RESOURCE_INSTALLATION。descriptor 的种类列表仅限制允许的资源种类。Runtime 应用仍须按方法验证目录族和授权检查顺序，不能把元数据中的两个种类解释成任一者足以放行整条列表。

## 4. 线协议与 API 的最小改动

建议给 RuntimeCreateAccountRequest 追加 ResourceRef catalog = 5，给 RuntimeValidateManifestRequest 追加 ResourceRef catalog = 3，给 RuntimeCreateInstallationRequest 追加 ResourceRef catalog = 4。编号须在实施时再经 descriptor 检查确认空闲。字段必须显式存在，kind 必须为 RESOURCE_CATALOG，ID 必须匹配当前方法的部署目录。CreateInstance 已有 installation_id，不新增字段。五个 List 请求已经有 catalog，不改字段编号。

追加字段在 Protobuf 编码层可兼容旧读者，但新服务端要求字段存在是请求语义收紧。旧客户端未填 catalog 会收到 INVALID_ARGUMENT。不能在服务端静默填入默认目录，以免把缺少授权上下文的旧调用转化为有效创建。旧服务端会忽略新字段，因此升级前必须禁用这些原先尚未验收的入口，不能让新客户端回退到旧 owner。

Control 对应 CreateAccount 和 CreateInstallation 的 REST body schema 增加必填 catalog，校验 TargetCatalogRef 并原样转发。五个 List 已有必填 catalog 查询参数。ValidateManifest 如无现有 REST 入口，本轮不新增入口。CreateInstance REST 继续使用 installation_id。Control 只能从已验证会话的逐条 AssignmentRecord 为请求所需的目录或父安装生成委托，不依据显示角色、scope 并集或清单推导权限。

创建响应是创建权限所允许的一次结果交付。客户端可以取得新 ID，但后续 Get/List/Update 仍须相应 exact grant。已有一次性 bootstrap 明文交付的字段级延期决定继续适用，该字段须单独满足 P06/P12 验收，普通回执不得保存明文。

## 5. 创建、幂等和分步授权

顺序为校验输入及目录映射，进行当前在线授权，再在 Runtime 的一个 UoW 中检查父对象条件并提交新对象、审计与收据。重试每次重新授权。授权被撤销后，持有旧幂等键也不能读取历史创建结果。

创建不自动向 Identity 发 SetAssignment。若调用者随后需要读写新资源，授权管理员必须对返回 ID 执行现有 SetAssignment，再重新取得当前委托。这是显式的第二步。第二步失败时，第一步创建的资源仍保留。界面应显示资源已创建、授权待完成，并允许有权管理员继续授予权限或走现有生命周期处理。不能把 grant 失败伪装为资源创建失败，也不能自动补偿删除。

创建的幂等键仍由主体、方法及稳定创建域绑定，不采用尚未生成的新资源 ID。当前 runtime-create-account / runtime-create-installation / runtime-create-instance 是内部收据标记，它们与公开目录 UUID 的授权身份是两件事。建议本轮保持旧收据 key 的稳定性，把显式目录或父安装纳入规范请求 fingerprint。改变内部 receipt key、旧结果编码或普通回执保留策略须与另一份持久回执提案协调，不能作为元数据修改隐含实施。

数据库提交不确定时返回既有未知结果恢复语义，客户端使用同一幂等键查询或重试，不分配新业务意图。依赖故障、超时、重试和授权变化不得产生第二个对象。初次创建和后续 exact grant 的审计分别由各自 owner 记录。

## 6. 列表快照与撤权

现行 OpenAPI 的五个 List 描述要求不可变的十分钟快照。当前 Runtime list_page 只保存 keyset 位置，跨页继续读取实时行，且实现注释明确不冻结行集。实施必须解决这项差异，不能只补 scope_kinds 后把列表登记为完成。[HTTP 唯一契约](../../api/openapi.yaml)

推荐保留现有外部快照保证。首次请求在 Runtime 本地一致性读取中创建有界的不可变候选快照，保存记录、排序、观察时间、查询指纹及目录 ID。快照令牌绑定主体、方法、目录、页大小、当前授权版本和固定到期时间，子页不能延长原十分钟窗口。进程内快照可复用当前有界 cursor 生命周期，重启后旧令牌返回 CONFLICT。建议初始预算为全局最多 1024 个快照、每主体最多 64 个，每快照最多 4096 条候选记录，全局最多 16384 条候选记录且编码后的快照数据总量不超过 64 MiB。这些是本提案待批准的容量参数。任一预算超限返回 RATE_LIMITED，不能静默截断为末页。

每一页都先重新核验目录授权，再核验待返回记录的 child 权限。权限版本变化使旧令牌返回 CONFLICT，客户端从第一页重新查询。单项当前无权或已不可见则不披露该记录；Identity 不可用、超时或无法证明当前授权时整页失败。不能把依赖故障当成一个普通的隐藏对象，也不能从部分成功结果伪造完整页。

快照期间新建、修改或删除记录不改变原快照的数据视图，但每页的当前授权检查仍成立。客户端如需最新权威状态，重新建立快照或使用 exact Get。第一页重放和任一旧令牌重放必须保持同一页位置及固定快照事实。响应只含 SnapshotInfo，不接受客户端伪造服务器观察时间。

如果维护者希望保留实时 keyset 行集，需要明确批准修改 OpenAPI 与 doc/21 的外部语义。那是另一种设计，本提案默认不采用。即使采用实时方案，达到当前 32 批扫描上限时也必须给可继续的令牌或稳定容量错误，不能误报没有后续数据。

## 7. 错误和失败恢复

- catalog 缺失、kind 错误或 UUID 非规范，以及非法分页输入，返回 INVALID_ARGUMENT。
- 格式正确但不匹配方法资源族的目录、未登记目录或无当前 grant，返回 PERMISSION_DENIED。错误不披露另一个资源族的目录内容。
- 有权访问的父安装不存在，使用现有 NOT_FOUND；存在但生命周期不允许创建，使用现有 CONFLICT。未通过父安装授权时先拒绝，不探测存在性。
- 已支持的清单业务校验不通过，沿用 RuntimeValidateManifestResponse.valid=false 和 violations。无法解析请求或超过传输边界限制时使用现有输入错误。
- 同键异参、游标主体/目录/方法/授权版本不匹配、游标过期或进程重启导致快照不存在，使用 CONFLICT 和现有恢复建议。
- 身份依赖不可用使用 UPSTREAM_UNAVAILABLE，超过 deadline 使用 TIMEOUT，容量超限使用 RATE_LIMITED。gRPC、HTTP、acceptance 和 recovery 由现有稳定映射生成，不新增错误码。

## 8. 发布和兼容步骤

1. 维护者批准本提案中的作用域、配置与请求字段，以及保持不可变快照的实施范围。
2. 先修改 proto/OpenAPI 唯一来源及 doc/01、doc/02、doc/06、doc/08、proto/README 和 target_spec 的设计输入，保持 target 实现状态未通过。
3. 在隔离部署源生成五个 UUID，派生 Runtime/Control 配置和显式 Identity grants，校验重复、错误族及缺失配置失败行为。
4. 同步部署配置加载、RPC 转换、应用授权和 Control 请求转发。旧 owner 入口在升级期间保持禁用。新 owner 的 catalog 缺失请求拒绝行为必须先验收，再启用新 Control。
5. 通过逐方法真实授权、故障和兼容门禁后，才逐字段登记 target 实现状态。ListHosts 仍须等待其生产数据来源接线证据。
6. 回滚前停止受影响的创建/目录入口，保留新建资源和收据。旧 owner 不理解目录检查，不能在新授权方案下直接重新开放入口。

## 9. 验收条件

真实 Identity、Runtime、Control 证书和隔离 PG 测试须覆盖九个方法各自的允许、错误 kind、错误资源族、未登记 UUID、缺目录 grant、只有目录无 child、只有 child 无目录、另一个资源族目录、撤销 grant 和身份依赖故障。

创建测试应验证同键换 request_id 和进程重启只产生一个对象，创建成功后 exact Get 仍拒绝，显式 SetAssignment 后才可读取。实例创建必须拒绝另一个父安装，不能因获得安装目录管理权限而创建任意实例。清单能力声明不能产生权限。

列表测试应覆盖两页以上、可见与不可见对象交错、32 批以上的稀疏可见集合、不同主体/目录复用令牌、授权变化、同页重放、资源在翻页期间变化、容量拒绝和 Runtime 重启。快照的到期时间不得随着翻页后移。

协议门禁运行 descriptor 兼容性、旧客户端读取器、target_spec/contract_status/evidence scopes 一致性、OpenAPI 解析和跨语言 round-trip。鉴权和配置属于外部可观察行为，不能仅靠单元测试翻转状态。

## 10. 审查范围及文档影响

本次需维护者确认的具体范围为三项创建/校验方法采用目录授权，实例创建采用父安装授权，五种目录与逐对象授权的元数据组合，五个稳定离线目录的配置来源，三个追加的 catalog 请求字段及对应 REST 必填字段，以及按现行契约完成有界不可变列表快照。

当前只提交本提案。后续实施直接影响 proto/kiarabot/v1/runtime.proto、proto/tests/target_spec.json、api/openapi.yaml、proto/README.md、doc/01-contracts.md、doc/02-core.md、doc/06-control-api.md、doc/08-operations.md 和对应 target evidence。目录快照与幂等语义还须检查 doc/01-runtime-semantics.md。doc/18-architecture-baseline.md 保持只读，若实现选择与冻结基线存在差异，由维护者决定是否修订。

P06 投递、P07 Action、P08 进程调度、P09 存储和 P11 Provider 契约无需因本提案同步修改。Host 权威注册落地、Identity 在线目录生命周期和通用资源授权向导均不包含在本提案的实施授权内。

核对依据包括 RuntimeResourceApplication 的 create_account、validate_manifest、create_installation、create_instance 和 list_page，runtime_rpc.cpp 的 ListAccounts/catalog，IdentityAuthorizationApplication 的 CheckAuthorization 方法策略检查，以及 identity_bootstrap_main.cpp 的 assignments 解析。图索引为 2026-10-30 且上述文件未跟踪，本次依据精确源码读取，没有依赖图索引的缺失结果判断接口不存在。


## 11. 本文档实际校验

执行了 git diff --check、本文内部状态检查及本地相对 Markdown 链接存在性检查，均通过。本文没有变更线协议或实现，因此 descriptor、代码生成和 C++ 构建测试无需为这次纯提案重新运行。后续实施的门禁已在第 9 节列明。
