# 提示词：完整目标WSS与Panel恢复

你接手KiaraBot的WS工作流，目录`/Users/betta/Workspace/KiaraBot`。
先读`doc/plans/2026-11-04-next-agent-prompts.md`和`2026-11-04-control-ws-handoff.md`，
并按AGENTS读取规范与doc/01/02/06/07/08。你不是唯一编辑者，保留其他agent改动。

目标是完成已批准全部目标WS频道及Panel恢复，不止做协议或operations空壳。
WS-02/03已有真实PG/R/S当前授权通过证据；WSS新protocol/source文件尚未完成网络接线和验收。

第一阶段收口v1.2契约：Identity.ListLogChanges、Runtime.ListLogChanges、
Runtime.ListMessageMetadataChanges、Scheduler.ListTaskChanges。
旧logs按occurred_at/id翻页不能保证提交序tail，旧list_tasks只列非终态且无固定cutoff，
Runtime目前没有安全message metadata变化源；不能拿operation journal或Control已见ID集合代替。
你独占I/R/S/observations这四份proto及registry/spec/goldens/直接contract tests，保留v1.1和HERR。
明确安全typed投影，无正文、Task payload/target、原始审计context或平台秘密；精确scope/permission、
500ms有界查询、600s retention、提交序head/lowwater、typed gap。
每(owner,feed)独立持久epoch，拒绝跨family cursor；普通restart保留、restore/rebuild轮换。
避免scheduler↔observations循环import。先contract/docs/独立descriptor门禁，通知统筹统一生成和roundtrip。

第二阶段按单一PG文件锁接入真实I/R/S feed：同业务UoW原子记录safe change，提交序不使用nextval冒充，
终态/删除不丢事件，容量/过期有lowwater。新增schema版本/manifest必须真实PG describe后更新。
恢复工具接真实epoch轮换，并用owned backup→restore→旧cursor拒绝验证。

第三阶段完成现有锁定Boost.Beast/Asio/OpenSSL的真实WSS，不新增自写RFC6455协议栈。
覆盖HTTP Upgrade与origin配置、hello/resume/SESSION_READY、当前会话/在线权限、单owner/scope、
逐页安全来源、有界队列/背压、超时/关闭/停机取消。长连接不能占满REST四worker。
空闲无事件时仍及时发现撤权/到期；owner故障不能成功空流。
按冻结AsyncAPI完成operations、health、adapters、plugins、tasks、logs、messages、control及Host接入。

第四阶段接Panel：先建立session/cursor，再取权威REST快照，缓冲并有序应用增量；
cursor只在应用后推进。RESYNC重取快照，刷新/登出清空旧session；不重复管理写。

文件范围：Control target websocket新文件/IdentityAuthClient/https_endpoint/相关测试、
panel stream/api/views、直接API文档；cmake/platform.cmake由你接收其他worker片段合并。
service_config与doc08共享配置段交Host agent单编辑；RuntimeRPC/PG/CMake跨锁先协调统筹。
build/full-locked及公共生成由统筹独占。测试注册名称新增后给精确命令，不编造已经存在的目标。

回归至少保留ws02_postgres_operation_observations、ws03_operation_observation_application/e2e、
p05_runtime_identity_client；新加真实TLS WSS、全频道授权/故障/恢复及Panel测试。
只有整链真实通过才更新相应availability，最后交付文件/文档/实跑日志和未闭项。
