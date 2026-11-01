# P02 独立验收

工作区kiarabot-contracts，分支codex/p02-acceptance，共同基点d20b54b；开始工作树干净。
目标：六服务公共CLI/配置、HTTP探针、mTLS拒绝、未实现RPC、有界停止和故障隔离黑盒测试。
允许tests/architecture/及本报告，禁止生产源/CMake/API/proto/其他模块说明和协调记录；不推送、不自动集成、不进入P03。

文档影响判断：新增tests/architecture/README.md、行为重建清单及本报告。只验证共享P02约定，不更改外部语义；doc/08/11与API/proto无需由本任务更新，生产行为说明由实现所有者同步。doc/18只读，doc/19–23不修改。

首个能力为可复用临时PKI：标准库驱动显式OpenSSL CLI，私有临时CA/六服务serverAuth叶子证书、可信/外CA/过期clientAuth及过期服务端证书；不读取生产CA、无固定生产端口或私钥提交。
验收包含链/用途/loopback SAN、私钥0600/目录0700、错误密钥与过期/外CA的具体原因、临时产物清理。此自测不是六进程实际运行。

首能力实际验证：`python3 tests/architecture/fixture_test.py --openssl /opt/homebrew/bin/openssl -v`，6项通过，实际OpenSSL4.1.0；不是doc/11固定3.5.0，固定环境仍待工具链提供。`python3 tests/architecture/check_references.py`核对24条旧有效行为源/锚点，全部pending_target_rebuild，未删除旧测试。`git diff --check`通过。六二进制当前未提供，未运行真实六进程；未运行full/tidy/统一toolchain，无C++/生产源改动且根接线由工具链任务拥有。

## 配置、探针与有界关闭能力

新增run.py/process_support.py，六二进制与OpenSSL由参数/env显式输入，缺失CLI退出2；隔离继承的旧服务/代理配置，所有服务测试工作目录和证书仅tmp。按共享约定覆盖help0且无文件副作用、缺/未知参数、错误schema/service/未知字段/非回环probe、malformed JSON的精确2及配置诊断，失败不留下监听；正常live200与ready503完整ProcessProbe、未知路由404、Control业务不暴露，TERM/INT分别5秒内0退出且监听关闭。无真实二进制时不以夹具代替服务。

实际验证：support_test.py四项判定器自测通过（首次本地bind被sandbox拒绝，按环境要求提升权限后用127.0.0.1临时HTTP端口复跑通过）；缺OpenSSL入口直接返回2且无passed/skip。py_compile首次系统缓存目录权限失败，授权重跑通过；非生产代码无需C++/full/tidy。六服务lifecycle尚未运行，当前未提供P02二进制。文档只同步本目录README和本报告，生产接口/契约未改；没有额外模块文档影响。

## mTLS与未实现RPC能力

新增tls_acceptance.py、grpc_environment.json与tls_fixture_test.py。真实服务用例检查全部六入口缺文件/错格式/密钥不匹配/过期叶证书/错CA的精确退出1及TLS类诊断；五服务在可信mTLS通道上逐项检查77个owner方法和Runtime三条Bridge的UNIMPLEMENTED。无/外CA/过期客户端与错误信任根必须UNAVAILABLE，拒绝前后可信请求仍成功到达未实现边界且live正常。泛型bytes调用无需生成代码；不能凭UNIMPLEMENTED证明owner注册正确，也不宣称业务已实现。

实际验证：真实本地mTLS夹具4项通过；首次sandbox拒绝bind后经授权重跑。平台实际Python3.9/LibreSSL2.8.3、签发OpenSSL4.1.0；初版断言暴露LibreSSL服务器端错误分类差异，修正为仍严格核对客户端UNKNOWN_CA/CERTIFICATE_EXPIRED具体TLS告警（或OpenSSL服务器精确verify_code），无关闭验证。服务端错误CA verify_code允许19自签链/20未知签发链，hostname必须62。可信双向TLS有认证peer与应用响应，缺客户端必须PEER_DID_NOT_RETURN_A_CERTIFICATE。

真实grpc环境明确锁Python3.11.18/grpcio1.85.0，与SDK pyproject/uv.lock对齐；缺版本即失败。本工作区未提供该环境及P02六二进制，真实服务TLS分组未运行，不能报通过。未运行full/tidy/C++生成：本次只有Python验收与文档，无生产/协议变化；文档同步本目录README/本报告，其他模块/契约无需更新。
