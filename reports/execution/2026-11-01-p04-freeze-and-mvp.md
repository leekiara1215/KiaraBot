# P04收口并切换单机MVP

2026-11-01晚间用户明确：最快跑通可运行流程，插件机制、完整账号管理、多主机部署等
非核心内容后移；未完内容必须入库便于换电脑续开发。该要求取代“先逐项完成P04–P14
才交付”的短期顺序，长期设计与人类维护doc18不改。

暂停开启新的P04权限/凭据/失效流扩展。已完成独立能力保留；根目录仅结束已经开始的
cherry-pick，使当前接口、PG实现、普通结果13/14、构建和严格fixture一致。没有启用
尚未完成的Assignment/Register/Rotate路由，没有伪造readiness或删除原行为断言。

本次组合接线范围：cmake/platform.cmake的Exchange纯应用链接、Core新增契约测试
注册；两个只读Core fixture新增Assignment state未使用trap；Exchange/RPC手工Session
补精确原Control绑定。没有客户端字段或生产权限变化。文档影响仅工具链入口及本报告，
各源能力已经更新模块/协议/运维文档，不重复改API/proto或受保护doc18。

实际检查：full-locked完整configure/build通过，新增契约/指纹/Exchange/Check/真实PGApp
等定向CTest初轮9/10；RPC失败定位到fixture手工Session缺新绑定，补齐后独立1/1通过
2.93秒。没有放宽生产缺绑定拒绝，实际指纹使用OpenSSL EVP而非测试摘要。固定format16、
git diff --check已执行。根目录随后补验真实PG应用与通用PG两项，2/2通过20.40秒，
覆盖schema10及普通完整结果codec；不是仅沿用作者通过结论。未在用户收缩后重启
整仓统一门禁；先验收最小可用闭环。

单机MVP复用reference Core/SQLite、现SDK和实际NoneBot adapter；固定回复处理器仅为
随程序提供的最小能力，不扩建插件安装/发现/市场/热更新。默认使用本机OneBot协议
替身，不启动真实QQ或付费模型。新六服务Runtime仍未接消息业务，不宣称完成迁移。
