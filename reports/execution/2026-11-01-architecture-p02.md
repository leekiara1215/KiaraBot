# P00/P01 收尾与 P02 并行实施

用户要求按计划继续开发，沿用既有 worktree、独立对话、小步提交与主对话逐提交集成。
上一轮最后的 schema 判别字段提交 `0b2aebc` 已独立集成为 `ab4aebd`。
本记录属于实施协调，不修改已确认方向或受保护 `doc/18`。

## 范围与退出条件

输入是已集成 P01 契约/内部模型；输出是六个独立进程、服务专属纯 C++ application
库、边界传输与阶段测试。P00 固定环境和 P01 完整门禁继续收尾，P02 代码在同一集成
分支准备；前置与 V0/V1/V2 未通过前，不宣布 P02 完成，不推进 P03 数据库业务。
所有 target 业务仍未实现；禁止把旧 Core 改名当六服务、创建业务数据或伪造 ready。

## 并行文件归属

| 任务 | 工作区 | 允许范围 | 禁止范围 |
| --- | --- | --- | --- |
| 工具链/构建 | kiarabot-toolchain | 根/模块 CMake、依赖锁与质量脚本、隔离旧构建、doc/11、本任务模块报告 | 新服务业务源、API、保护基线 |
| 五服务与公共进程支撑 | kiarabot-core-runtime | 新 domain/ports/application target、runtime/platform、五个 core/app/*_main.cpp、新内层测试、doc/02、08 | Control 内部实现、根/模块 CMake、旧源码移动 |
| 独立验收 | kiarabot-contracts | tests/architecture/、阶段行为清单、自测及本任务报告 | 生产源码、CMake、API/proto、其他线程文档 |
| Control 与统一集成 | 主工作区 | control/api 新 application/target、app/main.cpp、相关测试、doc/00、06、12-ci-cd、16、23、协调记录 | 他人未提交内容、保护基线 |

各开发从本记录提交开始建立新分支，旧分支保留。每完成独立能力立即精确暂存并提交，
不 squash、不 push；主对话审查后按依赖 cherry-pick。每个实现提交附实际检查及文档影响。

## 首个共享接口交付约定

Core 任务先独立提交服务就绪端口与公共 runner 声明，主对话审查后交给其他任务：

- `core/include/kiarabot/core/ports/target/service_readiness.hpp`：纯标准库端口，使用既有
  `kiarabot::core::target::Service`；描述服务类型、依赖状态和就绪结果，不含框架类型。
- `core/include/kiarabot/core/runtime/platform/service_process.hpp`：外层入口，提供
  `int run_service(const target::ServiceReadinessPort&, int argc, char* argv[])`。
- 五个 Core application 各自提供工厂并独立编译；Control 的 application 由主对话提供，
  只依赖纯就绪端口。公共 runner 不链接旧 RuntimeSnapshot/Control 对象或中央 SQLite。
- P02 application 的就绪结果必须包含“业务未实现”门禁，任何配置不得启用假 ready；
  后续阶段才实现依赖验证。接口最终细节以该首个独立提交为准，不能各自复制类型。

## 配置与外部行为约定

六入口均支持 `--help`（0、不监听）和 `--config PATH`；缺参数、未知参数或无效配置
退出 2，运行/绑定/证书故障退出 1，正常 SIGTERM/SIGINT 关闭退出 0。
配置为 JSON，`schema_version=1`、`service`（control/identity/runtime/plugin-host/scheduler/model）、
`probe_listen`、`tls.{certificate_file,private_key_file,ca_file}` 必填；非 Control 另有
`grpc_listen`，Control 不提供入站业务 gRPC。配置与 CLI 的诊断不包含秘密/PEM/DSN。
拒绝未知字段和服务类型不匹配，路径只引用文件，不把私钥放配置正文。具体权限检查
与 TLS 验证细节由公共 runner 在实现说明中列明，不能关闭证书校验。

探针只接受 `127.0.0.1:PORT` 的独立 HTTP 监听，不能承担远程业务流量；
`GET /health/live` 返回 200/HEALTHY，`GET /health/ready` 在本阶段返回 503/UNHEALTHY，
沿用 API 的 ProcessProbe 结构。未知路径 404；Control 的 `/api/v1/`、`/ws/` 业务入口
在本阶段明确拒绝，不挂接旧同进程业务。业务 gRPC 必须配置 mTLS，缺/错/过期证书
不得成功启动；注册正确 owner 服务及 Runtime 桥接服务，未实现 RPC 返回 UNIMPLEMENTED，
不接受请求后写入任何状态。信号处理、线程、socket 和 gRPC server 有明确 RAII/有界退出。

## 构建与验收约定

`full` 建立六目标与全部目标契约；固定环境入口为 `full-locked`。内层库不得 PUBLIC/
INTERFACE 链接 protobuf/gRPC、JSON、数据库或 TLS；通过实际 compile DB、CMake target
依赖图和头编译验证。新测试标记 `p02` 与 unit/integration/fault 分类及超时；缺测试失败。

旧实现不再链接六个生产入口。需要保留的算法/行为测试以明确 reference 构建隔离，
编制待重建行为清单；必要的旧文件机械迁移仅由工具链任务执行，禁止顺便格式化或
删除断言。全仓手写翻译单元须有编译命令，reference 的通过不代表目标业务实现。

验收任务覆盖六服务帮助/配置拒绝/证书拒绝、live/不就绪、未实现 RPC、端口冲突、
SIGTERM/SIGINT、并行运行和单服务故障隔离。证书使用测试临时 CA，无生产秘密；
网络权限按环境审批，不绕过。完整工具链/格式/tidy、固定依赖安装与实际验证分别记录。

文档影响：各实现同步其模块和运维行为；主对话同步边界、API 运行说明、CI 与进度。
本记录仅协调，无新增协议字段；proto/API 语义若需要变更必须先单独提交契约再实现。
构建/生成/venv/证书产物不提交。
