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

## 并行、端口与崩溃隔离能力

新增isolation_acceptance.py，所有端口内核随机分配，不触碰生产监听。持有probe/grpc端口注入冲突，要求精确1与bind/listen/address/port诊断及无其他监听泄漏；随后六进程同时live、ready保持503，逐一SIGKILL，必须-9回收、受害者监听释放，余五连续三轮健康，再单独重启受害者。只向本脚本拥有的子进程/组发信号，finally回收资源。test_manifest.json向工具链提供7项入口、p02分类及timeout，不修改CMake。

实际验证：support_test.py扩至5项并通过，新增用真实自建Python子进程验证精确SIGKILL/兄弟存活与重复kill拒绝，属于故障注入判定器自测，不是六服务隔离通过。Python AST、24条reference检查及git diff --check通过。真实六进程isolation仍未运行：本工作区没有P02六二进制路径输入；不能使用临时HTTP夹具冒充服务。文档仅本目录README与报告；未改生产源、API/proto/模块文档，不运行full/tidy。

收尾只读核对已提交公共接口/配置实现（30eadfe、0b56cdf），没有合入或改写他分支：接口退出码与配置字段一致；invalid_address稳定诊断纳入配置负例类别。TLS/all在任何服务启动前预检锁定grpc环境；OpenSSL路径也必须可执行。上述改动仍是验收判定，无生产行为/契约影响。

## 首轮交付与复验

- 71c6982：临时PKI、6项证书自测、README、24条reference行为清单/检查及本报告。
- 2be1d77：run.py/process_support.py的配置/探针/TERM/INT黑盒分组、support_test判定器自测及说明。
- 799d03e：锁定grpc环境说明、TLS配置负例/77 owner RPC与3桥接状态、真实mTLS夹具4项自测及说明。
- 54cd092：端口冲突/六进程并行/逐服务崩溃及单独重启验收、故障判定器自测、7项接线manifest及说明。

最终再次实际运行fixture_test.py（6项）、support_test.py（5项）、tls_fixture_test.py（4项），共15项通过；网络自测按已授权的127.0.0.1临时端口权限执行。check_references.py的24项、AST及manifest入口/标签检查通过，整个基点差异git diff --check需在交付时复核。run.py显式提供OpenSSL但缺六二进制时实测退出2并标明missing executable for control，无skip/通过消息；当前非锁定Python被grpc预检明确拒绝。根CMake/生产/API/proto无修改，无生成物/密钥提交。

待主对话/工具链运行：将六个target实际路径、OpenSSL3.5.0及锁定Python3.11.18/grpcio1.85.0输入manifest三组真实黑盒验收，并注册p02 labels/timeout；六进程生命周期、真实RPC、绑定及崩溃隔离均尚未实测通过。固定依赖全量安装/full/tidy/统一toolchain不在本工作区执行；P00/P01/P02门禁未由夹具自测关闭，不推进P03。文档影响仅本任务README/本报告与reference清单，无外部契约变更，因此其他模块文档无需由本任务同步。

## reference迁移映射收尾

主对话要求兼容已审查59e1afd隔离后的路径。重构check_references.py为可自测函数，仅使用core/reference/manifest.json的明确映射：保持行为JSON的logical source/anchor原样，验证manifest/base/字段、限定Core来源与精确core/reference目的地、Git blob ID和无符号链接；拒绝缺失/错误/重复/未知映射、字节漂移、重新引入旧源及逃逸。无manifest迁移前仍读取原源；未登记且原源失踪绝不自动搜寻。未复制或合入工具链实现，无生产/契约更改。

实际验证：reference_test.py六项自测通过，覆盖迁移前/后、缺manifest/缺映射、缺目标、字节与锚点变化、错/未知/重复/逃逸映射、重新引入和symlink；现工作树24条逻辑行为检查通过。只读核对59e1afd的真实manifest全部路径/结构与Git blob；没有改变reference_behaviors.json以掩盖断言缺失。test_manifest新增p02/unit/reference自测入口（现8项）。文档影响仅tests/architecture/README和本报告，其他模块文档/协议无需同步。六二进制仍未实测，无P03推进。

## 独立任务：旧参考闭环wrapper路径修复

本任务获准增加tests/acceptance/run.sh及README范围，仍禁止生产/CMake/proto/API。修复构建根与reference目录混淆：CORE_TEST_BINARY固定构建根/reference/core/kiarabot-core且不回退旧位置；仅Control pytest子进程将KIARABOT_ACCEPTANCE_BUILD设为reference；CTest从构建根按reference标签运行且--no-tests=error；Ruby CONTROL_POLICY_BINARY仍为构建根/kiarabot-control-application-test；SDK和显式启用Adapter继承已导出的reference Core。保留全部原SDK/Control/Adapter测试调用及既有Adapter opt-in，没有新增skip或删除断言。成功文字明确旧参考闭环，不冒充六服务业务验收。

新增acceptance_wrapper_test.py六项自测（含多子场景），以tmp副本/命令记录夹具检查成功路由、带空格路径、原SDK/Control Python覆盖、缺环境、旧Core残留及policy错误位置不回退、SDK生成/Control/CTest/Ruby/SDK/Adapter生成与测试每阶段非零精确传播。首轮仅暴露macOS /var与/private/var路径别名差异，自测临时根改为canonical路径后通过，无生产逻辑放宽。manifest新增p02/unit/reference入口，现9项；根CMake不修改。

文档影响判断：更新tests/acceptance/README、tests/architecture/README及本报告；仅验收路由和闭环性质说明，没有生产公共I/O、状态或协议变化，所以doc/01/06/08/11及proto/API无需同步，确认基线未改。实际检查：bash -n tests/acceptance/run.sh、Python AST/manifest入口检查、acceptance_wrapper_test.py、check_references.py、git diff --check；真实SDK/Control/Adapter/reference CTest未在本任务运行，缺集成reference二进制/锁定环境，不能把命令夹具通过当业务通过。主对话报告的full/P02结果不作为本次本地执行证据。

补齐Adapter生成失败的负例后发现原`cd && generate && pytest`组合会使ERR阶段诊断遗漏（退出7仍保留）。改为同一隔离子shell内顺序执行，沿用set -Eeuo pipefail；生成失败仍不执行pytest，并恢复明确adapter tests阶段诊断。不是删除负面断言或忽略失败。

本任务最终实际结果：wrapper路由自测7项（含构建根误传reference、7个阶段失败子场景）全部通过；bash -n、24条行为引用、Python AST/9入口manifest和git diff --check通过。临时命令夹具全在tmp并清理。真实参考业务闭环/full/tidy未运行，原因仍为本任务无集成reference构建/锁定环境且无C++改动；仅交付路径修复和可审查路由验证。
