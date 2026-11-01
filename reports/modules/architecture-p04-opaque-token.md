# P04 文本凭据随机端口

目标仅Core内部RandomPort声明和直接模块说明；既有bytes返回任意二进制，而TokenPair
和HTTP Bearer要求文本，不能直接把raw随机字节送入Protobuf string，也不能在Core自建
不安全SecretBuffer或密码算法。新增opaque_token明确32字节CSPRNG熵/64小写hex字符，
OwnedSecret保留安全分配/销毁，span不带NUL。成熟随机/编码实现由独立crypto能力提交。

文档影响判断：同步doc02和本记录。尚无外部producer行为变更，无新wire字段、状态或
错误码，proto/API/doc01不需修改；适配器实现同期同步doc05。doc18只读。
公开头独立C++20 -Wall/-Wextra/-Wpedantic/-Werror语法编译、format16、inner/边界及
diff通过。不能以纯声明代替实际随机实现/测试；合并本端口必须同时跟进实现提交，
现有SodiumRandom需实现新纯方法后才可构建。无数据库、网络、运行或生产验证声明。
