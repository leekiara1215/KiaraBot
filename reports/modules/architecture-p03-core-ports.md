# P03a Core 服务本地端口

基点15a6a83，分支codex/p03-core-ports；仅Core target domain/application/ports、相关测试、
doc/02与本报告。无SQL/PG/JSON/Proto/框架依赖，不初始化数据库，不修改runner/CMake/
组合根/其他文档/保护基线。前阶段主集成门禁与真实PG验收未完成前不宣称P03通过。

## 首个接口能力

输入：可信内部资源/主体/时间/版本/请求摘要；输出：统一Outcome/Failure及四服务本地
事务/Repository接口。资源引用补kind，operation补owner/resource；Failure/Handoff/Command
引用一致，SHA-256内部32字节。业务成功结果是ReplayResult类型化完整快照，支持可重放
结果与created_at/至少7天保留/有界容量；秘密会话轮换的加密回放不在本普通收据范围。

公共原子组：receipts、journal(audit/outbox/inbox)、operations。source+message完整唯一键、
immutable envelope/hash、claim token/lease/now、inbox首次接收和保留/容量均显式表达。
claim默认上限批量100/租期30s，发送100次需处置，容量默认普通10000/控制1024/inbox10000。
Repository不自行提交；commit Unknown释放资源，只能原身份查询，不可假定回滚或自动重跑。

四工厂begin()不变：Identity/Runtime/Scheduler/ModelUnitOfWorkFactory，返回各自
Outcome<unique_ptr<...UnitOfWork>>；begin无外部参数，具体连接/时钟/策略由基础设施构造。
共同UnitOfWork暴露owner/receipts/journal/operations；各库最小规范化仓储见doc/02新章。
不要求P04–P13复杂业务仓储已实现。现有business_ports预留接口继续存在，其领域值移入
service_records.hpp以免UoW依赖其他ports或复制类型。

精确接口文件：
- core/include/kiarabot/core/domain/target/foundation.hpp
- core/include/kiarabot/core/domain/target/context.hpp
- core/include/kiarabot/core/domain/target/service_records.hpp（新）
- core/include/kiarabot/core/domain/target/persistence.hpp（新）
- core/include/kiarabot/core/application/target/context.hpp
- core/include/kiarabot/core/ports/target/unit_of_work.hpp
- core/include/kiarabot/core/ports/target/business_ports.hpp
- doc/02-core.md
- reports/modules/architecture-p03-core-ports.md

文档影响：同步doc/02与本报告；无已接线外部生产行为变化，proto/API唯一来源和doc/01、
03–08无需修改；doc/18只读、19–23依照不改。接口是P03准备，不是RPC/PG生产者声明。
暂未发现需修改线协议的遗漏；ResourceKind/OperationRef沿platform.proto既有词表，
任务/模型/生命周期枚举仅映射已有状态，边界仍待对应业务阶段实现。

实际检查：foundation_test.cpp与invariants_test.cpp分别以C++20
-Wall -Wextra -Wpedantic -Werror独立编译并执行通过；format16格式化及dry-run通过；
python3 scripts/quality/check-boundaries.py、check-architecture.py --inner、git diff --check
均通过。首提交只接口/模型声明，后续补正常/拒绝/到期/容量/冲突的纯规则测试。
真实PG并发/锁/提交未知与根CTest接线不在本worktree权限范围，未执行也不记通过。

首接口补充：UnitOfWork::current_time()返回Outcome<TimePoint>，采样调用时DB UTC而非
事务起点冻结时间或本机时钟；失败显式传播。所有now是应用从该端口取得的建议样本，
实现仍在条件写时用DB当前UTC重判。预算2秒/锁等待250ms，不跨事务或网络复用。
ReceiptCapacity/JournalCapacity明确仅可信应用配置，不允许请求自由放大；版本为uint64_t。

新增精确首接口文件：core/tests/architecture/persistence_contract_test.cpp。
该最小测试编译验证DB时间Outcome、四工厂所有权返回形状、任务/模型uint64版本，
并检查同ID不同resource kind的收据scope不会相等；独立-Werror编译执行通过。
更改后现有foundation/invariants两个测试再次-Werror编译执行，边界/inner/diff检查通过。
