# P04 Identity 极小内部接口草案

状态：原始草案已由父任务审查采用；现授权typed安全模型/crypto-clock-random端口与纯policy。
生产crypto适配器/数据库schema/应用与网络接线仍待P03关闭及后续任务；P03纯虚集合不修改。
基点：b2761da3e44ee96932d601b6975b01b2f9b4b5c1。仅本报告，不写业务/SQL/传输/假ready。
依据：[通信与方法](../../doc/21-service-contract-design.md)、
[事务与初始化](../../doc/22-transactions-and-migration-design.md)、
[Identity目标契约](../../proto/kiarabot/v1/identity.proto)、
[权限词表](../../proto/kiarabot/v1/policy.proto)。G08/G09/认证shape和Catalog等待契约任务定稿。

## 1. 目标与最小范围

优先闭环：离线可信引导 → 密码登录 → 每次REST当前会话验证 → 同键刷新恢复 → 注销/撤销。
再接：固定角色的精确资源授权 → 登记工作负载/实例引导凭据 → 受限委托 → 失效版本/快照。
不实现自定义角色、联合登录、通用CA/KMS服务、匿名登记、自动签发部署根或用户管理平台。

Core只接收内部类型；JWT/证书/密码算法、TLS对象、数据库连接和序列化库在适配器。
所有接口继续用Outcome/Failure。只在P03稳定且父授权后新增Identity专用类型与扩展端口。
后续可能文件：domain/target/identity_types.hpp、identity_policy.hpp；ports/target/
identity_crypto.hpp、identity_repository.hpp；application/target/identity_service.hpp。
以上为原审查阶段候选路径；本轮首接口已按采用决定创建部分类型/端口头，详见实施记录，
仍无生产应用或二进制实现。

## 2. 领域值：不把凭据材料混入P03普通回放

| 类型草案 | 最小字段/职责 |
| --- | --- |
| CredentialId / CredentialVersionRecord | 独立类型ID、subject、kind、uint64 owner version、Pending/Active/Revoked、创建/撤销时间；typed PendingMaterial无秘密，kind为Password/WorkloadBootstrap/CertificateBinding的封闭分支 |
| PasswordVerifier | 成熟adapter产出的编码验证材料、hash policy版本和必要key引用；不存原密码，不在Core编写Argon2/HMAC/比较算法 |
| BootstrapVerifier | token验证摘要、key version、精确实例/资源绑定、允许audience、有效期/撤销状态；不是永久流授权；消费与续期语义等G09明定 |
| CertificateBinding | 被验证证书的SHA-256、受信SAN/服务身份、subject、资源绑定、凭据版本和有效期；请求自报指纹不能变成已验证peer |
| SessionSecurity | SessionId、subject、绑定凭据版本、access/refresh验证摘要与key version、各自到期时间、rotation、owner version、撤销状态；raw token只短暂存在于内存响应 |
| ExactAssignment | AssignmentId、subject、一个精确ResourceRef、可选固定role、显式封闭Permission集合、version；不得保存role集合和资源集合的全局笛卡尔积 |
| VerifiedPeer / VerifiedDelegation | 来自验证边界的直接peer及subject/actor链、iss/aud、iat/nbf/exp、jti、资源与permission原子绑定、证书绑定、授权版本；普通请求字段不构造可信身份 |
| EncryptedSecretReceipt | 明确Login/Refresh/CredentialDelivery用途、幂等scope、带密钥的请求commitment、subject/session/轮次绑定、ciphertext/nonce/key引用、创建/保留期限；不进入ReplayResult |
| BootstrapState | 单例状态Required/Consumed、owner version、完成时间、最小非秘密证据；启程后不能因重启/空缓存重开引导 |

Access/refresh摘要使用有用途区分的成熟adapter验证材料，摘要密钥/加密根由部署提供并与
库内密文分离。无日志格式化/自动输出secret值；加密载荷内的token对不进入audit/outbox。
SessionSecurity与P03 LoginSession同一owner、同一UoW，后者是脱敏元数据，不是第二授权源。
两种token到期不能由一个expires_at混用。父已确认access15分钟、refresh30天不变，公开
SessionRecord.expires_at=refresh会话有效期，TokenPair另含access期限。rotate/revoke后，
旧绑定credential version的后续验证/刷新/秘密回放拒绝；不修改P03字段来存秘密。

## 3. 仓储与UoW扩展：保护P03正在适配的ABI/纯虚集合

不往现有IdentityUnitOfWork/Factory加纯虚方法，不改其他三个服务UoW。
拟在P03稳定后另增IdentitySecurityUnitOfWork : IdentityUnitOfWork，与独立typed factory；
应用构造时显式注入该factory，不能从旧工厂返回值dynamic_cast、拼装多个独立事务或共享连接。
扩展对象的base repos与新增repos都必须属于同一个Identity事务；旧P03实现仍可独立编译。

下表是方法意图，不是当前已提交的C++声明：

| 新增typed repository | 最小读写/原子条件 |
| --- | --- |
| IdentityCredentialRepository | 按CredentialId/current version、登录标识或已绑定主体查验证材料；create-only/expected_version写；主体+kind+version唯一，不复用撤销材料 |
| IdentitySessionSecurityRepository | 按access/refresh摘要索引查SessionSecurity；按SessionId+expected version+rotation条件轮换/撤销；与base sessions元数据原子更新 |
| IdentityExactAssignmentRepository | 按subject及精确resource读绑定；按assignment/version条件写；验证封闭permission与固定role，不在库层发明授权规则 |
| IdentitySecretReceiptRepository | 按受限Login/Refresh/交付scope查加密原结果；同键异commitment冲突、容量满拒绝；重放前同时校验当前subject/session/credential/轮次仍有效 |
| IdentityBootstrapRepository | 读取持久单例状态，Required+expected_version条件消费；首次管理员、密码材料、显式授权、水位/审计与Consumed同事务 |

共同receipts/journal/operations/current_time/commit继续复用P03父接口。Identity授权变化
同时更新现有AuthorizationState.version+invalidation_dirty；普通outbox满也不能丢撤销。
新session/credential验证材料不能塞进P03 Principal/LoginSession的字符串字段或普通ReplayResult。
P04初始化新增表及schema版本由infra单独任务实现；本草案不修改P03 schema或提前执行DDL。

## 4. 登录、轮换与秘密重放的最小应用时序

密码登录：受控边界输入用户名/密码、幂等键、已验证Control peer；规范化登录标识并限频。
密码验证等高CPU操作在短事务外，材料缺失也执行受控dummy验证避免可观察用户名枚举。
准备token/加密回放所需随机材料后，短事务复核主体及凭据版本/状态，再原子写session元数据、
验证材料、加密成功收据及必要审计。提交成功才返回一次token对；commit Unknown只能同键恢复。
请求commitment若涵盖密码等低熵秘密，不能用普通裸SHA-256留下离线字典验证材料。

刷新：先以可信摘要定位会话/受限原收据，再锁session、rotation、secret receipt scope。
同键、同原刷新请求且当前仍为该成功结果轮次，才解密重放原token对；不续期、不再次轮换。
旧token换新键、当前轮次已前进、已注销/撤销、主体失效、receipt失效均不得返回旧秘密。
加密认证绑定用途+scope+subject+session+请求commitment+轮次/版本，防止替换密文行串响应。
相同key换不同请求的具体认证优先序/稳定错误按G08/G09冻结，不能用冲突响应泄露其他会话存在。

每次ValidateSession读当前主体/会话/凭据状态与token用途/期限，不能以尚未过期JWT或缓存替代。
REST每次向权威Identity验证，依赖不可用失败；WS在Control按250ms/500ms预算与750ms最后确认
门禁完成最迟1秒关闭；Identity只提供当前事实，不把异步失效流当作此保证的唯一机制。
注销/撤销的状态、水位与审计同事务提交；不等待远端WS关闭，不恢复旧token。

## 5. 固定角色、封闭权限与精确资源绑定

Permission由唯一契约定稿后逐项映射为Core封闭枚举；边界将未知wire值/方法拒绝，不能让
客户端字符串自由创建权限。HTTP full_method/业务操作到Permission的映射属于受审服务端表，
不是调用者提供的“所需权限”。不复制Protobuf类型或在Core内反射descriptor。

授权条件按同一绑定行/原子授权项判断：subject匹配、resource kind+id完全匹配，且该行固定role
允许该方法或该行显式Permission允许；需要多资源时每个目标分别满足，不能把所有role和
所有scope扁平合并。例：admin@账号A + viewer@账号B 不能管理B；operator@实例A不能写实例B。
默认admin/operator/viewer不含message.read；敏感Action检查、消息元数据、日志/审计等只由
明确范围显式能力放行。插件主体不用人类role，实例资源授予与Runtime业务gate仍共同约束。

Catalog仅等唯一契约新增后的有限目录管理锚点，用于明确登记/枚举方法；不是“*”，不继承
到目录内账号/实例/消息，不允许Catalog授予推导子资源操作权。当前不增Core kind，不修改wire。
角色授予与显式permission的完整合法组合、目录方法G08边界由契约测试和纯policy表共同冻结。

## 6. 最小密码学、peer、时钟和随机端口

| 端口草案 | 输入 → 输出；拒绝边界 |
| --- | --- |
| PasswordCryptoPort | 暂态秘密+验证材料/固定hash policy → Outcome<Verified/Rejected或新Verifier>；用成熟libsodium等适配器，Core不实现密码算法；失败不可当密码通过 |
| TokenMaterialPort | 指定用途及强度策略 → 随机opaque token/验证摘要；验证为恒定时间成熟adapter；摘要用途隔离，随机失败不发凭据 |
| SecretReceiptCryptoPort | 暂态token对+typed binding+key引用 → AEAD密文；反向只向已授权当前轮次返回暂态结果；篡改/未知key拒绝，不退明文 |
| DelegationCryptoPort | 内部受限claims+SigningKeyRef → JWS；opaque proof+已验证peer+受控keyset → VerifiedDelegation；固定Ed25519，拒绝none/算法降级/任意jku/x5u/未知key |
| PeerCertificatePort | 真实传输证书及部署登记 → VerifiedPeer；X509/TLS只存在外层；链有效仍须SAN/服务身份/当前凭据绑定，body里证书hash不替代真实peer |
| SecurityClockPort / RandomPort | 有信任状态的UTC+单调时钟样本/已知跨节点误差 → Outcome；CSPRNG填充限定长度秘密值，失败显式；不把本机时钟当DB时间，也不伪造低误差 |

上述类型仅持内部值/opaque秘密/引用，不持JWT库对象、TLS句柄、SQL或数据库连接。
固定JWT/JWS库、AEAD实现与密码成本参数在授权后按既有依赖锁选择并实际验证，当前不新增依赖。
proof需验证iss/aud/sub/exp/iat/nbf/jti、每项permission/resource绑定、actor链和真实peer证书绑定。
每次转交换audience、绑定直接调用者并收窄权限，最多两次业务转交；不能将sender服务自身权限
变成用户/插件权限，不能把access/refresh保存进outbox后台执行。

安全时间预算沿doc/21：exp-iat<=56s，已知节点差<=2s、验签容差<=2s，总上界60s；时钟不可信
或超界则拒绝/不就绪，不靠请求活动续期。普通只读最多60s且不超过证明更早到期，重启清缓存；
高风险/敏感读在线校验，授权检查到本地提交/新副作用检查点<=1s，否则释放事务重新核验。
DB current_time及写入时UTC判定继续用P03端口，密码/签名/外部授权不得持有DB锁执行。

## 7. Bootstrap最小闭环与失败语义

首次管理员只走显式离线部署引导，持受控本地引导材料；公开Login不能“库空自动创建管理员”。
事务锁BootstrapState单例并以版本/唯一约束保证只消费一次；首次主体/凭据/精确授权/水位/审计
与Consumed原子提交。重启、依赖断线、恢复备份不能自动重新开放bootstrap或复活旧凭据。
工作负载bootstrap与初始管理员bootstrap分开建模，不能让Host凭据代表所有插件或所有资源。

错误继续使用Failure和既有ErrorCode：形状/未知permission或audience为InvalidArgument，
认证材料无效/过期/撤销为Unauthenticated，已认证但越精确范围为PermissionDenied，
版本/幂等异内容/轮次冲突按认证shape映射Conflict或Unauthenticated；容量/限频RateLimited，
依赖不可确认UpstreamUnavailable/Timeout，密文或持久状态损坏Internal。commit Unknown保留
Acceptance::Unknown+QuerySameIdentity，不将无响应当作已回滚或自动重发外部副作用。
公开错误不输出密码、token、PEM、ciphertext、key或用户是否存在；必要审计仅稳定状态与标识。

## 8. 实施前依赖、测试与退出条件

契约依赖必须先关闭：
- G08/G09方法认证shape、封闭Permission与默认角色/显式能力边界、有限Catalog资源唯一词表。
- 工作负载bootstrap交换后的证明续期凭据/消费规则，不能仅靠旧过期proof续签。
- 登录幂等scope、刷新同键回放与旧token/新键错误优先序，沿既有秘密收据期限而非普通7天。
- token TTL及公开expires映射、凭据换代拒绝旧会话已按父确认冻结；剩余认证shape由契约定稿。

建议后续独立能力：先typed安全模型+ports+policy纯测试，再Identity仓储/schema真实PG回归；
再离线bootstrap/登录/ValidateSession/refresh/revoke闭环，最后委托/失效与Control接入。
父已授权在P03实测期间并行typed接口与纯规则；生产接线仍等P03关闭，不更改其纯虚集合。

纯测试：role/resource交叉不提权、无默认message.read、未知permission/audience拒绝、
Catalog不继承、proof收窄/两跳/证书绑定/时钟边界、刷新同键/新键/旧轮次、秘密receipt篡改。
Crypto adapter测试：真实成熟库签验、错key/算法/nbf/exp、乱序及未知key、随机/密钥故障；
不能用恒真fake验签代替生产crypto验收。
真实PG测试：bootstrap/刷新并发只成功一次、事务前后断线/提交未知、版本/权限撤销与重放、
普通outbox满仍可提交dirty水位、密钥与密文分离、持久加密收据不泄漏、重启不恢复缓存授权。
传输/Control测试：真实TLS/SAN/peer绑定，REST每次当前会话验证、WS撤销/依赖分区SLO，
秘密不进入响应投影/日志/outbox，未实现业务不得假ready。不存在真实环境就明确未验收。

## 9. 本次文档影响与实际检查

仅新增本接口草案；无公共方法/协议/运行状态或可执行行为变化，因此不更新doc/02、
doc/01、03–08或proto/API；实现时再同步对应唯一来源/模块/运维和验证。doc/18未改。
本次不运行C++构建/PG/crypto/网络测试：没有新增或修改实现，也不将草案签名当已编译接口。
实际检查为相对链接存在性、git diff --check、仅本报告范围检查，结果随后收口记录。

收口结果：4个相对来源链接及2个P03依赖路径存在性检查通过；git diff --check通过，
工作区只有本报告新增。未改P03接口/源码/测试/构建/协议，未执行实现验收。

## 10. 已采用决定与本轮首接口

固定依赖沿既有jwt-cpp/libsodium，不新选crypto库；不在Core手写PBKDF/HMAC/AEAD/JWS。
秘密成功回放为24小时，AEAD binding认证用途/scope/subject/session/credential版本/请求
commitment/轮次以及创建/截止时间，不套普通7天receipt。默认角色无message.read。
首接口Permission按0de1622已有39项逐名映射；Catalog登记/发现、G08/G09增量另行对齐。
本轮首接口的具体文件/检查见architecture-p04-identity.md，不将原始草案中的后续方法
描述当作已实现服务。连接proof续签帧尚未冻结，不在本轮Identity类型里虚构其业务状态机。

父审查后的首接口补充：注册stub为Pending+typed无材料version1；Active须有匹配验证材料，
不伪造空hash或Revoked。安全session行新增不可变created_at及脱敏有界list_metadata。
secret receipt scope不含mutable rotation/expected_version；独立typed request parameters
进入keyed commitment与AEAD binding。已消费refresh input的受限resolver只定位原peer/key
的24h恢复handle，并非认证通过；同一事务记消费与轮换/密文收据，原结果当前轮次与
credential有效性仍须重查。生产状态机不在首接口中实现。
