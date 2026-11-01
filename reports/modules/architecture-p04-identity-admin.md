# P04 Identity离线初始化与真实Peer权威查询

## 组合复核：持久化初始主体绑定

实际六方法/RPC 组合要求当前凭据与主体资源绑定一致，公开 PrincipalRecord 也要求
binding。bootstrap 现在将 Human/Control 均绑定到各自自身 Principal 资源，与已有凭据绑定
一致；不会由外层补默认字段。严格 scripted repository 同时检查两种主体的绑定和类型。
此改动限 Admin 生产源、同名测试、doc02 和本报告，不改变线协议、API、SQL schema、
错误码或部署配置；其他模块文档无需同步，doc18只读。严格 C++20/Werror 加 ASan/UBSan
编译执行通过；两 TU 固定 tidy22（clang-analyzer warnings-as-errors）和 format16、
boundary/inner/diff 检查通过，未为局部绑定修复重复全仓统一门禁。

目标Core Identity；新增identity_admin_application.hpp/.cpp及注入端口测试。
只修改application/target、core/src/application/target、architecture测试及直接模块文档。
线协议/API未变化，不给HTTP/gRPC暴露离线bootstrap。doc18只读。

输入：离线可信composition预派生PasswordVerifier、已按可信CA/SAN校验的Control
CertificateBinding、明确的初始主体/凭据ID与精确授权；不接收CA私钥、不写SQL。
输出：已提交初始化状态或显式Failure。初始化只Required->Consumed；同一事务写入
Human管理员与Control平台主体、login、两个Active有限期凭据、精确授权、authversion、
audit及marker。只Committed成功；Unknown不自动重试，返回Unknown/QuerySameIdentity。

Peer查询以真实TLS证据导出的leaf SHA256查当前唯一索引；检查当前credential、principal、
kind/service/binding/SAN/有效期后形成RegisteredPeer。不是CA信任即业务登记。
证书密码校验在无UoW时调用成熟PeerCertificatePort；之后再次读取当前登记防止
查询与验签之间已撤销/换代。依赖失败原样传播，缺行拒绝认证，坏存储Internal。

验收：严格C++20编译、注入端口正常/拒绝/依赖故障/Unknown测试、格式与架构gate。

## 短期Issue能力

新增identity_delegation_application.hpp/.cpp，实际实现Issue。使用既有六方法应用验证
Control的access proof，短UoW读取当前源session/credential及授权水位/精确grants，
tuple授权；本人List/Revoke沿用真实主体/Session owner规则。watermark前后核对，
避免READ COMMITTED授权拼接。随后无UoW调用clock/random/sign，再验证源状态，
失败销毁临时proof且不返回秘密。期限至多56秒并受源access/credential/cert expiry限制；
明确origin与hops1。

Exchange尚未实现：现有verify要求直接peer证书绑定，但父证明绑定上一跳Control，
Exchange实际调用者为Runtime，不能跳过绑定或把Runtime冒充Control。已向父报告
需crypto拥有者明确入口。没有伪造已验证父claims或自行解析JWT。

认可Issue/Exchange无业务副作用前提下改requires_idempotency=false，唯一来源修改由父
处理。本能力不改proto，也不拿普通CommandReceipt或假CredentialDelivery保存JWT。
初始Control grants由可信离线配置逐项显式指定，不创建隐式全局管理员权限。

## 精确应用能力文件

- core/include/kiarabot/core/application/target/identity_admin_application.hpp
- core/include/kiarabot/core/application/target/identity_delegation_application.hpp
- core/src/application/target/identity_admin_application.cpp
- core/src/application/target/identity_delegation_application.cpp
- core/tests/architecture/identity_admin_application_test.cpp
- doc/02-core.md（直接对应两个应用章节）
- reports/modules/architecture-p04-identity-admin.md

依赖已由父任务提交的5a1f5eb纯规则能力。本批只提交上述7个文件；doc/02差异仅为
Admin与Issue两个新章节，不再改动已提交的policy/state_rules能力。

## 实际验证（2026-11-01）

- 两个生产cpp与注入端口测试严格C++20/Wall/Wextra/Wpedantic/Werror编译执行通过。
- 测试不维护第二套数据库，repo为明确脚本回调，未预期查询abort；crypto仅为编排
  替身并断言无活动UoW，不声称完成OpenSSL/Ed25519真实验收。
- 覆盖bootstrap正常、marker已消费、audit失败、Unknown、deadline、Control角色拒绝；
  Peer正常、撤销、禁用、到期、缺登记、查询/crypto故障、crypto期间撤销；Issue正常
  self、跨主体拒绝、sign期间源撤销、源credential撤销而水位未变、sign/query失败、
  clock不可信。
- 八个既有target纯测试严格编译执行通过。
- Admin/Issue测试ASan/UBSan通过；固定format16的dry-run通过。
- boundary、inner架构gate与git diff --check通过。文档校验为diff空白检查及人工核对
  类型/方法/失败语义，没有协议schema变化，不适用代码生成或descriptor检查。

文档判断：doc/02和本报告需要同步且已同步；不更新proto/API（无字段、稳定错误码
或调用选项改动），其他模块文档无直接责任变化。doc18受保护只读。
未运行全仓构建、PG、RPC验收及check-toolchain全仓静态分析：新cpp/test的CMake与
生产composition归父任务接入，当前不声称ready或完成P04整体闭环。
