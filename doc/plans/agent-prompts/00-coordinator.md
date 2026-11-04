# 提示词：统筹后续实现

你接管KiaraBot五项扩展的后续统筹。工作目录`/Users/betta/Workspace/KiaraBot`。
使用GPT-6.1 Sol/high；具体代码任务交同配置subagent，你负责依赖、文件锁、集成与验收。
你不是唯一编辑者，不回滚、覆盖或清理已有未提交工作。

先完整读取`/Users/betta/Workspace/KiaraBot/doc/plans/2026-11-04-next-agent-prompts.md`
以及其中链接的三份源码交接、批准计划、冻结记录和台账；共同规则均适用。
用户已授权自主推进，无需逐步确认；不要把这个任务只做成新计划。

先检查git差异、当前进程、契约是否半成品、临时Host诊断及所有新源码的CMake接线状态。
为当前续执行保存新检查点，但不能替代丢失的开工快照。不要直接运行全量构建冲撞未完成契约。

按依赖分派：
1. WS agent先冻结v1.2四个owner查询及可靠feed；Model/Host可并行完善各自实现。
2. 原始descriptor兼容、生成、跨语言roundtrip、schema/API/status/evidence门禁由你统一验收。
3. 分批验收Model RPC→Runtime异步Bridge→SDK、Host终止修复→Identity→gate→Host/Control、
   WS可靠来源→真实Beast WSS→Panel恢复；不丢掉已批准的余下频道。
4. 串行安排WS feed、Host gate与BOOT的PG变更，真实describe后刷新manifest和运维说明。
5. BOOT签发/重发/文件sink/受限消费链完成后，安排整链故障与字段级登记。

统一持有build/full-locked、公共生成目录和质量门禁。只有一个agent编辑共享协议/registry、
PGschema/UoW、各公共CMake及RuntimeRPC/组合根；明确每次移交。
每个切片先给允许文件、输入输出/错误、构建/测试与验收条件，再分派；不得把局部pass扩大为整项完成。

最后使用同目录`90-integration-acceptance.md`执行验收，更新doc/27、doc/30和执行台账。
最终交付要说明五项状态及剩余外部环境限制，不自动发布或调用真实QQ/付费模型。
