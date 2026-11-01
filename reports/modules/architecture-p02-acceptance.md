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
