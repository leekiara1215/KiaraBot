# Identity GetVerificationKeys RPC

独立分支codex/p04-verification-keys-rpc，前置父审Core151f31e及公共材料port47c23d8（本分支21e4cbf/e5f9c05）。本能力仅改RPC header/source、RPC test、doc01/02/08和本报告，不改CMake、纯Core/PG/主入口/配置或doc18。b954c50 Control delegator fixture适配已先单独提交，不混在这里。

冻结签名：

```cpp
struct IdentityRpcOptions {
  target::IdentityVerificationKeysApplication* verification_keys = nullptr;
};
make_identity_rpc(SecurityApp&, PeerAuthenticator, Issuer* = nullptr,
                  InvocationDeadline = {}, IdentityRpcOptions = {});
```

保持既有2/3/4参source兼容，新增能力用第五参具名IdentityRpcOptions{.verification_keys=&app}，不插bool或改变issuer/ownTLS次序。依赖均借用至服务stop/Wait。准确头已通过进度消息给父/组合根作者，后续Exchange可在此struct继续具名追加。缺能力仍完整RpcFailure/CAPABILITY_NOT_SUPPORTED，不返回空keyset。

GetKeys复用run(...,control_only=false)：实际mTLS/current peer及InvocationDeadline都保留，不要求只Control，Core用例负责当前platform和显式Catalog grant。没有JWT/delegation预检，也不解析context.delegation/kid/jku/x5u；否则unknownkid会导致验证公钥获取自举死循环。权限、catalog anchor和当前水位不从body取得，外层不读repos或另建keyring。

显式known=0在边界InvalidArgument；正uint64 optional不丢presence、不截断，future交Core在授权后判断。none/same/old完整返回current，成功不缓存也不假empty-delta。映射仅public DTO四字段：opaque key_id、array32原始bytes、正key/set uint64版本、target_codec精确Timestamp。验证当前Core单active key、可信Catalog ref、正checked_at、非空ID/非零版本和有限未来expiry，畸形成功Internal；Core材料过期/依赖失败原稳定上下文转发。没有private/configpath类型、无私钥读取/导出或本地材料构造。

实际独立/tmp构建使用fixedProtobuf5.29.3/gRPC1.71/OpenSSL3.5、AppleClang21/C++20/-Wall/-Wextra/-Wpedantic/-Werror。真实mTLS另server注入完整Core应用TEST double；现有真实Core Admin/Issuer与成熟PeerCrypto测试保留，但新GetKeys权限/材料为明确fake，不声称151应用PG或真实签名材料闭环。151应用自身验证已由其作者/父负责。

覆盖：无JWT成功；带unknown-kid context仍成功且session/issuer调用数0；Runtime平台peer可调用；none/same/old全current；uint64max集合及max-1键版本无损；raw32含零字节不转PEM/hex；纳秒123456789；zero known不调用app；future在拒绝权限时仍PermissionDenied，授权时InvalidArgument；依赖/过期材料失败；零版本/空集合/无限与过期expiry/坏Catalog/缺checked/空keyid/多active等畸形成功拒绝；InvocationDeadline失败保持原上下文且不调用app；无客户端证书握手不能到app。旧7方法/128/生命周期/Unsupported测试全部保留。

实际命令：cmake --build /tmp/kiarabot-p04-identity-rpc/build -j4；python3 core/tests/platform/identity_rpc_test.py <build/identity_rpc_check>；format16三文件dry-run；tidy22两TU全部clang-analyzer-*、warnings-as-errors及统一Apple SDK参数；inner/模块边界/status/semantic/API引用和diff。固定编译、mTLS全套及初轮检查通过，最终结果随后记录。

文档影响：doc01/02记录factory及GetKeys输入/授权边界/public输出/失败，doc08明确必须注入真实Core应用/材料用例和寿命；wire/API不变不重复descriptor/lint，无生产implemented/readiness状态提升。未运行full或真实GetKeys PG/成熟材料/HTTP链：CMake由buildchat，生产组合根与授权/生命周期由父验收。无生成物/证书/私钥提交。

最终固定-Werror build、真实mTLS完整旧套及新增GetKeys、format16、两TU tidy22全部analyzer/warnings-as-errors、inner/模块边界/status/semantic、46 API映射和diff检查均退出0。新增cap仅publicDTO映射/可选依赖与明确fake Core应用边界测试，无PG生产验收声称；生成物、证书和私钥均未提交。
