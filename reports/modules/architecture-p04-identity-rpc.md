# P04 Identity RPC boundary

## 目标、边界和依赖

在 `codex/p04-identity-rpc-service`（基 e34acd5）新增 Identity generated Service 适配器，不改纯 application/ports、PG、主入口、配置、CMake 或 doc18。不激活旧reference实现、不设置ready、不改target implemented登记。允许文件为identity_rpc.hpp/.cpp、identity_rpc_test.cpp/.py、doc01/02/08及本报告。

公共factory为 `make_identity_rpc(IdentitySecurityApplication&, PeerAuthenticator)`，返回Outcome<unique_ptr<IdentityService::Service>>，借用application并持有回调闭包。回调类型为function<Outcome<VerifiedPeer>(grpc::ServerContext&)>。缺回调拒绝构造；运行时先检查真实AuthContext及leaf-first证据，再调用可信组合根提供的当前登记/凭据/SAN核验，核对返回指纹、平台主体及凭据版本。六个已适配入口要求Control peer；其他owner方法没有业务实现，只返回稳定CAPABILITY_NOT_SUPPORTED。fixture的不可变测试登记仅证明边界，不是生产登记实现。

依赖a11716c冻结的IdentitySecurityApplication六方法与b0949b3内部类型、已审target_codec和peer_certificate；投影必须复用Core的project_assignment/default_role_allows/kPermissionNames，不在边界复制默认角色授权表。验证时Core policy header尚未单独提交，只读include其工作树公开头（Core HEAD a11716c，identity_policy.hpp blob dc1db46f5525d432df547964d29be9c8607efa99），没有复制未提交应用体到本模块。父集成须先落Core该helper；本提交不包含该头的副本或修改。

## 输入、输出和错误语义

Login/Refresh/Validate/GetPrincipal的密码/token借用SecretView，Revoke/List委托也仅借用输入正文；应用同步返回后不保留。输入request/trace为doc21§4.1 lowercase UUIDv4；坏关联用OpenSSL CSPRNG新UUID作错误关联，写幂等键非空且128字符封闭语法。有效期限取body、真实gRPC及5秒cap最早，缺传输期限默认2秒；检查认证前后取消/到期再向应用传明确deadline。可信回调必须有自己的有界I/O并尊重取消；同步函数无法抢占任意阻塞回调，不伪称已证明其生产SLO。

只返回纯应用成功的已提交结果，局部完整投影后swap响应。Timestamp经target_codec保留精确纳秒，版本/rotation保持uint64最大值。持久created_at/expiry/checked_at/version/binding缺失拒绝，不填now/iat/0。Token结果必须有实际秘密及原expiry，不从旧状态或占位返回。PrincipalRecord.binding是Control/公共投影必需字段，内部Principal虽optional，此边界缺失即Internal，不能从subject虚构；Core成功路径须提供持久binding。

Assignment由Core helper验证/投影：role行同role、无explicit；explicit行每个scope资源具有同一permission集合，拒绝无法安全表示的稀疏pair product。每条subject/ID独立，其他主体、重复ID或不合法resource/version拒绝。仅显示roles/permissions并集，不把显示scope做笛卡尔授权。List拒绝输入next_page_token/expires_at，cursor与snapshot_id成对，page<=100；输出页大小、snapshot、subject、有效期及会话元数据核对，跨主体/selfSession权限仍由应用决定。

Failure编码使用唯一RpcFailure二进制详情：Acceptance三值保持，operation owner/resource/id和retry_after保留，无current_version来源则不编造。Recovery名称映射：CorrectInput→CORRECT_INPUT、Authenticate→REAUTHENTICATE、ReadCurrent→RESYNC_SNAPSHOT、QuerySameIdentity→QUERY_SAME_INTENT、Backoff→RETRY_SAME_ID、Diagnose→MANUAL_REVIEW。Unknown不可通过修正输入/重新认证等建议换新键重做；畸形组合转Internal但保留Unknown和原身份查询。异常发生在应用调用后保守Unknown，调用前NotAccepted，不输出what()/秘密。极端CSPRNG/分配故障无法形成安全关联时仅脱敏transport INTERNAL，不伪造业务容器/ID。

## 实际验证

临时 `/tmp/kiarabot-p04-identity-rpc/CMakeLists.txt` 生成compile DB，fixed protoc29.3生成当前common/platform/policy/identity及grpc到tmp，链接固定Protobuf5.29.3/gRPC1.71.0/OpenSSL3.5.0。AppleClang21/C++20，手写TU严格`-Wall -Wextra -Wpedantic -Werror`；生成物独立库不手改、不提交。

```sh
cmake -S /tmp/kiarabot-p04-identity-rpc -B /tmp/kiarabot-p04-identity-rpc/build \
  -DCMAKE_PREFIX_PATH=<reviewed architecture-installed/arm64-osx> -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/kiarabot-p04-identity-rpc/build -j4
python3 core/tests/platform/identity_rpc_test.py /tmp/kiarabot-p04-identity-rpc/build/identity_rpc_check
clang-format --dry-run --Werror <三份新增C++>
clang-tidy -p /tmp/kiarabot-p04-identity-rpc/build --checks='clang-analyzer-*' \
  --warnings-as-errors='*' <两个新增TU，带统一Apple SDK参数>
python3 scripts/quality/check-architecture.py --inner
python3 scripts/quality/check-boundaries.py
python3 proto/tests/check_implementation_status.py
git diff --check
```

真实临时CA和双向TLS server/client调用全部六方法，包含每方法11稳定码、Unknown完整operation/retry_after、无凭据peer/登记拒绝、授权依赖故障、超时。另覆盖纳秒/uint64max、缺AuthContext不能被回调伪造、无客户端证书握手拒绝、坏关联/坏时间/幂等键/分页、所有其余方法（含流式）稳定未实现、不调用应用、应用决定跨主体拒绝、坏Assignment product/他人绑定、缺created/version/checked/binding、失效expiry、缺秘密、异常脱敏、危险Unknown恢复建议，以及默认2s/max5s/真实传输更早/body更早/回调返回已过期不调用应用。首轮失败定位为测试human fixture未填公共必需binding，已修fixture而非放宽生产校验；source_location只输出行号，诊断不含秘密。

文档影响：同步doc01/02公共边界、投影和失败语义，doc08同步生产注入及生命周期要求。proto/API字段未变化，因此不重写协议README/API schema或运行wire lint；现有target状态不变。doc18只读；本轮新增适配器不使旧基线运行路径成为新生产实现。

未运行仓库full/check-toolchain/PG生产闭环：本任务明确不改CMake，buildchat负责最终注册，真实当前登记/业务应用/PG组合根由父任务另验。以上网络测试纯应用与verifier均为明确fixture，不证明生产认证/撤销/数据库事务已通过。

最终结果：固定依赖-Werror rebuild、真实mTLS全套测试（包含Accepted/NotAccepted/Unknown、全部Recovery映射、所有未实现owner入口）、clang-format16 dry-run、两新增TU clang-tidy22全部analyzer/warnings-as-errors、inner/模块边界/状态文档门禁均退出0。Core policy依赖blob复核未变。stage后git diff --cached --check再次通过；生成物/测试证书/临时CMake均未提交。
