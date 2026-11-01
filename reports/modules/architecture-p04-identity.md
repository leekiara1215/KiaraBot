# P04 Identity首接口与纯规则实施记录

基点为已审阅P03公共端口/规则和独立ns/Catalog修正；分支codex/p03-core-ports。
本轮获父授权新增typed安全模型/crypto-clock-random ports与纯policy；生产接线等P03关闭。
不改P03 unit_of_work.hpp的ABI/纯虚集合，不实现SQL/传输/密码算法，不启动新进程或假ready。

## 首接口能力（先独立交付）

精确文件：
- core/include/kiarabot/core/domain/target/identity_permissions.hpp
- core/include/kiarabot/core/domain/target/identity_types.hpp
- core/include/kiarabot/core/domain/target/identity_credential_rules.hpp
- core/include/kiarabot/core/ports/target/identity_crypto.hpp
- core/include/kiarabot/core/ports/target/identity_repository.hpp
- core/tests/architecture/identity_contract_test.cpp
- doc/02-core.md
- reports/modules/architecture-p04-identity-draft.md（原草案及采用决定）
- reports/modules/architecture-p04-identity.md（本报告）

输入输出：内部带版本Credential/Session、精确role+resource/permission+resource、typed秘密
scope/回放绑定、验证后的peer/claims、可信时钟/随机端口；均使用Outcome/Failure，不含
Protobuf/JSON/SQL/TLS对象。crypto结果认证拒绝bool false与依赖Failure分离，不能吞错。

安全材料：CredentialVersionRef唯一版本；Password/WorkloadBootstrap/CertificateBinding
分离封闭分支；LoginIdentity明确用户名归属/密码credential索引，非任意字典。
SessionSecurity独立access/refresh摘要、用途/key版本、各自期限、uint64轮次/版本及
绑定credential；metadata与安全行同事务，公开expires_at=refresh期限。
access15分钟/refresh30天/秘密回放24小时按父确认固定；rotate/revoke后旧绑定版本拒绝。

秘密回放：EncryptedSecretReceipt与普通ReplayResult不可互转；暂态载荷为move-only
SecretReplayPayload(token对或一次凭据交付)，AEAD adapter处理typed载荷而非万能JSON。
SecretBuffer安全内存/擦除由成熟adapter负责；Core不自实现擦除/哈希/AEAD/JWS或弱随机。
binding认证scope/主体/会话/凭据/请求keyed commitment/轮次/创建截止时间；防替换密文与
修改行时间续期。每次回放先查当前状态/轮次，不能凭旧收据恢复已撤销或轮换秘密。

仓储：IdentitySecurityUnitOfWork为新增派生类及typed factory，复用同一物理事务的
P03 repos，新增5组安全repo；不改旧factory签名，不cast旧返回值、不另开连接凑原子性。
ExactAssignment保留同一主体的原子role/resource和permission/resource配对，不能从不同
row借scope组合权限；它是同一授权权威的typed视图，非可独立写的第二授权库。
Bootstrap marker只显式Required->Consumed，缺marker不是空库自举；与管理员/密码/
精确授予/水位/审计原子提交，工作负载bootstrap另型，连接proof续签不在首接口中虚构。

词表：Permission按已审阅0de1622:policy.proto的39项逐名对应，wire数值不直接cast；
额外4项准确为ACCOUNT_READ/PLUGIN_READ/ADAPTER_READ/PLUGIN_CONFIG_READ。
G08/G09后续登记/发现权限、认证shape与连接proof帧仍待契约增量，不放行unknown。

密码学/时间：仅声明PasswordCrypto/TokenCrypto/SecretReceiptCrypto/Random/SecurityClock/
PeerCertificate/DelegationCrypto ports，沿锁定jwt-cpp/libsodium；禁止Core手写算法。
固定Ed25519，真实peer证书来源于传输且对照登记；拒绝none/算法降级/未知key/jku-x5u。
56+2+2<=60秒、最大两次转交、资源/actor/cert收窄均是adapter/后续policy必须验证的契约，
当前没有签验实现，不将声明当成安全验证已通过。crypto/密码高CPU操作在事务外。

错误语义：查询成功空optional才不存在；依赖失败Outcome传播；save要求expected version/
rotation/唯一约束；无效材料不能fallback；commit Unknown继续沿P03原身份查询恢复。
实际授权、会话期限、回放/引导状态机的正常/拒绝/过期/冲突行为将在下一独立规则能力测试。

文档影响：同步doc/02、原草案与本报告；未接外部行为、不改唯一线协议/API或doc/01、
03–08/18。P03公共头未变。P04schema/生产crypto/Control每次权威验证与WS撤销验收后续任务。

## 首接口实际检查

identity_contract_test.cpp按C++20 -Wall/-Wextra/-Wpedantic/-Werror编译执行通过；验证
派生关系、旧/新factory类型分别保留、秘密回放不可进入普通ReplayResult、暂态token不复制、
三类固定期限。四个新头逐个独立-Werror语法编译通过，不依赖生成协议或外层头。
其余format16/既有回归/边界/inner/词表对照与diff检查在冻结前补录；无PG/crypto实现测试。

根CMake不属本任务范围；工具链可注册identity_contract_test为p04/unit。此时尚未授权
生产接线，不因端口存在令Identity readiness变true。

首接口冻结检查：foundation/invariants/persistence_contract/persistence_rules/time/
identity_contract六个纯测试-Werror重编译执行通过；四个新头分别独立编译、format16
--dry-run --Werror通过；边界/inner/diff与39项Permission对唯一commit逐名检查通过。
P03 unit_of_work.hpp的git diff为空。未执行PG/生产crypto/JWT/TLS/RPC验收：仅端口声明，
没有对应实现，不能将静态类型测试称作认证业务已通过。

## 首接口审查修正与冻结说明

1. 显式CredentialState::Pending及PendingCredentialMaterial(kind)无材料分支；factory创建
   version1 stub，validator拒绝Active+Pending/空hash、Pending+已发放材料、未知kind/state。
   Revoked是真实撤销并要求撤销时间；Expired由UTC投影，不新增持久化伪终态。validator
   只检查结构，不声称已验证PHC算法/证书/密码正确性。
2. SessionSecurity.created_at为不可变权威元数据，SessionMetadata明确refresh expires。
   list_metadata是subject一致有界读取，超过可信快照容量RateLimited/故障Failure，不
   截断假空页；供每页当前鉴权、10分钟不可变、重启失效的后续快照实现使用。
3. RefreshReceiptScope仅peer/session/key，CredentialDeliveryScope仅actor/credential/key。
   input_rotation和expected_version移到SecretRequestParameters，必须同时进入keyed
   commitment及AEAD binding；方法variant固定不能交叉错配。Rotate无secret body仍对
   expected_version做MAC。测试比较同scope不同parameters/binding，未用fake MAC冒充crypto。
4. resolve_refresh(input verifier,exact peer/key lookup,DB now)返回Current或Consumed
   RefreshHandle，仅解析不授予认证；已消费输入只在原scope/key、24h与原输入有效期内
   恢复。record_refresh_consumption与session轮换/metadata/secret receipt同一物理事务；
   只存verifier和原scope/轮次，无raw token。旧token新key不能解析历史；原key恢复还须
   检查当前session结果轮次、credential版本/active状态及receipt期限后才能解密。

最小contract测试已加入typed Pending工厂/状态材料拒绝、真实created_at/refresh期限投影、
稳定scope与mutable parameters分离、错误方法参数拒绝、resolver/list query返回形状。
本次首接口新增identity_credential_rules.hpp后，精确冻结范围由8文件更新为上方9文件。
后续role/resource交叉授权、全会话/秘密恢复行为与生产crypto/PG另独立实现测试。

修正后的最终检查：六个target测试再次-Werror编译执行通过；五新头分别独立-Werror
语法编译、固定format16、边界、inner、diff通过。identity_contract新增结构/scope测试
另以ASan/UBSan编译执行通过。P03 UoW差异仍为空。生产MAC/AEAD/刷新历史PG查询和
快照缓存尚无实现，本测试不冒充这些能力验收；G08 wire映射继续等唯一提交SHA。
