# P04 Login 写入源 Control 绑定

目标 Core Identity 登录编排；允许修改 identity_security_application.cpp、原严格
编排测试、doc02及本报告。不改RPC/proto/API/crypto/PG或受保护doc18。

SessionControlPeer类型已冻结。新建会话在当前peer重新核验后，使用真实VerifiedPeer的
certificate.subject及credential.id，作为同一SessionSecurity保存的一部分；不接收body
自报来源。刷新基于当前记录复制并保持绑定，原秘密成功收据重放也不改写来源。
没有新增状态/错误码；只依赖仓储保存输入的已有单事务语义。

文档影响：doc02必须同步实际写入责任，已更新。PG schema及不可变约束由独立PG
cap同步doc05/08；本提交不声称旧schema已持久化新增字段。线协议不暴露该服务端状态，
proto/API/doc01无需更新。其他模块责任未变。

实际full-locked完整构建通过，31/31相关P04及真实PG/服务进程测试通过94.07秒；
新增严格断言验证真实peer字段写入、登录重放与两次refresh保持原绑定。四个受影响
Core TU使用tidy22 clang-analyzer-* warnings-as-errors，格式16、边界、完整TU覆盖及
实际链接图/diff检查。PG新schema与组合根持久读回仍需成组后另验，不重复声称完成。
