# G08 Identity权威模型对齐

基点d1ba916，按唯一契约78b09a6（已由父审查）；优先交PG作者，不混入尚未提交的
identity_policy.hpp / identity_state_rules.hpp。不改P03UoW或proto/API，不实现crypto。

精确文件：
- core/include/kiarabot/core/domain/target/identity_permissions.hpp
- core/include/kiarabot/core/domain/target/identity_types.hpp
- core/include/kiarabot/core/domain/target/identity_credential_rules.hpp
- core/include/kiarabot/core/ports/target/identity_crypto.hpp
- core/include/kiarabot/core/ports/target/identity_repository.hpp
- core/tests/architecture/identity_contract_test.cpp
- doc/02-core.md
- reports/modules/architecture-p04-g08-models.md

Permission追加MessageRead（按名映射，内部40），不得被固定角色/creator默认派生。
ExactAssignmentSpec以variant表示一个固定role或非空explicit权限；持久投影仍按原子
role/resource与permission/resource，后续policy拒绝混用/未知/空scope并核验更新的旧新并集。

CredentialVersionRecord新增binding ResourceRef及optional expires_at；Active必须finite
expiry>created_at，Pending没有expiry，Revoked可保留旧expiry。bootstrap有效期不得超过
材料expires，cert不得超过not_after，Password由可信策略设置期限。Expired仅当前UTC
投影，status输出ref/subject/binding/state/created/expiry/checked，不输出材料。
make_pending_credential(id,subject,kind,binding,now)显式接收权威binding，stub version1；
旧的无binding helper签名不再使用，避免默认Principal/Instance错误归属。

文档影响：同步doc/02及本报告；wire已有78b09a6，不在本树修改；其余模块和doc18不改。
实际测试：typed Pending无伪expiry、Active无expiry拒绝、精确expiry投影Expired、Revoked
优先、bootstrap/cert expiry不能越材料、MessageRead新增、既有稳定scope/恢复接口回归。
contract test按C++20 -Wall/-Wextra/-Wpedantic/-Werror及ASan/UBSan编译执行通过。
固定format16、头自足、其余回归与边界检查在冻结时补录；无PG/生产crypto验收。

冻结复核：六个target测试重新-Werror编译执行、三个变更头独立编译、固定format16、
40项Permission与78b09a6逐名比对、边界/inner/diff均通过。P03接口未改，未混入规则草稿。

## 同一必要兼容补丁的显式来源/current证书索引

父追加要求同步冻结，当前范围为上方8文件，取代先前6文件清单。未混入两个纯规则
草稿或应用接口。DelegationClaims新增source_credential及optional source_session，JWT
必须签入，exchange保留原subject/credential/session；结构测试缺来源拒绝、删session/
换版本拒绝。在线Current来源状态规则后续实现，不靠authversion猜测session未撤销。

CredentialRepo新增by_certificate(Sha256Digest)->Outcome<optional<CredentialVersionRecord>>：
唯一leaf/current-head受限索引，仍核验真实subject/service/SAN/active/expiry，不以请求
body身份选择凭据、不扫描所有行。PeerCertificatePort的同一span签名不变，仅明确leaf-first
连续DER X509串的有界编码（总64KiB/8张/非空/无额外前缀或尾字节），由OpenSSL adapter解析。
没有实现生产签验或SQL；这为正在并行的JWT/PG作者提供同一编译接口，不声称认证链已通过。

最终8文件冻结：六项相关target测试及另行应用声明contract测试均-Werror通过；当前
identity_contract含origin/by_certificate/expiry结构检查再次ASan/UBSan通过；固定format16、
边界/inner/diff通过，P03 UoW文件未改。生产JWT/PG实现与两个规则草稿不在本提交范围。
