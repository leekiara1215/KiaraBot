# P02 独立验收

工作区kiarabot-contracts，分支codex/p02-acceptance，共同基点d20b54b；开始工作树干净。
目标：六服务公共CLI/配置、HTTP探针、mTLS拒绝、未实现RPC、有界停止和故障隔离黑盒测试。
允许tests/architecture/及本报告，禁止生产源/CMake/API/proto/其他模块说明和协调记录；不推送、不自动集成、不进入P03。

文档影响判断：新增tests/architecture/README.md、行为重建清单及本报告。只验证共享P02约定，不更改外部语义；doc/08/11与API/proto无需由本任务更新，生产行为说明由实现所有者同步。doc/18只读，doc/19–23不修改。

首个能力为可复用临时PKI：标准库驱动显式OpenSSL CLI，私有临时CA/六服务serverAuth叶子证书、可信/外CA/过期clientAuth及过期服务端证书；不读取生产CA、无固定生产端口或私钥提交。
验收包含链/用途/loopback SAN、私钥0600/目录0700、错误密钥与过期/外CA的具体原因、临时产物清理。此自测不是六进程实际运行。

首能力实际验证：`python3 tests/architecture/fixture_test.py --openssl /opt/homebrew/bin/openssl -v`，6项通过，实际OpenSSL4.1.0；不是doc/11固定3.5.0，固定环境仍待工具链提供。`python3 tests/architecture/check_references.py`核对24条旧有效行为源/锚点，全部pending_target_rebuild，未删除旧测试。`git diff --check`通过。六二进制当前未提供，未运行真实六进程；未运行full/tidy/统一toolchain，无C++/生产源改动且根接线由工具链任务拥有。
