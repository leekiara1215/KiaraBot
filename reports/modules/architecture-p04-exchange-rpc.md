# P04 ExchangeDelegation RPC 适配验收

## 范围与依赖

目标是 Core runtime/platform 的 Identity gRPC 外层。允许文件为 identity_rpc.hpp/.cpp、
对应平台测试、直接契约/模块/运维文档及本报告。未修改生产 CMake/main、配置、PG、
纯 Core 用例、proto/API schema、生成物或受保护 doc/18。

基于父分支 6ab7e76（含 Core Exchange a0b7f90/79cf491），先承接独立 GetKeys
83cc837（本分支 1c77c75），再承接 Core 跨 UoW UTC 回退修复 ea433b7（本分支
e7027a7）。Exchange RPC 变更可以单独审取，但依赖上述公共用例/端口。

工厂原有参数不动，第五参 IdentityRpcOptions 新增可选 borrowed exchange 指针，必须
活到服务停止/Wait。缺省保持完整 typed CapabilityNotSupported。真实当前 Runtime mTLS、
model/scheduler 受众、原始 parent_proof、实际 Runtime 叶指纹、封闭去重 scope/permissions
是输入；输出仅为 Core 证明及公开 metadata。最多128 exact tuples，在解码/展开前拒绝
超额，不要求普通写入幂等键。完整 Failure 仍经统一 RpcFailure 映射，未发生 receipt/write。

父 Control 证据与当前 Runtime TLS 证据不混用。外层没有 JWT 解析、验签策略、权限决策或
当前身份仓储查询；实际 Core Exchange 负责两平台显式能力、来源业务权限、签前后重查和
期限。可选来源 session 缺省保留，不能误用 Issue 的强制人类 session 规则。

## 文档影响判断

更新 doc/01-contracts.md、doc/02-core.md、doc/08-operations.md：描述可选接线、输入
校验、错误传播、期限和验收边界。proto/README.md、proto schema、API、doc/06-control-api.md
无需变更：wire/HTTP/WS 契约和既有语义未变化，尚未宣称生产可用。doc/01-runtime-semantics.md
无需更新：无新增投递、重试或收据语义。doc/00-boundaries.md 无需更新：责任方向不变。
受保护 doc/18 只读，本次没有提出架构基线变化。

## 验收与实际检查

独立临时 CMake 位于 /tmp/kiarabot-p04-identity-rpc，未写入仓库；使用固定工具链
Protobuf 5.29.3 / gRPC 1.71、AppleClang、C++20 和 -Wall -Wextra -Wpedantic -Werror，
链接实际 Core Admin/Issue/Exchange、实际成熟证书验证和当前生成 wire。

- `cmake -S /tmp/kiarabot-p04-identity-rpc -B /tmp/kiarabot-p04-identity-rpc/build -DCMAKE_PREFIX_PATH=<fixed architecture-installed/arm64-osx> -DCMAKE_BUILD_TYPE=Debug`
- `cmake --build /tmp/kiarabot-p04-identity-rpc/build -j4`
- `python3 core/tests/platform/identity_rpc_test.py /tmp/kiarabot-p04-identity-rpc/build/identity_rpc_check`

以上通过。真实 mTLS 回归覆盖原六 RPC、Issue、GetKeys，并新增实际 Exchange 用例：
Model/Scheduler 两跳成功；Control/Runtime 不同指纹；来源/链/tuples/expiry 保持及收窄；
128成功/129预检拒绝且签验/签名/随机数无调用；非法受众、空/超长/Bearer 父 proof、指纹
不匹配、重复 permissions、错误平台；父验证失败、签名依赖失败、能力缺失、签后撤权、
来源 session 撤销及无 session 的非人类来源。拒绝验证完整 typed detail 且没有部分 proof。
仓储/随机/签验均明确 fixture；不代表生产 PG、JWT、组合根或部署验收。

以下静态与文档检查全部通过：

- clang-format 16 dry-run/Werror：本次3个手写 C++ 文件。
- clang-tidy 22：两个翻译单元，clang-analyzer-*、warnings-as-errors、实际临时编译数据库。
- `python3 scripts/quality/check-architecture.py --inner`
- `python3 scripts/quality/check-boundaries.py`
- `python3 proto/tests/check_implementation_status.py`
- `python3 proto/tests/check_semantic_rules.py`
- `ruby proto/tests/target_api_test.rb`（46 HTTP operations）
- `git diff --check`

未运行全仓库 scripts/check-toolchain.sh/dev/full preset：本任务只接外层可选能力，生产
CMake 接线由父任务统一验收；本报告不把局部编译/真实 TLS fixture 等同于整体生产可用。
协议无改动，未重复生成/lint wire。后续若合并 crypto6a311c5 新 pure port，新增的
ExchangeCryptoFixture 未使用 verify_for_authorization，必须像其他严格替身一样显式抛错，
不得伪造成功 claims。
