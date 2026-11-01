# P02 五服务与公共进程支撑

基点 d20b54b，分支 codex/p02-services，限定新 Core target/platform 源、五个入口、
新测试、doc/02、doc/08；不改 CMake、Control 内部、proto/API、旧源码或保护基线。
文档影响：readiness/进程接口与新运行方式同步 doc/02、08 和本报告；
线协议沿 P01，不新增协议/API 字段。doc/19–23 只读依照，不改协调记录。

## 能力一：公共接口

ports/target/service_readiness.hpp 是纯 C++20 端口，Service 沿用既有六服务枚举。
结果含依赖状态与 ready；P02 应用必须含 BusinessImplementation/NotImplemented，
且 ready=false。端口线程安全、查询不执行 I/O，与 live/listen 分开。
runtime/platform/service_process.hpp 声明 kiarabot::core::platform::run_service，
借用端口至全部线程/socket/RPC 停止；CLI/退出码遵守协调记录。
后续五工厂统一签名 std::unique_ptr<target::ServiceReadinessPort> make_<name>_application()，
命名空间 kiarabot::core::target；头/源为 application/target/<name>_application.hpp/.cpp，
name 为 identity/runtime/host/scheduler/model。Control 可独立实现相同端口。

能力一验证：临时独立 C++20 -Wall/-Wextra/-Wpedantic/-Werror 头编译及执行、
clang-format --dry-run --Werror、git diff --check 均通过；未运行进程测试，runner 仅声明。

## 能力二：五个独立 application 与入口

新增 application/target 的 identity/runtime/host/scheduler/model 工厂和五个 *_main.cpp；
无共享可变全局/旧快照/协议依赖。依赖均 Unverified，BusinessImplementation 保持
NotImplemented，任何配置不能令它 ready。主 CMake 接线由构建任务拥有，本轮临时
编译五源与 service_applications_test.cpp 验证服务身份和不可绕过的未实现 gate。
文档更新 doc/02 与本报告，运维行为未超出能力一约定；proto/API 无变更。

能力二实际检查：五个 application 源和测试以 C++20 -Wall/-Wextra/-Wpedantic/-Werror
独立编译并执行通过；clang-format、边界检查、git diff --check 通过。入口链接待 runner。

## 能力三：严格配置与 TLS 材料验证

新增 runtime/platform/service_config.hpp/.cpp、platform/service_config_driver.cpp 和
service_config_test.py；使用 nlohmann JSON 与 OpenSSL，仅外层依赖。配置未知/重复/
类型/服务/地址错误退出2，TLS时间/链/密钥/文件权限故障退出1，无内容诊断。
文档更新 doc/08 与本报告；无协议变化，doc/02无需重复配置细节，其他判断同上。
实际编译：C++20 -Wall/-Wextra/-Wpedantic/-Werror，头路径 core/include、已有只读
build/full/dependencies/include 和 pkg-config openssl；运行
python3 core/tests/platform/service_config_test.py /tmp/p02-service-config-test 通过，
含正常、过期、错钥、错CA、缺文件、私钥权限、符号链接、未知/重复字段等14种输入。
本机 OpenSSL4 报旧时间API废弃，已改用兼容3/4的 ASN1_TIME_compare 后无警告编译。
测试证书先用系统CLI构造负days失败，改用显式历史起止日期签发后重跑通过。
TLS源需接线 OpenSSL::Crypto、nlohmann_json::nlohmann_json；尚未创建网络监听。

## 启动边界修正：FIFO 与工厂异常

read_file 使用 O_NONBLOCK|O_NOFOLLOW 后 fstat，仅接收 regular；补配置与全部TLS
字段的 FIFO 测试（各3秒超时，预期分别2/1）。五main工厂调用纳入 catch-all，
脱敏 INTERNAL退出1。仅更新上述源、配置测试、doc/08与本报告，无协议影响。

修正验证：配置driver以 C++20 -Wall/-Wextra/-Wpedantic/-Werror重编译，配置/TLS
测试（含4种FIFO路径）通过；五入口按相同警告门禁独立编译链接通过，格式及diff检查通过。
网络测试属于随后runner能力，本修正不据此宣称监听已验收。

## 能力四：真实探针、mTLS owner 服务与同步生命周期

新增 runtime/platform/service_rpc.hpp/.cpp、service_process.cpp、platform/service_rpc_client.cpp、
service_process_test.py。五个服务独立监听，Control可复用runner且不创建业务gRPC。
注册生成owner Service默认方法，Runtime另注册AdapterBridge/PluginBridge/HostBridge，
UNIMPLEMENTED零业务写入。mTLS要求验证客户端证书，禁grpc复用端口，未实现业务gate不变。
HTTP回环非阻塞有界读写，SIGINT/TERM同步消费，RPC关闭2秒deadline并Wait，无异步业务handler。

文档同步doc/02（边界/生命周期）、doc/08（配置/probe/退出/限制）和本报告；
API/proto未改，健康体沿用既有ProcessProbe，Control业务路径仅未实现拒绝；doc/18未改。

实际验证：
- protoc对当前proto目录生成全部目标C++/gRPC到/tmp/p02-services-generated（未提交）。
- 五application/五main/三platform源C++20 -Wall/-Wextra/-Wpedantic/-Werror编译，
  外部头标SYSTEM；分别链接五个/tmp/p02-{identity,runtime,host,scheduler,model}。
- 生成源不应用手写源码的-Werror/Pedantic；首次直接使用本机新Abseil头触发
  nullability-extension警告，改临时编译外部头为SYSTEM、生成源使用生成物警告策略后通过。
- /tmp/p02-rpc-client独立编译；
  python3 core/tests/platform/service_process_test.py /tmp /tmp/p02-rpc-client 经权限批准运行通过。
  覆盖五服务并行、help/参数拒绝、live/ready/404/405、五owner的UNIMPLEMENTED、
  无客户端证书握手拒绝、Runtime三个桥接UNIMPLEMENTED、两端口冲突、慢HTTP请求期间
  SIGTERM有界退出、其他四服务继续live及SIGINT退出0。测试使用临时CA/证书无生产秘密。
- 沙箱首次网络测试启动失败；首次获批运行发现TLS起始相等边界失败，RSA重跑因耗时较长通过；
  随后以独立提交修正notBefore相等边界并注入固定时刻测试，不用sleep绕过。

临时工具链为本机gRPC1.84.1/Protobuf36.2/OpenSSL4.1，不能代替full-locked依赖验收。
本worktree不修改CMake，阶段CTest注册、完整构建/编译数据库/固定依赖/格式tidy门禁由
工具链与主对话集成后执行；没有真实PG或P03业务验证，也未声明P02整体完成。

### 接线文件清单

- 纯库：core/src/application/target/{identity,runtime,host,scheduler,model}_application.cpp，
  对应include/kiarabot/core/application/target/*_application.hpp；各库只依赖纯readiness端口。
- 公共外层：core/src/runtime/platform/{service_config,service_rpc,service_process}.cpp，
  对应runtime/platform头；链接nlohmann_json::nlohmann_json、OpenSSL::Crypto、
  gRPC::grpc++、全部目标协议、Threads，不链接旧runtime或Control实现。
- 五main：core/app/{identity,runtime,host,scheduler,model}_main.cpp；最终Host文件为host_main.cpp，
  可执行名kiarabot-plugin-host，配置service为plugin-host。
- 单测：core/tests/architecture/service_applications_test.cpp；
  配置driver：core/tests/platform/service_config_driver.cpp，测试脚本service_config_test.py。
- 集成driver：core/tests/platform/service_rpc_client.cpp；测试脚本service_process_test.py
  当前接受临时binary目录（文件p02-*）与RPC client路径；统一CTest可用适配参数/别名接入。
- 本地可复跑临时构建脚本/tmp/p02-services-build.py保存实际编译命令，生成与对象均在/tmp；
  永久CMake接线以本清单为准，不将临时绝对路径写进仓库构建配置。

补充实际检查：clang-format --dry-run --Werror、check-boundaries.py、git diff --check通过。
check-architecture.py --inner 实际退出1：本分支尚未接收工具链对旧源码的隔离，报告25处
旧domain/application/ports依赖；新增target无报告，仍不能将全仓inner门禁记通过。
python3 -m py_compile 首次因系统PYTHONPYCACHEPREFIX指向只读Library/Caches退出1，
改为只解析AST验证测试脚本语法，不将系统缓存失败称通过。
集成脚本另支持直接传五个服务可执行路径（identity runtime host scheduler model）及
RPC client路径，适合CTest的TARGET_FILE接线，无需临时p02-*别名。

## TLS 起始时刻边界修正

valid_time允许notBefore比较结果-1或0，仍拒绝-2解析失败和notAfter相等/到期；
注入UTC验证时刻并同时用于OpenSSL链验证。driver测试恰好起始成功、前一秒拒绝、
恰好到期拒绝，另签发新EC证书后立即验证成功，无sleep。生产默认系统当前UTC，
CLI/配置不提供时钟覆盖。只改配置头/源、driver/配置测试、doc/08与本报告。
文档影响限TLS校验细节，无协议变化；其他文档无新增影响。

实际检查：C++20 -Wall/-Wextra/-Wpedantic/-Werror重编译配置driver；
配置/FIFO/TLS拒绝及固定时刻、新EC证书立即加载测试通过；格式和diff检查通过。

最终EC夹具回归：修正后重新编译全部手写源，按六个显式可执行路径运行集成脚本，
五服务/mTLS/桥接/信号/冲突全部通过。临时构建脚本最初仅按cpp时间戳缓存，头签名
新增时刻参数后出现旧对象链接失败及缺失binary；已强制重编译全部手写源后通过，
永久CMake必须跟踪头依赖，不能照搬此临时缓存。./scripts/check-toolchain.sh本轮实际
运行退出1：Required tool not found: clang-tidy，其后full/生成/tidy门禁未运行。
配置driver/测试与五应用单测另已运行通过；文档diff检查、Python AST语法检查通过。
