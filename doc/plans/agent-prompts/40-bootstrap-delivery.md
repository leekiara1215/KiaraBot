# 提示词：Bootstrap凭据安全交付完整链路

你接手KiaraBot BOOT工作流，目录`/Users/betta/Workspace/KiaraBot`。
先读`doc/plans/2026-11-04-next-agent-prompts.md`、Host/bootstrap源码交接、批准子计划，
按AGENTS读取规范、doc/01/02/06/08及Runtime/Host冻结契约。你不是唯一编辑者，保留既有改动。

目标为完成BOOT-02/04/05与整链验收。BOOT-03的SecretDeliveryApplication、固定目录0600文件sink、
CAS/receipt/崩溃恢复已局部通过，复用已有实现，不重新造文件交付器。
先等统筹释放Runtime资源应用/PG schema/owner_gateway/Control组合根编辑锁；
默认WS新增feed迁移先落盘，Host gate/Control切片完成后再串行接你的共享文件。
可先独立写设计/端口与测试，不并发改这些文件或公共registry。

按冻结契约完成：
1. Runtime opt-in首次签发/持久delivery intent/摘要与真实凭据版本，Adapter和Plugin都覆盖。
   recipient省略必须保持旧创建/绑定、版本和幂等，无隐式部署或秘密交付。
2. GetBootstrapDelivery、Adapter/Plugin明确reissue及delivery report，
   权限/资源scope/expected version/幂等与稳定错误映射一致；普通REST只返回无秘密metadata。
3. Control把首次签发的短生命周期秘密直接交SecretDeliveryPort，
   recipient只可映射离线固定目标，客户端不能传路径、URL或命令；不要写日志、audit、receipt或数据库明文。
4. 受限执行/部署端安全消费并验证字段链；交付完成与peer注册、process运行、配置生效分别表达。

响应丢失先查原intent/receipt，不能重复创建或假装可从摘要恢复原文。
必须重发时使用显式授权重新发放、递增版本并撤销旧版本；旧worker不得覆盖新秘密。
文件写入和receipt采用已有恢复协议，不宣称跨文件原子。清理、退出、依赖失败不泄漏secret。

文件范围为Runtime资源/凭据应用与端口、Runtime私有PG持久化、Control owner适配/组合根、
固定recipient配置、受限消费端及测试/直接文档，具体路径先从交接源码核定。
公共proto/registry目前WS v1.2单编辑；若冻结wire有真实缺口先交统筹，禁止暗改或并发翻状态。
PG schema版本与manifest必须真实describe，不能猜hash；不自动迁移用户数据库。

保留p12_secret_delivery回归，并新增真实Runtime+Identity+Control+文件sink+受限消费端整链：
首次、无recipient兼容、重复/冲突、错scope/拒绝/撤权、响应丢失、进程崩溃恢复、旧版本迟到、
显式reissue及旧凭据拒绝、未配置目标/依赖故障、秘密不出普通REST/WS/日志。
完成文档、格式、构建/测试和字段级证据后再翻相应状态，说明已跑/未跑与风险。
