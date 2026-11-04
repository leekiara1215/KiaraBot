# Host/Bootstrap 扩展：实现切片证据

日期：2026-11-04。工作树基于0496b900及既有未提交重构；本报告只记录新增切片，不声称正式发布或整族能力退出。

## HERR-02 与 BOOT-03

HERR目标PluginBridge已增加wire-boundary形状前置拒绝：缺操作INVALID_ARGUMENT；单未来length-delimited
候选CAPABILITY_NOT_SUPPORTED；已知+未知候选、多候选、错wire type INVALID_ARGUMENT。区外metadata兼容，
已知oneof保留last-one-wins。reference blob/manifest未改，未接模型stream实际处理或提前协商。
真实TLS fixture直接发送raw-wire解析后的请求，拒绝路径Model/Task/Storage调用数均零；同流后续合法
model_chat+tag128只派发一次，重复已知oneof只派发最后操作。

BOOT交付模块已实现纯SecretDeliveryApplication/Port及固定recipient文件sink，绑定资源与peer，不接受
请求自报路径/owner/env；采用私有目录fd、每组件NOFOLLOW、0600文件、非阻塞peer flock、intent先行、
版本CAS、目标fsync/rename/目录fsync、无秘密receipt/query。进程崩溃后只有目标摘要与当前intent匹配才
恢复DELIVERED；否则UNKNOWN。Runtime签发、版本撤销、report/reissue、REST路由和自动部署接线仍待BOOT后续，
本切片没有修改PG或普通REST明文剔除保护，也没有翻转字段/整族target状态。

主代理实测：

- `cmake --build --preset full-locked --target kiarabot_model_bridge_test` exit0；
  `ctest --preset full-locked -R '^p11_model_bridge$' --no-tests=error --output-on-failure`
  1/1、3.94s。日志`build/logs/capability-expansion/build-herr-model-bridge.log`、`ctest-herr-model-bridge.log`。
  HERR三文件无新增warning；构建中旧Runtime action/RPC诊断和重复库链接警告不冒充本切片修复。
- `cmake --build --preset full-locked --target kiarabot_secret_delivery_test` exit0，新增C++无warning；
  `ctest --preset full-locked -R '^p12_secret_delivery$' --no-tests=error --output-on-failure`
  1/1、1.00s。日志`configure-secret-delivery.log`、`build-secret-delivery.log`、`ctest-secret-delivery.log`。
  实测四个持久边界fork子进程退出/重启、真实rename失败后的UNKNOWN、丢ACK查询、CAS迟到旧worker拒绝、
  symlink/硬链接/路径/owner/mode边界、busy锁及现有`target_host_credential.py verify`兼容。
- 锁定`build/format-venv/bin/clang-format`16.0.6对8个改动C++文件格式化/检查exit0；
  `python3 scripts/quality/check-boundaries.py`、`check-architecture.py --inner`、任务范围`git diff --check`均exit0。
  锁定clang-tidy22对Bridge、Bridge测试、交付纯应用、文件sink、交付测试五个改动TU最终均exit0，
  结果见`herr-boot-tidy-final-results.json`。首次sink检查发现dup失败可能传负fd及fdopendir失败泄漏，
  已用明确RAII转移修复，保留原失败日志；新增fork子进程内EMFILE实际负例验证稳定失败且不泄漏，
  此负例等待主代理有限重建。未重复全量产品测试或全仓工具链，避免共享构建争用。

文档影响：已同步`doc/03-plugin-sdk.md`错误语义、`doc/06-control-api.md`模块边界与交付错误，
`doc/08-operations.md`固定文件运行和恢复说明。HTTP/WS/proto未新增本切片接口，冻结契约未变，无需再改
公共schema；`doc/18-architecture-baseline.md`只读。HOST-02后续由独立章节记录，不用本切片证据替代维护
operation/Runtime gate/生产管理RPC退出。

## HOST-02（源码已齐，构建待验收）

模块为纯`HostMaintenanceApplication`及typed授权/RuntimeAdmissionGate/执行/存储端口，外层为Host私有
SQLite实现。允许文件为新`host_maintenance.hpp/.cpp`、`host_maintenance_sqlite.hpp/.cpp`、专项测试与
`cmake/host-maintenance.cmake`，以及直接受影响的HostService/HostJournal实现与测试；不触碰PG共享UoW。
输入为精确Host资源、expected version、幂等键和短期已认证上下文；输出为Host状态/安全Operation、
同一OperationObservationPort的有界cursor页。无秘密、PID或marker通过公共变化日志输出。

SQLite将命令身份、状态、审计和commit-ordered变化同事务保存；支持CAS、持久epoch、600秒与容量
low-water、提交后丢ACK恢复。Begin先持久LocalClosed，再确认RuntimeClosed，冻结有限执行集合，逐项
停止并记录known事实；Resume在不存在未知/冲突后确认RuntimeOpen，再打开本地门。Cancel仅限Accepted。
依赖或进度写失败保持Unknown/Blocked并沿原operation恢复，不把命令receipt当终态。HostService已传播
逐stop失败，journal活动集合保留Exited+Unknown，保留既有marker/白名单及无admission依赖的默认行为。

真实SQLite、触发器回滚、fork提交后丢响应/重启和固定测试子进程fixture已写入专项测试；其Gate与授权
是明确局部替身，不算真实Identity/RuntimeGate/RPC或完整HOST证据。实际接线尚未开始。当前11个改动
C++文件锁定format16与`git diff --check`通过；构建目标`kiarabot_host_maintenance_test`、CTest
`p08_host_maintenance`已交主代理串行验收，尚无运行通过声明。

文档影响：同步`doc/02-core.md`、`doc/08-operations.md`，新增
`doc/modules/host-maintenance-storage.md`。doc08当前源schema已同步I15/R19/S11/M10，保留历史记录。
公共协议/API未新增本切片接口，冻结定义不变；doc05由WS owner后续统一链接模块子文档，doc18只读。
后续身份链核定发现Host enrollment/config缺口，以及后台Host精确grant在线验证缺少专用语义，已将
最小受限Identity端口提案交主代理，不以本地替身或普通mTLS连接掩盖。

身份链源码核定：`identity_bootstrap_main.cpp:303`的platform service解析与
`identity_admin_application.cpp:19`的enrollment范围均只含Runtime/Scheduler/Model；
`service_config.cpp:664`同样拒绝Host的identity_endpoint，`host_composition.cpp`尚无Identity消费者。
现有`Identity.CheckAuthorization`在RPC及应用同时拒绝空subject_proof，之后校验audience绑定的
delegation；`IssueDelegation`源为当前access session。因此不能让Host后台恢复持久保存Control bearer，
也不能通过CA/SAN/本地白名单宣布host.execute已获在线核验。

后续提案（尚非现行契约）：已有enrollment/config显式追加Host，HostManagement人类请求复用现有
audience=Host的delegation消费者；后台gate单独增加受限Identity检查，固定Runtime调用者、
RuntimeService.SetHostAdmissionGate方法、单Host scope及从真实mTLS得到的Host叶指纹，在线核定
current registered Host与精确host.execute，再与Runtime fence/intent逐项比对。检查不接受任意permission/
方法或客户端主体，不改变Scheduler受限A的两方法范围。具体wire由主代理协调冻结后实施。


v1.1契约已按主代理授权落盘：Identity.CheckHostGateAuthorization固定Runtime调用者与单Host用途，
request为context/host/actual TLS certificate SHA256，response为decision及Host credential/principal版本。
新增Host enrollment显式HostId绑定的运行规范已写入doc01/02/08与proto README；生产代码仍待生成窗口后
接线，新增字段/RPC/semantic状态均保持not_implemented。原CheckAuthorization非空proof语义保持。
独立Python/descriptor生成、对上一冻结descriptor真实兼容、enum/status/semantic/evidence与新专项oracle
均exit0；完整`proto/tests/check_contracts.sh`（legacy reader=1、baseline=freeze-descriptor.pb）exit0，
日志`identity-v11-contracts.log`及`identity-v11-contracts-result.json`。两条新wire golden已登记，
Python-C++往返等待主代理重链serializer，不用旧binary伪称新消息通过。


HERR-03按真实2026-11-04 TLS raw-wire门禁（p11_model_bridge 1/1、3.94s，原日志与5TU tidy见前）仅登记
两个target semantic override：request_operation_classification与unknown_operation_error。旧unknown证据
路径修正为真实core/reference路径，reference状态/blob均未改；不宣称typed gRPC能识别已知oneof被覆盖
历史，不翻streaming/HostService族或任意其他字段。最终整体验证由主代理重跑。
