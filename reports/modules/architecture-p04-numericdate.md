# P04 真实数据库时间与 JWT NumericDate

真实 PG + 成熟 crypto + 应用联调发现：数据库时钟有微秒精度，Issue 应用直接把纳秒时间
传给要求整数 NumericDate 的 Ed25519 适配器，真实签发被 InvalidArgument 拒绝；整秒替身
未暴露该缺口。修复只在 Core 签发用例做保守表示转换，不放宽成熟 JWT 解析或伪造时钟。

实际当前时间仍为可信时钟与当前 DB 时间的较晚者，deadline/当前状态按原精度检查。
iat/nbf=floor_seconds(now)，exp=floor_seconds(min(iat+56秒, 来源 access/凭据/证书到期))。
到期只能缩短；签后仍用实际 DB checked_at 再验证，返回真实精度。

允许修改：identity_delegation_application.cpp、既有 Admin/Issue 测试、doc02、本报告。
文档影响：doc02 的签发时序与精度已同步；协议/API/JWT 格式及稳定错误码未改变，
proto/doc01/doc06 无需修改；doc08 已要求整秒与不得扩展期限，仍适用，无需重写。
冻结 doc18 保持只读。

实际检查：C++20/Werror 加 ASan/UBSan/no-recover 编译执行通过，新增 731ns 当前时钟与
987ns 来源到期测试，确认来源到期向下取整且 checked_at 不改。两个受影响 TU 的固定
tidy22 clang-analyzer warnings-as-errors、format16 和 diff 检查通过。
真实 PG + crypto 组合复测由独立存储夹具接入本提交后执行；尚未把此局部单测称为全链验收。
