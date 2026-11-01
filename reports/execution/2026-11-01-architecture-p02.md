# P00/P01 收尾与 P02 并行实施

用户要求按计划继续开发，沿用既有 worktree、独立对话、小步提交与主对话逐提交集成。
随后明确要求持续完成主线至程序可用，只有重大变故再打断；阶段检查和报告不表示停止。
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

## 本轮已集成的能力与实测

生产入口为 build/<preset>/bin 下六个 kiarabot-*，各有纯应用库，Core 内层与 Control
内层独立头编译。非 Control 为 mTLS owner RPC；所有业务尚未实现，明确 UNIMPLEMENTED，
live=200、ready=503。42 个历史 Core 文件字节不变隔离，原行为断言保留为 reference。

所有开发能力均独立提交后审查 cherry-pick，无 squash：

| 能力 | 原提交 | 集成提交 |
| --- | --- | --- |
| 就绪端口 | 18c73bc | f8b7290 |
| 五应用 | 8337736 | 1d37086 |
| 严格配置/TLS | ca1cbea | 1bc72c0 |
| FIFO 与入口异常 | 7230f30 | e6db90f |
| 证书起始边界 | 60bd92d | 5b71fb8 |
| 实际进程/owner RPC | edd9112 | 8c4cec1 |
| 非GET探针错误映射 | cf2a392 | 491b84a |
| 固定格式 | 4205e92 | 06fa2a4 |
| Control 应用/入口 | 主工作区 | ebd05c7 |
| PKI 与生命周期 | 71c6982、2be1d77 | c64a4fe、fa1b6d1 |
| TLS、隔离与RPC preflight | 799d03e、54cd092、f88f09e | c8411b3、456c14b、fbeb77f |
| reference 映射/wrapper | 97a2c46、29f717c | 9bcd68a、8d992bb |
| 隔离规格/质量锁 | c0dbcdf、00bd566 | 10c895f、6551b72 |
| 原样隔离42文件 | 59e1afd | 0e44f1e |
| 内层/13proto/往返 | 3956350 | 85545ae |
| 六入口与黑盒接线 | 3e28f1f | b4d377e |
| 保留Control reference与映射测试 | 4d96f1b | 7a8c7b1 |
| 固定依赖/真实PG与Ed25519 smoke | 011ea40 | 403a543 |
| 完整静态分析及固定检查入口 | 505fa3b | 15a6a83 |

主工作区实际运行：full 配置、构建，完整 CTest 最终 50/50（包含真实六进程/77 owner RPC
与三个桥接流、mTLS正反例、生命周期和故障隔离、269个消息往返、reference全部套件）。
第一次48/49暴露Control reference测试目录错误，修复路径后重跑通过，未跳过测试。
实际 CMake 图/内层检查、reference manifest与42文件原始git blob逐一对比、format16、
PKI/support/TLS/reference/wrapper自测、API lint/Ajv通过。系统依赖 full 不是固定环境。

工具链工作区固定环境实际 51/51 CTest、standalone infrastructure 10/10、80手写TU的
tidy22及format16通过；PG16.4 verify-full/SCRAM channel binding、libpqxx7.10和固定
Ed25519 sign/verify/错误audience拒绝均真实运行。主工作区正在独立复验该固定环境。
主工作区早先tidy出现Homebrew Boost调用链泄漏诊断，工具链对同TU用tidy22重验通过，
没有禁用checker或删除reference；不以该单TU通过代替主工作区全仓复验。

统一入口尚非全绿：历史contract evidence仍引用迁移前路径，且 implemented 的reference/
target语义须明确，已交契约线程修复。Python发行归档独立hash获取403另交工具链继续；
未验收远端Linux CI、真实QQ、付费模型、跨主机部署与恢复。不得将P02当可用机器人。

## 文档影响与检查

本次集成说明更新 README、doc/00、12-ci-cd、16、23 和本报告，纠正入口、构建、隔离及
阶段状态。doc/02、06、08、11已在各实现提交同步行为；本汇总不再重复改变这些行为。
proto/API无新增字段，不需以汇总为由重写；doc/18保护基线保持只读且无diff。
V0检查为 git diff --check 与本次文档相对链接校验；后续复验结果按实际追加。

## P00–P02 本机门禁关闭（固定快照 c43b51e）

主工作区实际执行固定环境统一入口，日志 /tmp/kiarabot-root-locked-unified-p03.log，
退出0且最终All toolchain and quality checks passed：dev12/12，full-locked53/53
（139.91秒，已含P03两基础测试），独立infrastructure10/10（12.36秒），真实PG/JWT、
registry/installed来源、实际链接图/内层/全部84手写TU覆盖、p02/p03标签、全84TU tidy22、
format16、完整契约/9 scope回归/descriptor与legacy reader、API/Ajv及质量自测全通过。
工具PATH显式使用锁定Python3.11.18/Node22.14/protoc29.3/format16/tidy22；vcpkg prefix
复用已核验工具链工作区，PG16.4和OpenSSL3.5工具由CMake实际配置取得。

此前统一入口的两个间歇失败保留在 /tmp/kiarabot-root-locked-unified.log：RPC helper
退出TSD清理SIGSEGV及reference SDK明确disable后非零退出竞态。分别独立修复26defb1/
ebdab21后，本完整入口全部通过；无删除测试/旧断言、禁用分析或改保护基线。细节与
压力复现限制见两个architecture-p02-*-stability模块报告，不将压力无复现当穷尽证明。

工具链旧聊天持续等待其历史权限审批，父已代跑其失败的HTTP fixture1/1通过，审查
上下文补丁并独立提交0d5fd94；P03构建改派新聊天/ci-cd worktree，不让审批阻塞开发。
Python预期归档hash已由固定uv内嵌清单验证并提交761e1e2；归档体未独立取得的HTTP403
仍保留，不声称Linux/远端CI/跨主机/真实QQ或付费模型验收。它们不由本机门禁代替。

本节只关闭上述固定快照P00–P02；其后新契约/配置/仓储增量需再验收。程序仍没有目标
业务闭环，继续P03及后续主线，无需用户另确认。

reference链接补记：机械隔离后，doc/14、15、17、20及reports/pending-confirmation中的
14个代码链接仍指向旧位置。按已核验manifest精确改为reference目标，保留锚点与历史
结论；所有目标存在、check-reference.py与diff检查通过。仅链接变化，无接口/运行语义
影响，其余契约/模块文档无需同步，doc18保护内容及保护清单未改。
