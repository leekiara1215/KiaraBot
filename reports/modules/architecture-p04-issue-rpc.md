# P04 IssueDelegation RPC capability

## 范围与冻结签名

接续六方法3295e1f+5a69018、peer wrapper9b96968+86603fb，单独将父审Core f2e8500的IdentityDelegationApplication接入第七个generated方法。已取父proof契约1b962d3（本分支e3f6ac6）、Core f2e8500/7623d94（54e48c6/3e8d593）；未自行修改Issue/Exchange协议、纯Core实现或PG/CMake/主入口/配置。doc18只读。

```cpp
target::Outcome<std::unique_ptr<v1::IdentityService::Service>> make_identity_rpc(
    target::IdentitySecurityApplication&, PeerAuthenticator,
    target::IdentityDelegationApplication* issuer = nullptr);
target::Outcome<PeerAuthenticator> make_identity_peer_authenticator(
    target::IdentityAdminApplication&, target::SecurityClockPort&);
```

原两参调用依然编译，只有一个IdentityService注册；issuer可选借用，其生命周期与application/闭包捕获端口均覆盖server停止/Wait。缺issuer时完整typed Unsupported，Exchange不假实现。这是Control/Identity组合根应使用的准确签名，已在进度消息公开；实际构建接线仍由buildchat。

## 输入、输出与错误

subject_proof原样借用AccessSession token，拒绝空/含空白Bearer前缀，不做前缀剥离或将委托当session；真正Purpose/当前来源验证由Core的六方法应用负责。audience按名称封闭映射identity/runtime/host/scheduler/model/control。ResourceRef经target_codec按名和UUID校验；permissions通过typed getter及封闭codec映射，不数值cast内部枚举。拒绝空/重复资源、空/重复/未知/UNSPECIFIED权限；wire明确的product逐资源×逐权限一次展开为PermissionBinding，不从其他assignment扩权。

client_certificate_sha256长度必须32且匹配当前真实VerifiedPeer指纹，正文不能自选身份。Core issuer独立复核当前源会话/凭据/精确授予/授权水位，签名前后无UoW跨crypto，签后再验证来源。响应仅在应用成功后复制真实proof，验证reply claim与请求audience/tuple集合、证书、Control actor、一跳、正时间/来源ID及56秒上限的一致性；不合格结果Internal/Unknown，不造占位秘密。业务失败沿唯一RpcFailure全部语义保持。

1b962d3规定Issue/Exchange无持久幂等key，重复请求重新授权并可返回新proof ID/签名/期限，不消耗业务配额或一次性材料、不持久JWT普通收据。其他Login/Refresh/Revoke/Rotate保持keyrequired；边界不改其规则。

## 实际验证及限制

fixed protoc29.3重新生成当前四proto/gRPC（包括Issue/Exchange requires_idempotency=false），Protobuf5.29.3/gRPC1.71/OpenSSL3.5，AppleClang21/C++20/Wall/Wextra/Wpedantic/Werror独立/tmp构建通过。链接实际Core IdentityAdminApplication与IdentityDelegationApplication，实际OpenSSL PeerCertificatePort；session应用、仓储、随机源和签名端口明确fixture，不冒充PG/生产JWT证明。

在真实mTLS第三台server中验证：无key成功；2资源×2权限保留4个tuple；Control当前证书/actor/source credential/session/一跳/<=56秒正确；重复请求获得不同proof ID；Bearer/未知audience/空scope/重复资源/空、重复、零或未来权限/证书不匹配均拒绝且未签名；无权ActionExecute逐tuple拒绝；错误token purpose被session fixture拒绝；签名依赖故障保留UpstreamUnavailable；签后源失效不回proof。原六方法、成熟peer路径与所有未实现方法测试保留，缺issuer typed Unsupported及Exchange不变。descriptor额外断言Issue/Exchange keyfalse，而Login/Refresh/Revoke/Rotate仍true。所有测试均已通过。

实际命令：cmake configure/build `/tmp/kiarabot-p04-identity-rpc/build`；`python3 core/tests/platform/identity_rpc_test.py <该目录/identity_rpc_check>`；clang-format16三C++文件dry-run；clang-tidy22两个新增TU全部clang-analyzer-*、warnings-as-errors并同统一Apple SDK参数；inner、模块边界、status/semantic、Ruby46 API引用/映射和diff检查。结果通过。临时CMake需Core Admin/Delegation源与kiarabot_identity_crypto/OpenSSL，主仓库CMake不在本任务修改。生成物/证书/私钥未提交。

文档影响：仅doc01/02/08同步factory生命周期、输入product/证书绑定、proof重试/秘密隔离和未实现边界；唯一wire变更已在父1b962d3，无本能力新增字段/错误码/API，所以不重复API lint/descriptor全门禁。未运行full、真实PG或生产签名/HTTPS闭环：生产组合根、端口/密钥/clock部署由父验收，target/readiness状态不变。未触碰受保护doc18。
