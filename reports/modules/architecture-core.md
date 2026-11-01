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
