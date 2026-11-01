# P04源会话Control绑定类型能力

独立于Revoke与Register/Rotate。目标Core Identity持久会话类型与同事务端口契约，
无SQL/proto/crypto/CMake/旧Admin/delegation修改。

新SessionControlPeer保存subject和稳定CredentialId，SessionSecurity末尾control_peer
optional只表达缺失以供失败关闭；新建必须有效，refresh/revoke不可换。不能把可空当作
生产可省略或任意Control。valid_session_control_peer只验证两个标识非空，不认证身份。
Login必须从实际已认证Control构造，PG需同事务保存并在更新时检查不可变。

权威验证始终读该CredentialId当前head及原Control principal gate，核对签名actor链首项。
证书轮换后，只要同一Control当前身份仍有效，原会话可继续；绝不以旧leaf认证新连接。
缺值、空ID、主体错配、当前证书撤销/过期或主体禁用失败关闭，查询故障传播Failure。
父需协调Login/PG/Exchange接入；类型存在不表示这些路径已完成或可启用。

精确冻结5文件：

- core/include/kiarabot/core/domain/target/identity_types.hpp
- core/include/kiarabot/core/ports/target/identity_repository.hpp
- core/tests/architecture/session_control_peer_contract_test.cpp
- doc/02-core.md
- reports/modules/architecture-p04-session-control-peer.md

本批doc02只有源会话绑定章节，Revoke章节暂存于工作树外，未混入这次文档差异。
其余未提交Revoke文件不属于本清单。文档影响：doc02与端口注释需要同步且已同步；
线协议不传递此服务端状态，proto/API/运行语义/其他模块无字段变化，doc18只读。
验收为严格C++20纯类型测试、固定格式、边界/inner/diff；PG、Login和RPC另行验收。

实际运行：`c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Icore/include`编译上述
contract test并执行通过；固定clang-format16 dry-run通过；check-boundaries.py、
check-architecture.py --inner与git diff --check通过。未运行PG/Login/RPC，原因同上。
