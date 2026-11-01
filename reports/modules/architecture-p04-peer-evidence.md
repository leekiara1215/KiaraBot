# P04 gRPC peer 证据

目标是Core runtime/platform边界，允许新增peer_certificate头/实现与对应C++/Python
测试；不改内层/协议/API/基础设施crypto/业务授权。输入为真实grpc::AuthContext，
输出为真实leaf指纹及叶在前DER链，错误为Unauthenticated/NotAccepted/Authenticate，
内存依赖失败为UpstreamUnavailable；不把body或metadata身份当作证据。

文档影响：同步doc02边界和doc08登记/拒绝说明，无新增RPC/API/状态/业务码，proto、
api与doc01无需更新；doc18只读。CMake最终由构建线程注册，未手改生成物。

公开gRPC1.71头明确server链可能不含leaf，session复用时链可空。提取先解析唯一leaf，
再补chain，仅容许一次相同leaf表示差异；重复中间/尾随文本/NUL/坏PEM/超限拒绝。
OpenSSL成熟PEM/X509编码和digest，不手写证书算法。当前函数不验证业务登记/链授权，
不能产生VerifiedPeer；权威lookup与PeerCertificatePort仍是后续真实RPC必经步骤。

实际C++20 -Wall/-Wextra/-Wpedantic/-Werror编译，使用固定vcpkg grpc1.71/OpenSSL3.5
及当前已生成Identity协议链接运行；真实mTLS server/client中读取真实AuthContext，
与openssl x509独立输出的SHA256匹配，无客户端证书时handler未被调用。属性负例
覆盖无认证/不安全transport、缺/重复leaf、重复chain、重复中间、PEM前缀/尾随垃圾、
NUL/超限；leaf-only、server去叶chain及重复leaf兼容表示通过。fixture仅返回证据OK，
不构造虚假的业务登录。临时证书由测试自己生成/回收。

首轮手工链接缺少macOS CoreFoundation导致失败，补全同固定Abseil所需framework后
测试通过；该本机参数只在验证命令，不写入源码。两个TU用固定tidy22 clang-analyzer-*
warnings-as-errors及实际SDK/依赖参数通过；三个C++文件format16，Python语法、边界/
inner及diff通过。正式CMake/CTest和全仓编译覆盖由构建线程接线后复验，不把手工链接
代替最终门禁。未运行真实Identity/PG授权、跨主机或生产证书轮换验收。
