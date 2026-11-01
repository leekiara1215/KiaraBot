# P02 六进程黑盒验收

本目录只拥有验收与临时夹具，不修改生产实现、契约或构建。共享CLI/配置/probes/退出码以
`reports/execution/2026-11-01-architecture-p02.md` 为准，未来由工具链任务接入CTest的p02标签。
任何前置缺失均失败；夹具自测通过不表示六个服务已通过。

输入：显式指定六个二进制或逐服务环境变量、OpenSSL路径及可用的本机临时端口权限。
输出：逐能力断言和明确失败退出；CLI/配置预期2、运行/证书/绑定故障1、正常信号关闭0。
负面用例必须检查具体退出码及对应诊断/传输状态，不能把任意非零视为通过。
验收期限：启动最多10秒、单次HTTP/RPC最多2秒、TERM/INT退出最多5秒，超时直接失败。

证书自测：`python3 tests/architecture/fixture_test.py --openssl /path/to/openssl`。
TestPki只在TemporaryDirectory生成0700测试CA目录/0600私钥，六服务serverAuth证书含
localhost/127.0.0.1 SAN，单独生成clientAuth、错误CA及固定2000年已过期证书。
不读取生产CA，不关闭证书/主机名校验，不等待系统时间或修改全局时钟。退出清理全部产物。
OpenSSL路径必须明确给定；记录实际版本，本机不同版本自测不能冒充doc/11锁定环境。

计划分组：配置/live/ready/关闭；mTLS/UNIMPLEMENTED RPC；六进程并行/绑定冲突/崩溃隔离。
帮助必须0且不监听；缺/未知参数、错误schema/service及未知配置字段必须2；缺/错/过期
证书或端口冲突必须1。探针live200/HEALTHY、ready503/UNHEALTHY，未知路径404。
Control只提供回环HTTP探针且拒绝业务REST/WS；其他五服务mTLS gRPC未实现方法返回
UNIMPLEMENTED，不产生业务状态。证书缺失客户端拒绝不能仅用RPC超时证明。

真实服务入口（六路径必须分别指定，不能只提供一个旧Core）：

```sh
python3 tests/architecture/run.py --group lifecycle --openssl "$TEST_OPENSSL" \
  --control "$CONTROL_BINARY" --identity "$IDENTITY_BINARY" --runtime "$RUNTIME_BINARY" \
  --plugin-host "$HOST_BINARY" --scheduler "$SCHEDULER_BINARY" --model "$MODEL_BINARY"
```

也可用KIARABOT_P02_OPENSSL及KIARABOT_P02_BINARY_CONTROL/IDENTITY/RUNTIME/PLUGIN_HOST/
SCHEDULER/MODEL逐项提供；无内置构建路径。分组lifecycle/tls/isolation/all，缺任何一个可执行
直接CLI退出2，绝不skip。lifecycle不需要grpc Python包；tls要求明确锁定的grpc环境。
CLI/配置失败要求退出2且诊断属于参数/配置类别；证书/绑定故障要求退出1并识别相应诊断。
子进程继承环境会删除旧KIARABOT/CORE/ONEBOT入口及HTTP代理，配置只引用当前临时证书。
探针检查ProcessProbe完整形状，不能将live当ready；未ready与未知路由返回正确状态码。

`python3 tests/architecture/support_test.py -v` 自测验收判定器：错误退出码/无关诊断被拒绝，
PEM/DSN泄漏被拒绝，缺依赖不skip，真实本地HTTP夹具验证状态/JSON形状及关闭探测。
该HTTP夹具只测判定器，绝不作为六进程结果。网络受限时申请本地临时端口权限，不改测试跳过。

TLS分组读取grpc_environment.json，必须Python3.11.18/grpcio1.85.0（现SDK锁），不匹配立即失败。
先运行`uv sync --project sdk/python --locked --python 3.11.18`，再使用该环境执行run.py。
泛型序列化bytes RPC不需要Protobuf生成器/runtime；不会偷偷回退grpcurl或未锁系统包。
五个owner service的77个方法与Runtime三条桥接流逐项验证UNIMPLEMENTED；无/外CA/过期
客户端以及错误服务端信任根必须UNAVAILABLE，前后可信mTLS RPC仍UNIMPLEMENTED且live正常，
防止把服务崩溃/错误端点冒充认证拒绝。UNIMPLEMENTED状态本身不能证明服务已注册，owner
注册/link图仍由Core及工具链独立检查；这不是业务授权或收据正确性的验收。

`python3 tests/architecture/tls_fixture_test.py --openssl "$TEST_OPENSSL" -v` 执行真实mTLS握手，
验证可信客户端成功，无证书、错误CA、过期客户端的具体TLS告警，服务端信任根和主机名核验。
无证书拒绝要求PEER_DID_NOT_RETURN_A_CERTIFICATE；链错误明确verify_code19/20，过期10；
本机LibreSSL在服务器侧可能返回含糊原因，此时必须在客户端观察UNKNOWN_CA/CERTIFICATE_EXPIRED
对应TLS alert，任意EOF/超时不算通过。该夹具自测不能替代五服务真实grpc测试。

isolation分组先持有内核分配的loopback监听，分别验证六服务probe端口冲突及五服务gRPC端口
冲突的精确退出1和绑定类诊断，检查另一个已启动的监听也被关闭。随后同时启动六进程，确认
PID独立、live/ready；依次对每个服务的直接子进程注入SIGKILL，必须精确-9回收、释放监听，
其余五个进程连续三轮仍live200/ready503。只重启被注入的服务，重新检查六进程，再统一TERM。
故障注入只作用于本脚本自己启动的子进程；finally回收进程组和所有socket/临时文件。

工具链接线建议见test_manifest.json：七个独立入口及p02/unit/integration/fault/reference标签、
明确timeout。tests/architecture不提供CMake、不创建或猜测构建目录；工具链传入各target的实际
路径。证书夹具/oracle通过与真实lifecycle/tls/isolation通过必须分开报告，不能只跑unit就关闭P02。

reference_behaviors.json保留逻辑source与原锚点。check_references.py优先按
core/reference/manifest.json的明确source→reference映射读取，逐项核对Git blob ID，
不得猜测目录或跳过失踪断言。迁移前无manifest时读取原路径；有manifest但逻辑源未映射
则只能读原路径，原路径缺失即失败。拒绝未知字段/来源、错目的地、重复映射、缺文件、
字节变更、原文件重新引入及symlink/越界路径。映射本身不改变pending_target_rebuild状态。
`python3 tests/architecture/reference_test.py -v`在tmp验证迁移前/后及上述失败路径；
工具链接线清单增加architecture_p02_reference_resolution，当前共八项入口。

旧参考闭环wrapper的路由自测为`python3 tests/architecture/acceptance_wrapper_test.py -v`，
manifest增列architecture_p02_acceptance_wrapper（当前9项入口）。只在tmp复制shell入口并
记录命令argv/env/cwd：构建根与reference分开，Control子进程用reference目录，CTest仍在根
选择reference标签，Ruby policy binary仍在根，SDK/已启用Adapter沿CORE_TEST_BINARY使用
reference Core。覆盖带空格路径、旧位置陈旧产物不回退、缺锁定环境失败与每阶段失败传播。
此命令路由夹具不执行真实业务断言，不属于六服务验收通过证据。
