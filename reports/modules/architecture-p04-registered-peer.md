# Identity RPC current registered-peer authenticator

## 当前定稿：Core持有权威语义

9b96968的外层直接read_registration实现被父审拒绝，必须与本后继重构一起整合。现已删除重复仓储/凭据/主体/证书前后复核逻辑，只有一条Core授权用例，不保留旧API或fallback。

```cpp
target::Outcome<PeerAuthenticator> make_identity_peer_authenticator(
    target::IdentityAdminApplication& application, target::SecurityClockPort& clock);
```

wrapper仅从真实AuthContext提取受限指纹/DER，按本地2秒与实际gRPC更早期限构造PeerRegistrationRequest，调用Core authenticate_peer。Core f2e8500+7623d94负责DB UTC/current index/head/principal/current凭据规则、释放UoW后调用成熟证书端口和验证后fresh recheck；outer不读仓储、grant或receipt。返回后仅核对实际指纹、正verified_at、trusted clock及已知/观测偏差<=2秒、取消/期限。端口自身有界I/O仍由组合根及Core保证，wrapper不抢占任意挂起调用。

依赖父已审Core5a1f5eb/bdc05b9、f2e8500/7623d94，已取公开代码，无未提交源依赖或复制。make_identity_rpc的六方法在3295e1f+5a69018，Issue随后独立cap。主入口/配置/CMake/PG和doc18不改，target/readiness状态不变；所有借用实例活到服务停止/Wait。

实际验证保留完整六RPC真实mTLS测试，第二台server使用本wrapper+真实Core IdentityAdminApplication+真实OpenSSL PeerCertificatePort，仓储/时钟和六方法app为明确fixture。覆盖CURRENT索引缺失/DB故障、主体禁用、head不一致、错SAN、不可信clock/与DB时刻差3秒、证书验证期间撤销，正常peer成功。监测UoW活动数，证明crypto/clock时为0，失权不调用六方法app；不冒充PG生产闭环。

临时证书必须符合X509_STRICT的CA签名KU及叶非CA/KU，测试在/tmp生成完整扩展，未降低生产校验。测试无秘密/异常正文输出。临时CMake链接实际Core Admin源及成熟PeerCertificate源；主集成测试需kiarabot_identity_crypto、OpenSSL::Crypto及Core Admin实现，由buildchat注册，不在仓库改CMake。

固定protoc29.3/Protobuf5.29.3/gRPC1.71/OpenSSL3.5、AppleClang21/C++20/Wall/Wextra/Wpedantic/Werror build通过，真实mTLS全套通过。inner/模块边界通过，clang-format16/clang-tidy22及状态/diff最终结果随后记录。未跑仓库full或真实PG/部署clock/bootstrap闭环，由父继续验收。

文档影响：doc01/02/08修正当前factory、单一Core权威和生命周期；wire/API无变化，不重复descriptor/API lint。受保护doc18只读，无生成物/证书/私钥提交。

最终结果：固定-Werror重编译、真实mTLS完整六方法及Core Admin+成熟crypto路径、format16、两TU tidy22全部analyzer/warnings-as-errors、inner/模块边界/状态文档/diff检查均退出0。9b96968必须与本重构同取，旧三参factory不再公开；CMake和PG实现未修改。
