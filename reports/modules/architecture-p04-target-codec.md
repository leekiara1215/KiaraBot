# P04 target transport codec

## 范围与 API

仅修改 `core/include/kiarabot/core/runtime/platform/target_codec.hpp`、`core/src/runtime/platform/target_codec.cpp`、`core/tests/platform/target_codec_test.cpp`、doc01/doc02最小说明和本报告。未改 CMake、PG、业务、其他传输或受保护 doc18。命名空间为 `kiarabot::core::platform::target_codec`；头文件在首次实现前已给出API草案。

- `to_wire/from_wire` 重载：纳秒TimePoint/Timestamp、ResourceKind/ResourceRef、封闭Permission、稳定ErrorCode。
- `http_status/grpc_status`：target_spec.json既有映射；逆向必须同时有稳定Error，不能凭歧义503/UNAVAILABLE猜码。
- `make_error`：稳定码、合法request/trace ID、显式retryable，固定脱敏message。
- `to_grpc_failure/from_grpc_failure/from_http_failure`：完整合法RpcFailure及匹配传输码。target unary唯一gRPC标准grpc-status-details-bin详情由Status.error_details承载序列化kiarabot.v1.RpcFailure；无Error-only fallback或文本推码。from_http_error只验证Error成员。

公开函数返回Outcome并noexcept；无效/未知/UNSPECIFIED返回InvalidArgument，分配/解析异常转Internal，受理为NotAccepted。这只是本地转换失败，不是远端执行结论。RpcFailure完整保留业务acceptance/recovery/operation/retry_after/current_version，由owner提供事实而非codec默认。生成类型只进入外层runtime边界。

时间用规范Timestamp与signed int64纳秒精确转换，覆盖负秒floor和两端溢出，不截断；显式epoch合法，缺失消息由调用方has_*先拒绝，禁止默认占位。ResourceKind显式名称表，Catalog内部15/wire16；Permission含MessageRead全部按名映射，无数字cast授权。资源ID除Action/Task沿来源规则外为小写UUIDv4，非空/长度/字符/版本位/variant位都校验；转换不证明资源存在、owner或当前授权。

## 构建与验收

不触碰仓库CMake；独立临时 `/tmp/kiarabot-p04-codec/CMakeLists.txt` 生成compile DB、编译proto和两个手写TU。固定依赖来自toolchain已安装architecture前缀：protobuf 5.29.3/protoc29.3、gRPC1.71.0；AppleClang21、C++20、`-Wall -Wextra -Wpedantic -Werror`。clang-format16.0.6及clang-tidy22.1.8。临时生成物不入库。

本分支尚未整合Core P03/P04，独立构建只读使用Core工作树 `core/include`（d1ba916基线加正在提交的MessageRead枚举），未复制/修改Core源。主集成需先接纳Core signed-int64纳秒/ResourceRef/Permission基础类型及MessageRead小修，再接codec提交；不能单独在旧基础类型分支编译。

实际命令（路径仅用于报告，不写产品配置）：

```sh
# fixed protoc对common/platform/policy生成到/tmp/kiarabot-p04-codec/generated
cmake -S /tmp/kiarabot-p04-codec -B /tmp/kiarabot-p04-codec/build \
  -DCMAKE_PREFIX_PATH=<toolchain architecture-installed/arm64-osx> -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/kiarabot-p04-codec/build -j4
/tmp/kiarabot-p04-codec/build/codec_check
clang-format --dry-run --Werror <三份新增C++文件>
clang-tidy -p /tmp/kiarabot-p04-codec/build --checks='clang-analyzer-*' \
  --warnings-as-errors='*' <两个手写TU及与统一脚本相同Apple SDK参数>
```

测试逐项核对全部40个权限、16种资源、11种错误及descriptor完整数量；包含未知和UNSPECIFIED、UUID/source ID正反例、int64纳秒min/max/负1/0及越界、Timestamp非法nanos/年份、稳定二进制Error关联ID/retryable保留、歧义码区分、缺/坏详情/错status/空correlation拒绝。未知值经真实protobuf parser读取，保留负例且无NOLINT或禁用analyzer规则。编译与codec_check通过。初次tidy报告负例中的常量越界enum cast，已用真实wire未知值输入修正，未削弱断言；新增noexcept静态断言用declval排除参数构造异常后编译通过。

文档影响：只同步doc01/doc02说明公共边界与错误/时间约束；proto/API契约未改变，故无需更新README、模块运行/REST/WS schema或重复wire/lint。业务认证、授权、TLS listener接线与数据库并发不在此测试范围。仓库full/check-toolchain不运行：授权明确不改CMake，新增TU接线由buildchat最终完成；本报告不声称全量绿或认证已实现。

最终结果：两TU clang-tidy22.1.8（全部clang-analyzer-*、warnings-as-errors）退出0，clang-format16 dry-run退出0；最终-Werror rebuild与codec_check退出0。check_implementation_status及git diff --check通过。所有新增文件在stage后再次diff检查，无生成物/构建目录/本机配置入库。


## 后继修正：唯一 target unary RpcFailure

父审指出初版错误地沿用了reference Connect的Error-only binary详情；96f88ce必须与本后继修正一起集成。已移除公开to_grpc_error/from_grpc_error，target unary仅to_grpc_failure/from_grpc_failure，无第二格式或自动fallback。完整容器必填error/context；acceptance/recovery为封闭非零枚举；current_version若存在为正；operation若存在要求登记runtime/scheduler/model owner、UUIDv4 ID、完整合法资源。不把可选operation强设必填，Unknown Login/Refresh保留原意图恢复事实。

新增测试覆盖3种acceptance与7种recovery、Unknown/NotAccepted/Accepted完整binary/HTTP往返、最大current_version、operation/retry_after保留；缺context/error、unknown/UNSPECIFIED、零version、坏owner/ID/resource、grpc状态失配和旧Error-only详情均拒绝。没有改wire/API唯一契约。最终固定依赖-Werror rebuild和codec_check通过；clang-format16 dry-run与两TU clang-tidy22（全部analyzer、warnings-as-errors）均退出0。status文档门禁与git diff --check通过。文档仅修正doc01/doc02 codec说明及本报告，doc18未动；不声明认证业务或整体接线完成。
