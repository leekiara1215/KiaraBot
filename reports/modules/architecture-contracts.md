# Architecture contracts — P00 / P01a

工作区：`kiarabot-contracts`；分支：`codex/architecture-contracts`；共同基点 `6d64b2a`。

## P00 规格

目标是唯一跨进程协议及浏览器 API，输入为 doc/19–23 已确认设计，输出为类型化消息、服务、状态和独立验证入口。允许修改 proto/、api/、doc/01-contracts.md、doc/01-runtime-semantics.md、doc/06-control-api.md 和本报告；不修改业务实现、根构建、doc/18–23 或其他工作区。

`proto/tests/target_spec.json` 将 doc/21 的每个逻辑方法固定为具体 service、request/response、文件、调用方、封闭权限、在线授权分类、幂等和 deadline。Host 的逻辑命令可在主动流上传递，不能转成任意 shell。Claim/Deliver 是 Runtime 内部投递边界，仅允许可信 Runtime 调度上下文；对外插件流只消费已领取投递。登录限频及 Control 服务认证替代已有用户会话前置。Refresh 验证当前凭据轮次。完成消息 ACK/Renew 使用当前认证流/fence，不在线重新验证已撤销人类会话。其余未分类方法在线核验。

公共消息固定为 RequestContext（尝试/trace/deadline/幂等键）、ResourceRef（类型和稳定 ID）、Fence（稳定实例/执行代次/连接代次/Runtime epoch）、Operation（所有者/资源/版本/状态/执行结果/阶段进度）、FailureContext（是否已受理/恢复动作/操作引用/retry_after）、StreamCursor（所有者/epoch/scope/序号）。资源/代次为非零，修改 expected_version 显式 presence；缺省不允许绕过条件写。上下文不接受自报角色或 owner，身份来自 mTLS/受限证明。幂等 fingerprint 排除 request/trace/新证明，包含业务期限/版本/业务参数，成功内部收据至少七天，Control 收据24小时。

错误映射逐码见 JSON；断线、CANCELLED/超时不创建业务 FAILED。UNKNOWN 单独记录，不自动重发平台/模型。固定角色 admin/operator/viewer 与显式资源范围取交集，无 message.read 默认授予。业务 payload 不扩大；ScheduleTask Action 继续拒绝。

验收：目标 descriptor 与方法表一一匹配、C++/Python 实际生成和跨语言往返、字段/枚举/方法状态登记、API 解析/引用/lint。生成文件只在临时目录。生产者均未实现，协议通过不代表 P02/P03 或全仓通过。

文档影响：P00 只新增规格和本报告，无已生效接口变化；P01 同步 proto/README、doc/01-contracts、运行语义、doc/06。doc/02–05、07–08 由实现所有者后续同步，本任务不修改它们且不宣称新生产行为；doc/18 只读，无基线编辑。

P00 检查：`python3 -m json.tool proto/tests/target_spec.json`、`git diff --check`。本机 protoc 与 grpc_cpp_plugin 可取得；系统 Python 缺 protobuf，将建立临时独立验证环境。根 CMake 接线由工具链任务负责。
