# Identity RPC invocation deadline from own TLS lifecycle

## 冻结接口与范围

```cpp
using InvocationDeadline = std::function<target::Outcome<target::TimePoint>()>;
target::Outcome<std::unique_ptr<v1::IdentityService::Service>> make_identity_rpc(
    target::IdentitySecurityApplication&, PeerAuthenticator,
    target::IdentityDelegationApplication* issuer = nullptr,
    InvocationDeadline invocation_deadline = {});
```

保留两/三参fixture源兼容，生产composition必须传第四参，提供实际selected自身TLS链最早有效期和当前可信clock gate。回调由Service持有，其捕获对象寿命覆盖server停止/Wait。准确签名已在进度消息公开，供父转Control/Identity composition作者。PeerAuthenticator仍仅表示真实远端peer，不混入Identity自身证书期限。

仅修改identity_rpc.hpp/.cpp、identity_rpc_test.cpp、doc01/02/08和本报告。不改CMake、主入口、配置、Core用例/ports、PG、证书/clock生产者或doc18。没有新增wire字段、错误码或协议格式；target/readiness状态不变。

## 行为

run先保持原body/gRPC/default2秒/max5秒和取消/期限检查，再调用可信InvocationDeadline。Failure原语义经现有完整RpcFailure编码，未知状态不假NotAccepted；回调异常在应用未被调用时返回稳定Internal/NotAccepted，不记录异常正文。非正值拒为UpstreamUnavailable，过去值拒为Timeout/NotAccepted；有效值只与既有预算取min。回调后及peer认证后再次检查到期/取消，再把受自身TLS限制的context.deadline传入所有Core用例，让其final_check拦提交。不读取repo、不判断证书链/时钟业务状态，不把own expiry塞入Control证书或扩大现有期限。

## 验证与限制

沿固定/tmp构建：AppleClang21/C++20/Wall/Wextra/Wpedantic/Werror，Protobuf5.29.3/gRPC1.71/OpenSSL3.5。真实mTLS server/client使用明确的fakeLifecycleDeadline闭包，不冒充实际生产链到期检测。测试过去期限、回调Unavailable（正确gRPC稳定详情及HTTP503映射/retry_after）、抛异常（Internal且NotAccepted）、零时间，全部无应用调用；较早的own limit精确传给App，远端Control cert.not_after保持原值，较晚limit不能放大body/transport期限。默认空回调在其他真实mTLS server继续覆盖原七方法/成熟peer/128正常129拒绝等全部旧断言。

实际命令：cmake --build /tmp/kiarabot-p04-identity-rpc/build -j4；python3 core/tests/platform/identity_rpc_test.py <build/identity_rpc_check>；clang-format16三文件dry-run；clang-tidy22两TU全部clang-analyzer-*、warnings-as-errors、统一Apple SDK参数；inner/模块边界、状态文档及diff检查。固定编译和mTLS全套通过，最终检查结果随后记录。

文档影响：doc01/02同步公共factory/失败/预算，doc08声明生产必须注入实际TLS/clock回调与生命周期；proto/API不变不重复descriptor/lint。未跑仓库full或真实自证书到期/PG提交闭环：生产callback由composition/CurrentActorOwnTLS作者提供，Core final_check和真实daemon链由父验收。没有复制未提交代码、证书/私钥或生成物。

最终固定-Werror build、真实mTLS全套（含生命周期fake callback及原7方法/128边界）、clang-format16、两TU clang-tidy22全部analyzer/warnings-as-errors、inner/边界/状态文档和diff检查均退出0。所有新增文件stage后检查无生成物/证书/本机配置；生产callback实际链/clock门禁仍由composition提供，不据fixture宣称已部署。
