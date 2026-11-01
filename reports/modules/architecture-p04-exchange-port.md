# P04 Exchange 父证明验证端口

目标：纯Core crypto port，不改线协议。新增显式verify_exchange_parent，与普通直接发送方
证书绑定verify分开；输入是真实接收Runtime、父JWT、可信clock，输出签名/受众验证结果
或稳定Failure。Core仍拥有当前登记、原始来源、授权水位、收窄和第二跳签发决策。

允许文件：identity_crypto.hpp、两个纯Core测试的严格替身override、doc02及本报告。
文档影响为doc02内层端口语义；proto/API/RPC方法/错误码未变，无需改唯一线契约；
doc05成熟crypto实际实现由对应cap同步。doc18只读。

新增虚函数为纯接口。旧会话/Admin测试意外调用父交换入口必须abort，不返回伪成功；
未在主树单独集成，必须等待实际crypto实现，避免抽象类构造失败。严格C++20/Werror
编译执行既有两组应用测试、format16、boundary/inner与diff检查后冻结。
真实签名/交换负向矩阵不由此接口声明证明，归成熟crypto及Core交换应用后继任务。
