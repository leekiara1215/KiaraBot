# Identity最小安全应用公开接口

状态：声明供RPC/Control并行接线；具体应用实现随后独立能力交付，不假称方法已可调用。
基点b0949b3（含G08模型、来源与真实证书索引），本次仅新增下方三文件供边界并行。

精确当前文件：
- core/include/kiarabot/core/application/target/identity_security_application.hpp
- core/tests/architecture/identity_application_contract_test.cpp
- reports/modules/architecture-p04-application-interface.md

## 公共入口与构造依赖

IdentitySecurityApplication继承ServiceReadinessPort，服务Identity。新增工厂：
make_identity_security_application(IdentitySecurityDependencies, IdentitySecurityOptions)
返回Outcome<unique_ptr<IdentitySecurityApplication>>。所有依赖由组合根借用注入且覆盖
应用整个生命周期；不替换P02无参数readiness factory，不在声明阶段解除未实现gate。

依赖：IdentitySecurityUnitOfWorkFactory、PasswordCryptoPort、TokenCryptoPort、
SecretReceiptCryptoPort、RandomPort、SecurityClockPort、DelegationCryptoPort。
PeerCertificatePort由真实RPC传输边界验证TLS证据后产出VerifiedPeer，应用再核验当前登记
peer credential版本/状态；不得从HTTP/JSON/自报certificate hash构造VerifiedPeer。
可信options给当前/保留验证key引用、receipt/commitment key、dummy password verifier、
密码凭据有限寿命、secret/session/snapshot容量；空配置/零预算/非法期限要构造失败，
不能用弱随机/无限容量/假ready弥补。没有增加JWT/SQL/HTTP对象或新第三方依赖。

## 六个typed请求/响应

| 方法 | 请求 | 返回 |
| --- | --- | --- |
| login | RequestContext、Verified Control peer、username、借用password span | move-only EphemeralTokenPair及DB checked_at |
| refresh_session | context、peer、借用refresh token span | 同上，原key受限恢复 |
| validate_session | context、peer、借用access token span | 当前Principal、SessionMetadata、逐条ExactAssignment、authorization version及checked_at |
| get_principal | 同access请求 | 当前Principal、逐条ExactAssignment、version/checked_at |
| revoke_session | context、IdentityActorProof、SessionId | 当前脱敏SessionMetadata及checked_at |
| list_sessions | context、IdentityActorProof、SubjectId、typed page输入 | 脱敏sessions、实际page_size、snapshot/next token、expiry、version及checked_at |

context明确request_id/trace_id/idempotency_key/deadline；secret span仅借用至方法返回。
IdentityActorProof为真实peer加Identity-audience证书绑定delegation，应用通过crypto验证，
再在线读取原source session/credential以及当前scope；不是边界自报actor结构。
Page输入只收page_size/page_token/snapshot_id，拒绝混入wire输出expires/next；0取20、最多100。
输出10分钟不可变、scope/subject/size/auth-version绑定快照，每页仍当前授权，重启失效，
DB/授权/容量失败用Failure，不返回假空页。显示role/permission并集不参与授权。

## 事务/错误顺序（实现必须遵守）

读/定位记录的短UoW释放后进行密码/HMAC/AEAD/签验/CSPRNG准备，再新开最终短事务：
复核credential/session/rotation/current authorization，条件写业务+历史refresh handle+
成功receipt+audit/失效水位；Committed后才响应。Unknown只按原key查询恢复，不能发
未提交秘密、换key重复执行或把未明当回滚。昂贵crypto/远端调用不得持有任何DB UoW。

查询每次读权威当前状态，current credential/session优先于旧receipt。应用边界不得传播
异常到RPC/HTTP，预期拒绝Outcome稳定Failure，依赖故障不能转为不存在或无权限成功。
所有方法须并发安全：每调用独立UoW，只有有界不可变快照缓存采用明确本地同步。

## 已集成委托来源（b0949b3，边界必须保留）

DelegationClaims须签入CredentialVersionRef source_credential及optional<SessionId>
source_session。Exchange原封保留，不能删session改成纯workload origin或换credential。
Identity高风险在线检查以该来源读当前credential head/active/expiry及session revoke/expiry；
普通读仍<=60s预算，不能仅以authorization_version假定所有session revoke均改变它。
上述字段已由b0949b3提交，成熟JWT adapter须在签验时包括它们；本次三文件不重改模型。
本人ListSessions采用本人Principal UUID+IdentitySessionRead，principal_id必须显式本人；
Revoke采用目标Session UUID+IdentitySessionRevoke，由Identity权威核验本人归属可自管，
跨主体要求精确grant，不能从Principal role继承所有Session。Control只做当前Validate后
IssueDelegation的精确scope映射，不构造非UUID Catalog、空subject或自行放行业务。

## 实际检查与限制

identity_application_contract_test以C++20 -Wall/-Wextra/-Wpedantic/-Werror编译执行通过，
校验6个请求/Outcome响应、factory依赖签名、readiness继承和秘密回复不可复制。
头仅标准库及内部ports；format16/边界/inner/diff在冻结前复核。没有业务实现，未验证
PG/真实密码学/RPC行为，不把声明编译当成生产Identity链已通过。
文档影响限本报告：尚无外部行为或方法实现，不重复改G08冻结中的doc/02、proto/API及
运维。应用实现/来源字段提交时再同步直接描述行为的doc/02；保护doc/18不动。

冻结三文件：本头、application contract test、本报告。offline bootstrap及IssueDelegation
后续各自追加typed能力，不与本声明交付混合；实际工厂/六方法实现随后继续，不停止在草案。
实际复核：C++20 -Wall/-Wextra/-Wpedantic/-Werror声明contract编译执行、format16、
边界/inner/diff通过；无生产方法体，不宣称网络认证已实现。
