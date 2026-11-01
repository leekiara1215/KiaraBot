# Core 内层首轮实施

基点 6d64b2a；限定 Core domain/application/ports target、architecture 测试、doc/02。
P00 仅冻结本任务内层规格，不代表全项目环境/契约阶段完成。C++20，无新增依赖。
输入是可信边界转换的内部值，输出是显式 Outcome；失败不隐藏为不存在。
文档影响：更新 doc/02 与本报告；proto/API、doc/01、03–08 无外部路径变更无需同步；
doc/18 只读，doc/19–23 不修改。根构建与旧接口替换由集成阶段负责。

## 能力一：资源与失败基础

foundation.hpp 定义稳定身份、代次、版本与恢复上下文。验收：身份不可隐式混用，
依赖失败可保留未知受理事实；独立叶子构建不链接旧 Core/协议/SQL。

已运行叶子 configure/build/CTest（1/1）、clang-format 与 git diff --check，均通过。

## 能力二：上下文与本地事务端口

新增 application/target/context.hpp、ports/target/unit_of_work.hpp、business_ports.hpp；
区分可信身份与持久审计证据、提交未知与回滚。读缺失与失败分离，条件版本与
唯一收据/claim fence 为实现强制语义。业务专属端口是 P06–P11 接口预留，未实现。
文档更新 doc/02，本报告；其他文档影响判断同上。验收为独立编译与类型边界检查。

已运行叶子 build/CTest（1/1）、clang-format、git diff --check，通过。尚无 UoW 实现，
不声称验证数据库隔离、提交故障或恢复。

## 能力三：版本、代次与租约不变量

新增 domain/target/invariants.hpp 和 invariants_test.cpp；无外部传输错误码变更，
内部到期/旧尝试以 Conflict 表达，边界后续映射具体恢复上下文。文档同步 doc/02。
验收覆盖正常续租/ACK、旧连接/执行/尝试、缺省代次、到期、取消、版本冲突/溢出、
ACK 与 timeout 的两种顺序及总时长封顶；不把串行值测试称为数据库竞争验证。

边界检查发现 ports → application include，已将共享认证/交接值类型移到
domain/target/context.hpp，application 仅保留 CommandContext；无外层反向依赖。

对照主对话指定 112ec27:proto/tests/target_spec.json，补齐 instance/runtime_epoch fence，
测试跨实例与旧 epoch 拒绝；execution_outcome 对齐 KnownSucceeded/KnownFailed。
UoW 公共事务与 RuntimeUnitOfWork 仓储分离，其他私有服务不提供 Runtime 仓储。

实际检查：叶子 configure/build（AppleClang21、C++20、Wall/Wextra/Wpedantic/Werror）
与 CTest 2/2 通过；clang-format --dry-run --Werror、git diff --check、
python3 scripts/quality/check-boundaries.py、边界检查器 7 个自测通过。
./scripts/check-toolchain.sh 实际运行但退出 1：Required tool not found: clang-tidy，
因此其后 full/dev/协议/API/全仓 tidy 门禁未运行；根构建属于后续集成范围，
本轮只以叶子结果证明本模块。未引入新第三方依赖，不需下载依赖。
文档校验为 git diff --check 及新增引用路径存在性检查，通过。

## 下一阶段依赖与限制

- P01a 目标契约完成后，边界映射逐项核对内部 ErrorCode/状态/权限词表；
  当前没有生成协议映射，不承诺缺省字段线级拒绝已实现。
- P02 由主对话接入叶子目标与质量门禁，统一旧接口移除；
  本轮不修改现有 CMake、runtime_store、runtime、组合根。
- P03 按服务具体化 UoW：真实 PG 验证锁、唯一、隔离、提交未知，
  本轮仅有端口，没有 PG、网络、业务链路或六进程就绪声明。
- P06 等后续在服务 UoW 中暴露业务 Repository；权限在线验证、claim/容量/
  去重持久化、时钟信任与保留期仍待实现。
- doc/18 只读，目标 Repository/代次模型与首版快照差异遵循 doc/19–22；
  无兼容桥接、旧数据导入、双写或删除数据。

## 能力四：直接非平台调用者与 fence 补全来源

输入：平台服务/直接 Adapter/Plugin 的可信认证绑定；输出：可区分的 DirectPeer。
AuthenticationContext.direct_service 改为 direct_peer，包含 PlatformPeer、AdapterPeer、
PluginPeer 三种值；直接认证主体与被代表 subject 分开。Service 仍仅六平台服务，
持久 HandoffContext.source 与 audience 未扩词。Fence 额外 ID 的可信来源在头文件
及 doc/02 明确：Runtime 已验证注册状态按全部必需 wire fence 匹配后解析，不凭空构造。
目标仅 domain/target/{context,foundation}.hpp、foundation_test.cpp、doc/02 和本报告。
文档影响：更新 doc/02 与报告；无已接线外部接口/协议/运行方式变化，proto/API、
doc/01、03–08 无需更新；doc/18、19–23 未修改。未推进 P02 或注册/业务实现。
验收：Adapter/Plugin 可构造 DirectPeer，但不能构造平台 Service；source/audience
继续为平台 Service；原有租约/fence 不变量回归通过。

实际复跑：cmake -S core/tests/architecture -B /tmp/kiarabot-core-inner；
cmake --build /tmp/kiarabot-core-inner；
ctest --test-dir /tmp/kiarabot-core-inner --output-on-failure（2/2 通过）；
对三个改动 C++ 文件执行 clang-format --dry-run --Werror；
python3 scripts/quality/check-boundaries.py；git diff --check，均通过。
文档检查：上述 diff 检查及 target 头文件引用存在性检查通过。
完整 check-toolchain 未重复运行：上轮已确认缺 clang-tidy，本轮仍仅接口变更，
不宣称 full/dev/协议生成/PG/传输映射验证通过。下一阶段由边界实现注册绑定解析。
