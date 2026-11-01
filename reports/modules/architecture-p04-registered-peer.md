# Identity RPC current registered-peer authenticator

本能力接续3295e1f与六方法修正5a69018，独立新增实际生产认证回调，不混入Issue/Exchange或变更业务协议。只修改identity_rpc.hpp/.cpp、其测试cpp/py、doc01/02/08和本报告；未改纯应用、PG、主入口、配置、仓库CMake或保护doc18。已取父审Core5a1f5eb/bdc05b9（本分支530930a/e577cad）公开规则，不再依赖未提交Core工作区或复制应用体。

公开签名：

```cpp
target::Outcome<PeerAuthenticator> make_identity_peer_authenticator(
    target::IdentitySecurityUnitOfWorkFactory& transactions,
    target::PeerCertificatePort& certificates, target::SecurityClockPort& clock);
```

端口均借用至服务停止及Wait。工厂返回实际回调而非占位：真实AuthContext→受限证书证据→fingerprint CURRENT索引→current head与主体→DB UTC及Core当前凭据规则→释放读UoW→受信时钟/DB偏差核验→成熟链/时间/SAN/角色验证→新UoW再次核验当前登记。全部证书端口/时钟调用在UoW释放后，期间撤销/禁用/轮换不能凭初始快照通过。无已登记凭据即Unauthenticated，依赖失败原稳定码传递，时钟不可信/偏差超过2秒UpstreamUnavailable；不构造假Peer/秘密收据/业务授予。

每阶段检查取消/真实传输期限及2秒本地预算；同步端口自身必须遵守已审有界I/O，不能凭此检查声明可抢占任意挂起调用。应用仍在自身事务复核当前peer/来源授权。与37f7a62生产typed工厂接口兼容，组合根须将此回调与应用绑定同一Identity权威工厂；本提交不在回调中另建数据库或替代组合根。Control平台主体和Human成功投影的binding必须持久提供，不能在边界补造；Admin修正由父负责。

验证沿独立/tmp CMake：fixed protoc29.3/Protobuf5.29.3/gRPC1.71/OpenSSL3.5，AppleClang21/C++20/Wall/Wextra/Wpedantic/Werror。完整原六方法RPC测试保留，并第二台真实mTLS服务器使用本生产回调与实际OpenSSL PeerCertificate adapter。仓储/时钟是明确fixture；不冒充真实PostgreSQL闭环。fixture监测UoW活动数，证明证书验证时为0，最终fresh read在验证期间撤销时拒绝；覆盖无登记、DB故障、主体禁用、head版本不符、错误SAN、时钟不可信/与DB差3秒，以及正常当前登记成功。不调用任何grant/receipt写端，unused写端直接抛测试异常。

首轮真实成熟验证器发现旧证书fixture缺CA签名KeyUsage，已在本任务py测试临时目录生成满足X509_STRICT的CA/叶扩展，未放松生产verify。测试不输出密码/token/私钥或异常正文。临时CA/私钥和生成源码不提交。

实际命令与前RPC报告相同（build目录/tmp/kiarabot-p04-identity-rpc），另将实际infrastructure/identity_crypto/src/peer_certificate.cpp编入临时测试库。主集成的identity_rpc_test须链接kiarabot_identity_crypto与OpenSSL::Crypto；不应把infrastructure头送入domain/application/ports。真实mTLS测试已通过。inner/模块边界、固定-Werror build、format16通过；最终tidy22和状态/diff结果随后记录。

文档影响：doc01/02补认证时序与错误/当前性，doc08补生产装配与借用生命周期；wire/API无变化，不改README或重复descriptor/lint。target implemented及readiness不变；未运行仓库full和真实PG生产验收，CMake最终由buildchat接线，PG/应用/生产clock/bootstrap闭环由父验收。Issue新增将另cap提交，不在这里假实现。

最终固定依赖-Werror build、真实mTLS全套（包含成熟证书验证及验证中撤销/时钟偏差）、clang-format16、两TU clang-tidy22全部analyzer/warnings-as-errors、inner/边界/状态文档及diff门禁均通过。库/测试仅/tmp构建，主仓库CMake未改。stage检查无生成物/证书/私钥。
