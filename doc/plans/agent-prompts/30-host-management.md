# 提示词：Host维护、身份链与独立Operation

你接手KiaraBot HOST工作流，目录`/Users/betta/Workspace/KiaraBot`。
先读`doc/plans/2026-11-04-next-agent-prompts.md`及Host源码交接，按AGENTS读规范、
doc/01/02/08、host-maintenance-storage子文档、v1.1冻结记录。你不是唯一编辑者，保留所有既有改动。

先修当前真实失败，不继续堆功能：HOST-02 maintenance测试在停止进程后进度写入失败恢复场景失败。
精确诊断为短grace后KILL errno1，但随后同marker进程已消失；原HostProcess测试通过。
host_process.cpp有临时诊断。核对实际waitpid、marker与signal竞态，不把alive=false或EPERM当作成功。
只有原owned child确认退出的证据才能收敛；仍活着/无法确认保持Failure与UNKNOWN。
去掉临时打印，补0/1/10ms、正常/强制停止、signal拒绝、写失败后恢复与未知执行不开放接纳的测试。

有限构建/测试：
cmake --build --preset full-locked --parallel 2 --target kiarabot_host_maintenance_test kiarabot_host_process_test kiarabot_host_service_test host_journal_test
ctest --preset full-locked -R '^p08_host_(maintenance|process|service|journal)$' --no-tests=error --output-on-failure
共享构建必须由统筹统一执行，或你成为唯一构建持有人后执行。

然后接完新Identity生产身份链。已有纯应用/RPC/CLI/config改动未统一编译，以交接清单为准。
显式Host enrollment使用独立host_id与已有RESOURCE_HOST binding，不假定HostId=PrincipalId，不新增无关DDL。
窄CheckHostGateAuthorization仅供当前Runtime实际mTLS和自身Principal上的检查能力调用，
固定检查Host当前叶证书、注册与host.execute，不允许任意method/permission/principal输入。
原CheckAuthorization的proof语义及Scheduler白名单不削弱；不能用假授权填空。
用真实Identity/证书验证撤销、错Host、错binding、非Runtime和当前水位；纯fake只能作局部测试。

再接Runtime SetHostAdmissionGate的真实持久禁派、当前Host注册/Fence/维护意图/单调版本验证，
以及Host Runtime客户端、HostManagement RPC、Host组合根、Control host routes/operation owner。
维护先持久受理→本地禁接纳→Runtime禁派→有限真实执行排空；Unknown/离线不能假定停完。
resume是明确新操作，不复活任务/凭据/旧代次。Host operation真相在Host SQLite，不跨库。
Control按固定注册endpoint和精确Host scope路由，普通REST不返回秘密。

你持有Host应用/本地存储/进程适配、Identity应用/RPC/CLI与service_config相关改动。
WS所需websocket_origins/missing_origin配置已部分落盘，保留并统一验证；文档同步doc08。
RuntimeRPC/资源应用/PG迁移/CMake与BOOT、WS、Model存在共享锁，先由统筹串行分配。
I/R wire与registry当前归WS v1.2，不能并发编辑；必要契约修正先交统筹。

验收覆盖真实TLS+PG+SQLite+Host进程、撤权/依赖故障、ACK/响应丢失、重启、重复/冲突、
drain/recover、当前scope隔离和Control owner查询。通过后释放Runtime/Control文件锁给BOOT，
报告精确检查与未闭项，不将本地typed gate fixture当作生产链证据。
