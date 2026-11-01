# 开发交接文档验收

仅新增 doc/25-development-handoff.md 与本报告，分支 codex/development-handoff。
初次盘点基点8e9974a；复核主集成至97976f7，补充作者原SHA→集成SHA映射。
没有编辑其他工作树、读取其未提交源码、合入代码、运行真实平台、push或部署。

文档包含：P00–03本机证据/P04部分/P05–14新服务业务未实现的边界；reference临时
单机MVP与后移功能；owner分支、已提交未审取cap与明确WIP；逐项目标/范围/前置/验收；
git bundle迁移及未提交内容/数据/秘密的另行安全保全；新机按锁重建、生成proto、
最低本地闭环验收。后续doc24与最终干净集成SHA由主协调定稿，不编造未提交启动器。

二次复核已记录：PG state a030394、Registered snapshot4d7976f、Exact receipt fae5772、
完整codec97976f7已审取，不把它们永久列为未合并。MVP smoke67f91e5已提交，其相关3/3
通过为作者/主协调反馈；本任务未重跑。Assignment app71acedd仍有未解tidy诊断，不入MVP。
初次盘点 Core凭据/工具链WIP未提交；后续Core凭据已由父保存独立codex/p04-identity-wip
的938c9d2（14文件，前基点15fd119），工作树clean且未纳主集成/MVP；doc25已更新。
Register/Rotate仅声明、Pending/delivery的PG/crypto未接；工具链未提交清单仍依原git status。主集成少量组装WIP待作者测试提交。

文档影响判断：无代码、契约、配置或运行接口改变；仅新增交接入口。doc11/doc23及模块
报告作为已提交来源只读引用；doc24由主协调维护，本任务不抢写；proto/API、运行语义、
模块文档无需同步。受保护doc18未修改。

实际检查：git diff --check通过；doc25所有相对链接存在；引用SHA可由git cat-file解析；
Markdown中每个bash代码块单独bash -n通过；proto/tests/check_implementation_status.py通过。
未跑C++构建/测试/format/tidy或新机安装：纯文档任务且没有改变可执行代码，不能把文档
中的建议步骤称为已在另一机器执行。未备份/传送数据或秘密、未建立bundle、未push。

最新补充：MVP源码762d6e1/407dd8c/c37df48/994a8b4与构建冻结ecf3cdc均已核对提交。
明确今晚reference单机不启动PG/六服务、独立data不双写不导旧库；完整PG工具链步骤仅供
后续架构开发恢复。最终launcher CLI/doc24仍由父实际验证，不猜参数。
