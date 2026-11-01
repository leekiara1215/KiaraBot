# P00 环境收尾与 P02 构建执行

起点 `d20b54bcc4ea01df023acb54cd528b4a0764b3db`，分支 `codex/p02-toolchain`，
指定 worktree 初始干净。范围仅构建/依赖/质量检查、旧 reference 机械隔离及 doc/11。
新六入口及 application 由其他任务所有；不复制未提交文件、不修改 proto/API 语义。

## 第一能力：reference 隔离规格（先于机械实施）

目标：让当前断言继续编译/执行，旧 Protobuf/JSON/快照依赖退出新内层扫描范围；
保留的 reference 结果只证明旧行为不变量，不证明新平台实现完成。

机械迁移清单与规则：

- 将 `core/include/kiarabot/core/{domain,application,ports,runtime}` 中的既有文件逐个迁入
  `core/reference/include/kiarabot/core/` 的同名层。所有 `target/` 及新
  `runtime/platform/` 文件明确保留原位，新增源码不属于迁移清单。
- 将既有 `core/src/application/*.cpp`、`core/src/runtime/*.cpp` 迁入
  `core/reference/src/` 同名层；新 target/platform 子目录不动。
- 仅迁移旧 `core/app/main.cpp` 为 `core/reference/app/main.cpp`，不动其他入口。
- 文件内容逐字节不变（以 git blob 对照）；保留原 include spelling/namespace，reference
  目标显式提供 `core/reference/include`。新内层目标只获得 `core/include`，因而不能通过
  默认 include path 偷读 reference 头。不格式化、不移除任何旧断言。
- `core/tests/`、旧 Control/infrastructure 源和测试保留原路径，构建为明确 reference 图；
  它们不属于新服务业务内层。旧模块测试保留数量、名称/断言；阶段只标 reference，
  不能贴 p02 冒充新服务验收。

构建接口：

- 根 `full` 构建六个新 executable、13 个 proto、独立 application 库、全仓手写 TU，
  同时构建 reference。`full-locked` 采用精确 manifest，缺环境即失败。
- 新六目标不链接旧 `kiarabot_bridge`、旧 Control、中央 SQLite、快照/旧迁移/旧组合根。
  每个旧目标记录 reference 属性；基于实际 CMake file API 图追踪到 reference/外层依赖
  的新 application 链路必须失败，不能只比目标名或 PUBLIC 链接字符串。
- 旧一体 executable 的 CMake target 改为 `kiarabot-reference-core`，旧输出 basename 可
  为兼容既有测试脚本保留 `kiarabot-core`，但只输出在 `build/<preset>/reference/core/`。
  同理 Control fixture/credentials 输出在 `reference/control/api/`，由 CTest 设置
  `KIARABOT_ACCEPTANCE_BUILD=<binary>/reference`。它们不是生产发布入口，不安装。
- reference infrastructure 仍可独立配置测试；显式传入原 proto 根和 reference Core
  include 根。所有 reference TU 纳入 full compile DB 和 tidy，不能靠排除源码变绿。
- 新测试在对应模块 CMake 中注册 p02 + unit/integration/fault、TIMEOUT；独立黑盒验收
  从已提交 tests/architecture 接入。新源未合入前配置明确失败或只验 reference，不能
  创建空库/替代进程或静默跳过缺入口。

行为 reference 清单（保留断言，后续按阶段重建）：

| 既有用例 | 后续目标阶段 |
| --- | --- |
| runtime/core smoke、状态与存储提交、日志/投影 | P02/P03/P13 |
| adapter ingress、插件 hub/投递/配置 | P05/P06/P08 |
| Action router/配额/授权命令 | P05/P07 |
| Host dispatcher/namespace | P06/P09 |
| scheduler/time zone | P10 |
| HTTP model provider/model host | P11 |
| Control application/service/HTTP/WS/event stream | P04/P12 |
| SQLite snapshot/limits/crash/migrate/file storage | 仅旧实现 reference，不作 PG/文件服务验收 |
| 旧进程/grpc lifecycle/deploy fixtures | P02 新独立黑盒另验，旧环境脚本保留 |

文档影响：doc/11 必须同步构建和 reference 路径；本报告登记检查。doc/00/02/06/08/
12-ci-cd 由对应所有者更新，本任务不改；没有 proto/API 语义变化，无需改唯一契约。
doc/18 只读。本规格未实施迁移，因此尚未运行构建/格式/tidy；先运行文档 diff 检查。

验收：机械迁移 blob 相同；13 契约生成与往返；full 全 TU 覆盖；实际链接隔离；新内层
头编译；reference 原断言；独立 p02 生命周期验收；固定环境与 PG/JWT 另有真实证据。

## 第二能力：固定质量工具与 registry 来源实证

新增 `dependencies/quality/requirements.in` / `requirements.txt`，精确格式/静态分析工具
及所有匹配 PyPI 归档 hashes。实际安装在本 worktree `.tools/quality`；未提交二进制。
PyPI tidy 16.0.6 不存在（实际解析失败），改锁可取得的 19.1.0.1，二进制 LLVM 19.1.0；
format 固定 16.0.6。下载先遭沙箱 DNS 拒绝，经规则请求环境授权后成功。

实际检查：两工具 --version 通过；固定 format 全仓检查通过。官方 vcpkg checkout
`ce613c41372b23b1f51333815feb3edd87ef8a8b` 的 --registry 校验通过；bootstrap/install
正在单独执行，尚不能宣布 PG/JWT/TLS 安装成功。质量工具 venv 使用已存在的 Python
3.11.17，不声称已通过目标 Python 3.11.18 环境。doc/11 同步获取及版本差异；无业务
接口影响，不更新 proto/API/doc02–08。`git diff --check` 通过；暂未运行 tidy：新
full/reference wiring 尚未完成，须先建立真实 compile DB。

## 第三能力：reference 机械隔离及完整旧行为构建

42 文件按首个规格逐字节移动，manifest 保存原路径/blob，check-reference.py 实际通过；
没有修改旧 C++ 内容/测试断言。旧 CMake 调整路径与 reference 输出、标签；
root 协议生成扩至全部 13 个文件，补入已存在 target_roundtrip 和两个 P01 内层测试。
缓存 JSON 头先校验后复用，固定 manifest 环境优先用 nlohmann_json CMake 包；不再
每次无条件重复下载。未创建新服务业务源码；未改 proto/API 语义。

实际验证：dev 配置/编译成功，9 tests 仅 loopback healthcheck 首次被沙箱拒绝，经授权
重跑通过。full 在本机 Homebrew 依赖（非固定环境）配置/编译成功，原 33 个 CTests
经网络权限授权全部通过；新增两个 P01 内层测试通过。full 编译覆盖检查、内层门禁、
旧边界门禁、固定 format 全通过。一次路径替换误伤 scripts/*/tests 的 CMake 路径已修正，
对应自测已重跑。最终 full 包含全部手写 TU，reference 没有被排除。

文档同步 doc/11 和本记录；新运行说明由各模块所有者更新。协议/API 无语义变化。
未宣称六入口/链接隔离或固定 PG 环境通过：它们留下一独立能力，依赖已审查共享提交。

## 第四能力：纯应用、配置与完整 P01 往返接线

已按主对话授权同步共享/Control、五应用、配置和 PKI 已审查提交；Git 目录重命名检测
曾把新增 main 建议移入 reference，已逐项恢复到 core/app（原内容未改）。
本能力仅新增 CMake/门禁，不修改这些生产源码。

cmake/platform.cmake：六个纯 application 库、每个内层头独立编译、外层配置库/driver；
p02_service_applications、p02_control_application、p02_service_config 实测 3/3 通过。
CMake configure/build 通过。P01 fixture 使用 WHOLE_ARCHIVE，实际 CMake 二进制和 SDK
Python 3.11.18 环境完成77 RPC策略/269消息往返，CTest p01_target_roundtrip通过。

新增实际配置图导出与递归链接隔离；5 项 architecture 自测通过（首次发现 LINK_ONLY
表达式解析错误，已修正并重跑）。实际六个 application 图检查通过，独立内层头编译
通过。服务 executable 默认仍缺，门禁不会报告六目标完成；full tidy 实际启动但被
compile coverage 拒绝，因为 runner 未集成，五 Core main 及 Control main 尚无构建条目。
未通过该门禁不算静态分析通过，后续接线后必须重跑。

环境增量：旧 uv0.13.5找不到3.11.18，经环境授权安装固定uv0.13.6后成功取得解释器并
uv sync --project sdk/python --locked --python3.11.18（实际命令带空格分隔参数）成功。
SDK生成产物/venv未提交；固定PG/gRPC依赖编译仍在进行。npm已用现有lock实际尝试，
先遇沙箱DNS，授权后遇本机TLS CA信任问题；保持校验开启，正在使用系统公开信任根重试。
无proto/API语义变化；doc/11同步构建/失败语义与真实检查，不更新其他所有者模块说明。

## 第五能力：实际六目标与 P02 独立黑盒

已授权runner提交接线为kiarabot_service_process，五Core+Control从各自入口和application
组合，统一bin目录。Host库名固定kiarabot_plugin_host_application/入口host_main.cpp。
门禁已扩展Control application target源码/头，允许自己的纯头但仍拒绝JSON/生成头/SQL。
6项architecture自测及实际六目标递归链接图/内层门禁通过；没有把reference作为新业务。

实际full编译六二进制成功。固定SDK Python3.11.18/grpcio1.85.0、固定CA CLI OpenSSL3.5.0
下运行 `ctest --preset full -L '^p02$' -j2 --output-on-failure`：11/11通过，包含lifecycle、
77owner/三桥接RPC mTLS/UNIMPLEMENTED、并行/绑定冲突/崩溃隔离、Core进程集成和夹具。
首次7项因自编OpenSSL未安装标准openssl.cnf失败；补跑上游install_ssldirs后全部重跑通过，
未放宽断言/TLS校验。内层独立头编译通过，P01 CMake roundtrip的77/269证据保留。
C++使用系统依赖，未冒充full-locked。实际编译覆盖已核对全部集成P02/reference TU；
尚未提交的P00 smoke.cpp独立收尾中，最终全仓门禁必须等该源接入后无排除重跑。

固定format16当前发现Core新service_config.cpp两处格式差异（stat大括号、STACK_OF宏参数），
属于Core源所有权；本任务仅报告，未越界格式化。实际tidy仍待最终全仓覆盖后重跑。
文档更新doc/11与本报告；没有协议/API语义变化，不改其他所有者模块文档。
