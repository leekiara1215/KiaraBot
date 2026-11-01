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
