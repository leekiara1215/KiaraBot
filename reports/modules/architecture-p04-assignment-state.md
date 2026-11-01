# P04 授予撤销当前状态模型

目标 Core Identity 领域规则；仅新增 identity_assignment_state.hpp、对应纯测试，
更新 Core 测试注册及直接 doc/02。本步没有仓储、RPC、协议或服务配置变化；
不修改保护基线，也不声称 Set/RevokeAssignment 已接线。

采用有版本墓碑而非空权限或物理删除。AssignmentStateRecord 保留完整旧精确绑定，
Active 无 revoked_at、Revoked 必须有限正 UTC；损坏状态 Internal。
prepare_live_assignment 复用既有 G08 spec/CAS，不允许覆盖 revoked ID 或变更 owner。
prepare_assignment_revocation 复用版本规则、推进一次且不破坏旧 scope；已经撤销
的新命令 Conflict，原幂等键重放归应用普通收据。当前模型不包含秘密。

后续持久实现必须：按 ID 返回当前状态；for_subject 和 inherited grants 的有效读取
过滤 revoked；同一物理 UoW 维护 exact/base 版本与状态，禁止旧 base save 复活 ID；
保存旧 scope 以授权撤销和恢复。不能凭墓碑授予权限或把空 optional 掩盖存储故障。
原 Set 成功应保存完整、明确 typed exact 快照，不通过 live query 拼历史响应。

文档影响：doc/02-core.md 必须同步当前状态与 ID 不复用语义，已更新。现 wire
RevokeAssignment 仍只返回新版本，未加状态枚举或改变稳定错误码；proto/API/doc01
无需修改。PG schema/doc05/doc08 未改，不能据此宣称持久约束已实现。

验收：C++20 Wall/Wextra/Wpedantic/Werror 和 ASan/UBSan 执行规则测试，覆盖撤销后
改 scope/换 owner/重建拒绝、错版本、耗尽、无效时刻及坏状态。正式 full-locked
构建、针对本规则和公钥应用 CTest、tidy22 analyzer、format16、边界、全部 TU
覆盖与实际链接图检查；确切结果在提交前记录。没有改生成物或调用外部平台。

实际结果：上述严格 sanitizer 执行通过；full-locked build 通过，两个 CTest 2/2
通过（0.47 秒）；tidy 无诊断、format/边界/覆盖/链接图/diff 全通过。初轮测试
对已有版本耗尽规则错误期待 Conflict，按既有 Internal 契约修正后重跑通过，
未更改或放宽生产版本规则。全工具链/PG 不在本纯领域提交重复运行。
