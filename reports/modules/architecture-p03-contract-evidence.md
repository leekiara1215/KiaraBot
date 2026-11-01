# 契约证据与target归属门禁

工作区kiarabot-contracts，从干净403a543新建codex/p03-contract-evidence，旧分支保留。允许proto/tests、相关登记、doc/01-contracts/01-runtime-semantics及本报告；不改CMake/业务/proto定义/API/doc18。

输入为reference隔离后的历史证据及P01/P02状态，输出为明确路径解析和独立target生产可用性。错误语义为门禁非零失败：未知/缺失映射、blob漂移、symlink/越界、生产声明缺当前source/target测试、reference冒充生产均拒绝。

文档影响：必须同步proto/tests/README及doc/01-contracts、运行语义的执行范围说明，因为旧implemented的无范围解读会误导可用性；无线协议/HTTP/WS shape、状态码或生产行为变化，因此proto/README、API/doc06及业务模块说明无需由本任务修改，doc18保持只读。

初始实际运行check_implementation_status.py与check_contracts.sh均失败，43条旧Core evidence路径不存在。修复保留全部categories/既有断言，证据原逻辑路径通过已审查core/reference/manifest.json明确映射且逐blob校验。schema v2将历史categories界定reference非生产，target默认not_implemented，仅P02两个HTTP探针有current runner/target测试证据override；ready仍503。词表检查在reference范围保留，target词表扫描排除reference/客户端/测试字符串。业务方法没有因监听或历史成功变为可用。

## 能力一：证据映射与执行范围

实际验证：9项evidence_scopes_test.py通过并接入check_contracts.sh；check_implementation_status.py、enum/semantic门禁、Ruby target_api_test.rb（33操作与全部引用）通过。以临时Python环境grpcio-tools1.76.0/protobuf6.33.5生成全部Python消息，再显式设置KIARABOT_LEGACY_READER_TEST=1、KIARABOT_PYTHON、KIARABOT_GENERATED_PYTHON运行check_contracts.sh，descriptor生成/6个兼容场景/legacy-reader全部通过，无默认skip。该临时Python3.9环境不是P00固定环境，不据此声称固定全量环境完成。

通过git show 403a543逐项比较确认categories、evidence原逻辑声明、vocabulary完全未变；新增执行范围不取消历史implemented。bash -n、git diff --check通过。无C++/proto/API定义变化，不重复full/格式/tidy/正式API lint；本任务只读对应代码确认当前生产两个探针，未自行跑六进程业务或宣称77个RPC已实现。

## 能力二：P03/P04后续查询缺口审查（建议，尚未改契约）

证据基点仍403a543，门禁修复已独立提交275112e。本节只对照doc/20–22与当前proto/target内层接口列最小补充建议，不将草案当成已批准线协议或已实现业务；需要主对话先裁定具体表达，再由契约变更同步proto/API/权限/状态/生成测试。没有新增消息历史、自定义角色、市场、文件服务、HA或跨库读取。

### A. 方法和返回资源的具体缺口

| 缺口 | 当前证据与影响 | 最小建议 |
| --- | --- | --- |
| G01 Model操作无法查询 | model.proto的UpdateModel/RotateProviderSecret返回OperationRef，ModelService只有6个方法、没有GetOperation；OpenAPI的owner路由已宣称ModelService.GetOperation | 加ModelGetOperationRequest/Response与ModelService.GetOperation，复用OperationRef/Operation。owner固定model，读取存储操作后比较真实resource/owner，不信客户端自报resource；不可见/缺失统一NOT_FOUND |
| G02 账号读 | runtime.proto有AccountRecord及Create/Update/Disable，只有GetResourceState返回VersionedResourceState，没有账号详情/列表 | Runtime GetAccount/ListAccounts；单查返回AccountRecord，列表返回repeated AccountRecord和快照页。所需account.read为封闭权限候选，不让account.manage自动等于任意账号读 |
| G03 安装与实例读 | 已有InstallationRecord/InstanceRecord以及创建/启停方法，没有typed Get/List；通用state不含manifest、installation/host绑定 | Runtime GetInstallation/ListInstallations/GetInstance/ListInstances，分别复用对应record，明确parent安装筛选不等于读取该安装所有实例的授权 |
| G04 Adapter目录读 | 注册输入有implementation/version/protocol/accounts/capabilities，Runtime没有可查询的认证注册投影record或Get/List | Runtime GetAdapter/ListAdapters，新增AdapterRecord：稳定adapter instance引用、版本、最后有效注册metadata、声明/有效能力、账号绑定、last_registered_at/last_observed_at、当前online及可选当前Fence。offline保留metadata但不伪造在线，不根据名称猜NoneBot2 |
| G05 Host登记目录读 | host.proto HostRecord含包摘要/能力/版本，但只提供Host主动登记/库存；Runtime没有自身Host目录查询 | Runtime GetHost/ListHosts返回Runtime拥有的登记/最后观察投影（可复用HostRecord的公开字段）；实际进程执行仍查Host.GetExecution，不让Runtime投影裁决旧执行已停 |
| G06 已保存配置不可读取 | Runtime有UpdateConfig，GetResourceState仅配置版本号，不能重建编辑页或确认未知写结果 | Runtime GetInstanceConfig。独立InstanceConfiguration包含desired_version、完整已获权的配置（秘密仅引用/脱敏）、applied_version（可0）、应用状态/当前delivery引用、redacted错误和observed_at；offline无需虚构Fence。权限plugin.config.read，敏感在线核验 |
| G07 Model配置编辑与初始目录 | GetModelMetadata仅脱敏别名/状态/version，不能读取endpoint/provider_model/timeout；UpdateModel要求已有model_id/expected_version，缺初始模型建立约定 | 在P11前补受model.config.read/既有model.manage明确范围保护的GetModelConfiguration，永不返回provider secret。最小首装可离线建立明确Model资源引用及disabled配置，不必为本轮增加CreateModel新业务入口；密钥配置完成后再按真实条件启用 |
| G08 凭据状态及首个凭据引用缺失 | IdentityGetPrincipalStatus只接受principal_id并返回PrincipalRecord；Rotate/RevokeCredential却必须已有credential_id/version；RegisterPrincipal不返回任何凭据引用 | P04前补GetCredentialStatus（或在现方法中明确typed selector/对应结果），返回credential_id、principal/绑定资源、version、状态/有效期，绝不返回秘密/哈希。RegisterPrincipal响应须有初始凭据引用；可同事务建立version=1、disabled且无秘密的credential stub，首次Rotate按expected_version=1一次性交付新秘密；不是匿名登记 |
| G09 角色/资源对应关系 | PrincipalRecord把roles[]和scope并列，若调用方做笛卡尔积会给越权资源套上admin权限 | PrincipalRecord仅展示，禁止由Control据此本地授权。P04明确有效授予以AssignmentRecord的角色/权限/准确scope绑定为单位；必要时返回这些绑定和authorization_version，而非扩大为用户/自定义角色管理 |

G01的线协议草案仅供裁定：

```proto
message ModelGetOperationRequest { RequestContext context = 1; OperationRef operation = 2; }
message ModelGetOperationResponse { Operation operation = 1; }
// rpc GetOperation(ModelGetOperationRequest) returns (ModelGetOperationResponse)
```

G02–G05共10个typed Get/List，不用一个operation_name+Struct顶替；Get输入为context+明确resource引用，List输入为context+下述明确目录引用/已获权exact ResourceScope+分页，输出为对应具体record数组及快照信息。G06再加1个GetInstanceConfig；G07/G08各1个受限查询。现存Runtime.GetResourceState继续只提供通用状态，不扩成任意JSON数据库读取接口。普通查询默认2秒/最多5秒；未明确归类的读保持在线授权，依赖故障503/UPSTREAM_UNAVAILABLE，不回退成功空列表。

### B. exact scope的新建、列表和离线引导建议

当前ResourceScope只是一组exact ResourceRef。空scope、kind=UNSPECIFIED、任意字符串all、admin角色名、调用服务身份均不得隐式表示全资源；kind仍封闭枚举。不应把“list接口需要可用”变成发现所有业务资源的后门。

推荐主对话裁定一个**有限且有明确记录的管理目录授权锚点**：新增封闭RESOURCE_CATALOG（不是kind=all），部署时预注册每所有者/资源族的少数UUID目录记录。它是创建/发现的授权对象，不是新租户/业务平台或可由客户端任意定义的集合。创建请求新增明确catalog_ref；锚点只允许为该已登记目录创建/列举，**不授予目录内所有子资源的读写**，也不把child_ref自动解释为ancestor scope。ResourceScope本身仍逐个exact Ref判断。此表达尚未写进proto，不能被当前客户端使用。

具体安全流程：

1. **首次离线引导**：显式init/bootstrap创建Identity管理员主体、本人会话管理依据、有限目录UUID和明确固定角色/权限分配；只给选定目录创建/发现权限及已配置的具体Model/Host等resource ID授予，不给全库通配。bootstrap输入来自一次性受控部署文件/标准输入，私钥不入库；无有效管理员证明的匿名RPC不开放。完成后原子封闭bootstrap标记，重跑不能再发万能材料，普通服务证书不因此变管理员。
2. **新建资源**：CreateAccount/CreateInstallation/CreateInstance等由owner验证该catalog归属/种类和当前创建授权，再用owner CSPRNG UUID建立具体资源；同键作用域为已认证主体+typed方法+exact catalog/parent+key，空resource ID不能作幂等作用域。身份授予仍归Identity。creator对新对象的有限读/管理权限必须由创建策略明确列出，并产生对**该新UUID**的单独授予，不能由client任意指定permission，也不从admin自动继承所有敏感权限。
3. **跨服务无假原子**：owner同事务提交资源（未就绪/disabled）、收据、operation、审计和授权登记outbox；Identity用可信owner证明及版本化创建意图为该UUID登记资源/允许的精确授予，含收据/失效；owner收到持久确认后推进gate。Identity离线则operation Blocked，资源不得出站或被当成已授权可用；恢复同message/operation ID，不跨库事务、不提前响应全局成功。
4. **列表发现**：List请求给exact catalog_ref作查询边界；返回集合还必须与当前主体已明确获授的具体child ResourceRef集合相交，目录授权本身不暴露所有子资源/总数。Identity需能按“本人+已授权的exact目录”给出可见child Ref/授权水位；可最小补ListAuthorizedResources(context,catalog_ref,封闭child_kind,page)或对现有GetAuthorizationSnapshot定义同样受限的本人发现投影，不能用空scope列所有用户/授予。该发现仅返回资源引用，不是完整用户管理或授权可写复制。
5. **失权与分页**：分页绑定主体、exact请求scope、目录、权限/授权版本、页大小、排序及10分钟快照。每页当前会话/授权验证；会话撤销401，权限版本改变使旧cursor按既有规则失效，不从缓存继续返回撤销后的敏感记录。普通Get不可见/不存在统一404；列表不泄漏未获权项。授权服务不可用必须503，不能伪装空页。
6. **响应丢失**：Create未拿到server生成的新ID时，按原typed命令+catalog/parent+同幂等键恢复原收据。只允许查询/重放该调用方自己的创建意图；操作可绑定catalog但必须额外验证creator/方法/原意图，不能凭一个catalog grant读其他创建者的operation。七天收据窗外不得自动新键重做未知工作。Model操作查询严格用Model owner，不转Runtime猜状态。
7. **敏感权限**：角色能力与每条精确资源授予取交集。message.read永远不由admin/目录/creator默认获得；Action metadata、日志/审计、模型秘密管理仍分别显式授权。创建Account也不默认给所有插件该账号权限；新实例业务权限需另有明确绑定。

若主对话决定首轮不增加管理目录锚点，则安全的更小MVP只能显式离线预置有限业务资源UUID/绑定/授予，并限制在线操作为这些资源的更新/启停；不能宣称通用在线创建/列表已闭环。不能以空集合/all/admin回退来绕过上述缺口。推荐目录方案是为已批准管理范围补授权入口，不建议扩成任意资源树或复杂继承。

### C. P03/P04必须先固定的存储/恢复表达

- ReceiptKey.resource在创建前尚无新资源ID，必须先确定exact catalog/parent作用域。现有内部CommandReceipt只有operation ID；P03实现还须保存足够的不可变typed结果/新ResourceRef/版本或受限结果引用，才能重放原Create结果，不能用“读当前资源”伪造原响应。含新秘密的收据独立加密且复核当前轮次；模型正文不被通用收据顺便永久重放。
- SnapshotPage现同时含request和response字段。新增查询至少约定请求只接page_size/page_token，拒绝client提交snapshot_id/next_page_token/expires_at；返回快照ID/有效期/next token归server。也可拆独立request/page-info消息，须先更新唯一契约后实现。
- Actor/owner/目录归属不由正文字符串决定。Identity当前授权必须有受验证subject/actor/audience/证书绑定及检查版本；资源所属owner核对ResourceRef.kind/id及本地gate，不能为了列表在Control跨库联表。
- P03不需要新增远程SQL/schema管理RPC；显式空库init和本地schema验证仍由运维/基础设施承担。上述记录/receipt/snapshot的最小字段应在P03 DDL前定稿；Identity凭据/目录发现及role-scope绑定应在P04鉴权闭环前定稿；Runtime typed读与配置查询在P05/P12接线前补齐；Model GetOperation须在P11管理写返回operation前补齐。

### D. 本节检查与后续文件

实际只读核对runtime/identity/model/host/platform/policy.proto、target_spec.json、Core target context/foundation/UnitOfWork/business_ports和doc/20–22。静态验证确认ModelService仅6方法且无GetOperation，Runtime不存在上述11个typed查询、Identity无credential状态查询；没有生成新方法/字段或改变状态登记。草案若采用，直接影响对应proto、target_spec/descriptor/enum/状态与scope证据、api/openapi/asyncapi映射、doc01/06及相关模块；应再做同键恢复、授权/失权/空scope/all拒绝、分页/不可见、依赖故障测试。本节未跑业务/数据库或重复生成/lint：只有报告，无契约定义变更。

## 定稿：主对话已采用的最小补全（本轮实施授权）

主对话按用户授权采用上述G01–G09的最小补全及有限管理目录方案，本节取代B节中“建议/待裁定”的状态，不重新请求用户确认。目录是封闭RESOURCE_CATALOG、离线预注册每owner/资源族有限UUID；无通配、隐式祖先继承、任意集合或新租户系统。创建/发现对exact catalog授权，child另存精确grant。资源owner本地先提交disabled资源/收据/operation/outbox，Identity只接受登记owner的mTLS可靠交接，复核creator当前创建授权和canonical最小初始child grants；完成前不得启用gate。只发现当前本人获准的child refs，message.read等敏感权限不派生。

实施按能力独立提交：Model.GetOperation及受限配置查询；Runtime typed Get/List与完整配置查询；最后目录创建恢复/Identity登记与本人发现、凭据状态/初始引用和role-scope对应。新增线协议同步唯一来源、模块最小目标说明、API映射/状态/target_spec与生成/descriptor/跨语言fixture。全部新能力target not_implemented，保留既有owner RPC和三条stub桥接断言，计数以descriptor动态推导。CMake/业务实现不在本任务范围，doc18只读。

## 定稿能力：Model权威操作与配置查询

新增Model.GetOperation与GetModelConfiguration，复用resource.read/model.manage与Operation/ModelConfiguration；补唯一OpenAPI配置查询、按owner的操作权限映射和中立结果schema。同步proto/README、doc01/运行语义/doc02/doc06/doc21最小目标说明。新方法/字段/API均not_implemented；不改Model配置业务范围，不调用Provider。Schema正反例检查必填与秘密字段拒绝；goldens固定两种Model查询的字段编号/序列化，往返仍逐所有owner方法双向集合核对。三条stub桥接保留；活跃说明改为动态计数，历史已执行计数不重写。

Model能力实际检查：check_target.sh实际生成C++/gRPC/Python、编译并通过79 owner RPC策略/集合、273消息跨语言往返及2个固定wire golden。统一check_contracts（显式legacy reader）/9项scope回归/enum/semantic/descriptor兼容场景通过；Ruby 34目标HTTP操作的引用、RPC存在及owner权限映射通过；Ajv新增配置必填、秘密/URL凭据拒绝与中立operation响应正反例通过。Redocly2.72.1通过（3条unused warning，保留旧Runtime结果schema及WS跨文件组件），AsyncAPI parser3.6.6通过，无TLS关闭。版本沿前述临时环境，不冒充固定环境；生成物仅tmp。未跑生产服务/全量full/tidy：无业务或C++手写改动，target状态不变。

## 定稿能力：Runtime typed资源/配置读取

增加五类Get/List及GetInstanceConfig共11方法，ResourceKind追加封闭RESOURCE_CATALOG供必要的精确列表输入，目录登记/Identity可靠注册在下一独立能力完成。增加Adapter最后合法metadata/当前online与Fence分离、Host登记投影、InstanceConfiguration与SnapshotInfo；分页输入不允许server快照字段。Permission只追加四个读枚举、Host复用host.read；target固定角色只增加明确读能力仍逐资源授权，不含message.read。

OpenAPI增加11个目标GET（Adapter采用明确adapter-instances实例目录，不激活旧reference/adapters）；全部RPC/API/字段/枚举均not_implemented。资源Get以exact Ref、List以精确有限catalog，缺/错/all/超过页上限由query-shape fixture拒绝；当前授权、owner和child grant仍需未来生产边界核验。同步proto/README、doc01/运行语义/doc02/03/04/06/doc21，未修改CMake或业务源。

Runtime能力实际检查：check_target.sh生成C++/gRPC/Python并实际编译，90 RPC双向集合/策略、300消息跨语言、5个wire golden通过；typed query-shape夹具覆盖每类Get/List正确kind/UUID、缺目录/错kind/all/页>100拒绝。统一check_contracts显式legacy reader及9项scope回归、descriptor场景通过。Ruby 45目标HTTP操作映射/权限/引用、Ajv目录/在线Fence/拒绝配置原因/快照/空结果等正反例通过。正式Redocly2.72.1通过（4 unused warning）、AsyncAPI parser3.6.6通过；git diff --check通过。无业务服务、固定环境full或生产授权验收，target状态不变，代码生成物仅tmp。

## Runtime API diff可审查性修复

主审指出0de1622默认diff出现1255处删除。只读对b451d35精确核对证实：旧components块在仅追加RESOURCE_CATALOG/四个Permission后逐字节一致，全部旧路径方法解析值不变；默认重复块匹配导致大量误对齐（git --minimal原为958新增/3删除）。为避免依赖reviewer切diff算法，本次只将本能力11个新增GET的parameters/responses改为紧凑表示，旧路径/旧schema未序列化重排。现在相对b451默认diff为717新增/3删除，3行是必要target固定角色capability扩展。

实际将修复前完整API解析JSON与修复后逐项比较，语义完全相等；Ruby45操作映射/权限/引用、Ajv全部正反例、Redocly2.72.1与AsyncAPI parser3.6.6、git diff --check通过。无字段/约束/状态削弱，无协议语义变化，故不重复C++生成或业务测试。Identity字段的工作区改动不包含在本独立修复提交中。
