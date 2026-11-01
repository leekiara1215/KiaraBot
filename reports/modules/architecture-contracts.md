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

## P01a 公共类型与流

新增 `platform.proto`；Adapter/Plugin 原有流增加强制目标 fence、安装和执行绑定、配置版本及投递 attempt/续租/回执。未提供第二套兼容桥；现有消费者尚未迁移。新增字段/枚举已登记 not_implemented 并同步状态表。公共消息/代次为规范约束，proto3 本身不执行必填校验，生产边界必须拒绝缺失/0；后续独立目标 fixture 验证边界映射前提。

本步实际检查：protoc 全量 descriptor 生成、实现状态登记检查、枚举一致性检查、git diff --check。文档更新为 proto/README、doc/01-contracts、doc/01-runtime-semantics、doc/06-control-api；其余模块文档无生产行为改变，留给实现所有者。未运行全仓构建：P01 不是旧消费者兼容门禁；C++/Python 生成与往返将在独立入口补齐并实际运行后记录。

## P01a 五服务类型化方法

新增 identity/runtime/host/scheduler/model/policy.proto，独立 request/response 与封闭权限 descriptor options。修正 P00 的调用方宽泛默认：人类会话入口仅 Control；CheckAuthorization 500ms/1s；Chat 30s/30s，由 logical_call_id 防重，无普通正文重放；ACK/Renew 当前认证 fence 控制消息不再要求在线查历史用户。新增已批准白名单策略 Get/UpdateActionPolicy。Claim/Deliver 不公开给插件。全部方法状态为未实现。

Python 本机默认3.9（与仓库目标3.11.18不同），临时环境 protobuf6.33.5、grpcio-tools1.76.0、grpcio1.80.0；Python生成器自带 libprotoc31.1，C++本机 protoc/runtime36.2、gRPC pkg-config 提供。不存在 Python7.35 下载版本；不绕过版本检查，改用已取得匹配生成器。根工具链任务须锁定最终统一环境，本机线格式验证不冒充目标锁验收。

五服务提交实际验证：独立 `KIARABOT_TARGET_PYTHON=/tmp/kiarabot-contracts-venv/bin/python proto/tests/check_target.sh` 通过77个RPC的 request/response/权限/幂等/deadline/在线分类匹配及269种消息 Python→C++→Python 往返；C++/Python/gRPC均实际生成，C++和gRPC生成源实际编译链接。clang-format 检查 target_roundtrip.cc；全量枚举、实现状态、语义门禁均通过。生成源及本机 Abseil 宏产生 nullability extension 警告、链接重复库提示，不能据此声称无警告全构建；手写测试独立格式校验通过。新proto接线清单：platform/policy/identity/runtime/host/scheduler/model.proto，根 CMake 尚未包括，由工具链所有者接入。未运行全仓、PG、生产认证/并发测试，P01仅规格/线格式验收。
