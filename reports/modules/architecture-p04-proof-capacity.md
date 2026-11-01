# P04 签发前的证明容量门禁

成熟JWT适配器既有128条exact bindings上限。Core Issue现在用明确常量在权威读取/签名
前拒绝超限；不等待密码学层才拒绝，也不截断、扩权或把去重后的集合当原请求。
允许文件为identity_types.hpp、Issue实现、同名Admin/Issue测试、doc02与本报告。
文档影响：doc02说明更早的容量门禁；doc05已写128限制，无需改；线协议、API字段/错误码
及运行配置未变，proto/doc01/doc06/doc08无需更新。doc18只读。

C++20/Werror、ASan/UBSan/no-recover实际编译执行通过；129条在零session查询/零签名时
InvalidArgument，128条明确授权仍完整签发。受影响2TU精确DB tidy22 clang-analyzer
warnings-as-errors、format16、diff检查通过。RPC product前门禁由独立边界cap接入。
未重复全仓统一入口；后续生产组合里程碑执行。此为限额验证，不声称真实crypto/PG验收。
