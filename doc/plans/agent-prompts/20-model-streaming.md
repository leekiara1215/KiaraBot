# 提示词：打通模型Streaming完整链路

你接手KiaraBot STR工作流，目录`/Users/betta/Workspace/KiaraBot`。
先读`doc/plans/2026-11-04-next-agent-prompts.md`及Model源码交接，按AGENTS读规范、
doc/01/02/03/05-infrastructure-model-streaming.md和冻结记录。你不是唯一编辑者，不能覆盖其他工作。

Provider/parser、Model旧Chat/新stream纯应用和真实PG恢复测试已局部通过。
当前STR06新codec/RPC/fixture已有未接CMake、未构建的半成品。先核对交接清单，完成它，避免重新实现。

按顺序执行：
1. 接Model ChatStream与ListOperationChanges生产RPC/组合根/CMake。
   stream sink非阻塞，有界队列、唯一网络writer、真正1秒写取消、join和异常收口。
   Runtime方法白名单显式限制，保留旧合法调用；观察查询只接实际Control及resource.read，不扩Runtime权限。
   proof expiry裁剪执行期限，不丢弃当前委派期限；共享decoder的Model audience已有专项测试。
2. 完成Runtime Model客户端、纯应用准入及异步PluginBridge。
   start worker不得堵塞reader heartbeat/refresh/cancel；唯一writer、每连接4流/进程64流、队列限额，
   连接结束先取消/唤醒再join，不引用已销毁handler栈。保留Task/Storage/ACK与HERR分类。
3. 完成Python SDK独立async stream及cancel/异常/退出清理；受理前HostResponse拒绝也必须结束pending流，
   不能一直等待ModelStreamEvent。旧非流式SDK不变，QQ仍聚合完整结果后一次Action。
4. 用本地受管HTTP Provider+真实PG/TLS/Runtime/受管插件与SDK做整链验收，不调用付费模型或真实QQ。

冻结语义必须实现：started0、delta1..N、terminalN+1；prestart仅terminal0；同logical ID
跨Chat/Stream共用受理防重，不重新调用Provider、不重放正文；sequence/request/logical/trace逐帧校验。
完整成功需合法STOP+[DONE]+已确认txn2；partial/UNKNOWN/usage/may_have_billed独立表达，
取消收据不是未执行/免费证明。每秒在线授权、输入/输出/内存/排队/执行和收尾限额按冻结契约。
保留35秒STARTED恢复/CAS和30天UNKNOWN真实SQL边界，迟到worker不覆盖恢复事实。

你拥有Model应用/RPC/Identity客户端/组合根、新codec/测试、Runtime Model相关端口/客户端、
Bridge流式切片与SDK；RuntimeRPC公共文件、core/CMake、platform.cmake、PG通用schema交统筹分配锁。
ModelCallRepository/model_store的原有切片可以接续，不能顺改WS CommonWork或其他服务数据库。
registry和公共proto当前WS编辑，不并发翻状态。host.model.stream.v1只在端到端可用后协商。

首先要求统筹有限构建交接文档所列真实目标；新增target/test接线完后报告精确名称。
基础回归：p11_model_gateway_application、p11_streaming_model_application、p03_postgres_database，
以及Provider、ModelRPC、p11_model_bridge、原任务/存储桥与PythonSDK相关测试。
完成格式/专项tidy、契约/文档影响同步，再报告实际结果和整链证据。
