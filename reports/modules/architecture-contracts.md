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

## P01a REST/WS 与收尾

OpenAPI 新增33个管理操作，引用类型化目标消息 schema，并增加 Error.failure_context 与规范 gRPC/HTTP错误映射；AsyncAPI 新增带 scope/owner cursor 的 operations/operation.updated。全部显式not_implemented，不扩大消息正文、模型历史、角色定义或插件市场。已有开发版本端点保留的实现状态仅指原行为，不代表已迁为六服务；不提供混合版本生产桥接。

按主对话复核，补五个owner service方法集合双向相等检查，mutating和requires_idempotency_key分离。Chat/ACK/Renew/注册/心跳/交接仍是状态变更，以业务稳定身份防重，无普通响应收据开关；普通写才要求请求幂等键。

文档影响：更新api/openapi.yaml、api/asyncapi.yaml、doc/01-contracts.md、doc/01-runtime-semantics.md、doc/06-control-api.md、proto/tests/README.md与本报告。proto/README已在前一提交描述全部目标文件，本步无新增文件职责无需重复更新。doc/02–05/07–08无生产实现变化，留给各实现所有者；doc/18–23未修改。

实际静态检查：ruby proto/tests/target_api_test.rb（33操作/错误映射/全部跨文件引用）、实现状态检查、枚举检查、语义门禁、Ruby YAML解析、git diff --check。正式npm lint首次DNS受限，升级权限后证书链错误，系统CA重试中；未关闭TLS验证。最终结果另行追加。

最终复验：`check_target.sh`（修正后的完整方法集合、独立幂等分类、77 RPC/269消息）通过；C++ protobuf36.2.0/gRPC1.84.1（Python grpcio1.80.0）、本机Python generator31.1/runtime6.33.5。下载固定lint工具时加系统CA信任后成功，Redocly2.72.1正式lint通过（2条unused组件警告：TargetRpcFailure、仅WS跨文件引用的TargetStreamCursor），AsyncAPI parser3.6.6通过（原未用组件information，无error），未禁用TLS。Ajv schema正反例通过：成功主结果/资源/操作/fence必填，缺失/空ID/零代次/uint64溢出拒绝，最大uint64与未应用配置0接受，registered和Blocked条件校验通过。

全仓 `./scripts/quality/check-format.sh` 实际失败：基点现有 control/api/src/configuration.cpp、infrastructure/src/file_storage.cpp及多份core/infrastructure测试的格式与本机clang-format不一致；均不在本任务范围，不进行无关格式化。新增target_roundtrip.cc独立 `clang-format --dry-run --Werror`通过。全仓full构建/tidy/统一toolchain未运行：根CMake尚未接入新proto及target_roundtrip.cc、旧消费者尚未迁移，当前不是全仓或生产门禁。真实PG、认证拒绝/撤销SLO、租约并发、跨主机、平台及计费模型均未运行，本任务无生产实现。

首轮停止P01a。后续依赖：工具链接入7个新proto、目标独立测试及API/Ajv门禁并锁定统一生成器/runtime；Core映射请求/资源/操作/Fence并实现非零/缺失校验，五服务生产者实现与文档归各模块；Control按所有者路由并实现schema/当前身份与范围授权。所有target方法/字段/频道均未实现，contract_status和target_spec明确登记。无P02/P03业务、无旧数据导入/清理、无push/merge、doc/18–23未修改。

## 可审查提交索引

- `112ec27`：P00 方法/错误/租约/权限规格，target_spec.json 与本报告。
- `e4983db`：platform公共类型、Adapter/Plugin fence/attempt/续租字段、登记和四份契约说明；descriptor/枚举/登记通过。
- `51be69b`：五服务类型化RPC及封闭权限options、独立生成/往返入口、枚举/状态/文档；77RPC/269消息、状态/语义/格式独立检查通过。
- `baa48bb`：mutating与普通幂等收据分离、RPC集合双向核对；host/runtime.proto、target_spec、target_contract_test、运行语义说明；目标独立入口复验通过。
- `402e42e`：33 REST操作、operations WS、严格required/uint64边界、Ajv/引用检查、登记和契约文档；正式lint/parser与全部静态检查通过。

提交后对整个基点差异运行 `git diff --check 6d64b2a HEAD` 发现platform.proto新增EOF空行（此前工作区检查未覆盖已提交差异），以纯空白收尾提交修正；不改变输入输出，无新增文档行为影响，无需重新生成。最终工作树应干净。所有构建/生成产物均未提交。

## P01a 外部自管实例 schema 修正

主审发现创建实例无条件required host_id会拒绝已批准的外部自管实例。仅修改api/openapi.yaml的TargetRuntimeCreateInstanceRequest/TargetInstanceRecord，增加相同的模式条件：managed必须非空host，external可省略host；显式空host仍拒绝。创建请求省略模式按managed，投影必须明确模式。输入输出/错误仍属Runtime创建及查询契约，形状错误INVALID_ARGUMENT，Host存在性/授权留生产实现。

同步doc/06-control-api.md与本报告，补target_schema_test.cjs两种模式正反例。未修改proto（原host_id/externally_managed字段已能表达此语义）、AsyncAPI（引用同一目标schema）、doc/01（状态/权限/方法无变化）、运行语义或doc/18–23；无需重新生成C++/Python或运行业务构建。本次检查为Ajv完整回归、正式OpenAPI lint、AsyncAPI parser、引用/实现状态/枚举/语义及git diff --check，结果以实际输出为准。

本修正实际结果：Ajv managed/external 12项组合及既有schema回归通过；33操作引用检查、实现状态、枚举、语义、git diff --check均通过；Redocly2.72.1通过（4条现有schema组合/unused组件warning，无error，新增实例条件无warning）；AsyncAPI parser3.6.6通过。无C++/proto变化，不重复生成或运行full/tidy。

## P01a 判别分支显式required

外部实例修正已提交`3243aff`。主集成复核确认此前最后schema的Redocly实际有4条warning，而非早期记录的2条；上节已记录4条。本步在TargetVersionedResourceState两个分支显式required registered，在TargetOperation两个分支显式required state；它们本来已是外层required，因此不改变合法输入输出，不放宽条件，无需额外业务文档同步或proto生成。

实际复验：Ajv全部模式/必填/边界回归、33操作引用、状态/枚举/语义检查、git diff --check通过；Redocly2.72.1通过且现在确为2条unused warning（TargetRpcFailure、WS跨文件使用的TargetStreamCursor），AsyncAPI parser3.6.6通过。未重复全仓构建/格式/tidy：仅schema等价约束与报告，无手写C++或实现变更。
