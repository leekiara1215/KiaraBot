# P04登记完整普通收据类型

精确独立5文件，不含SessionControlPeer或Revoke实现：

- core/include/kiarabot/core/domain/target/identity_registration_snapshot.hpp
- core/include/kiarabot/core/domain/target/persistence.hpp
- core/tests/architecture/identity_registration_snapshot_test.cpp
- doc/02-core-identity-registration.md
- reports/modules/architecture-p04-registration-snapshot.md

RegisteredPrincipalSnapshot完整保存原Principal+初始CredentialStatusSnapshot，非秘密。
ReplayResult index13；既有index12 SessionMetadata按root已提交类型同步，本分支原本
尚未取到此variant，未重新定义其布局或语义。0..12不移动；Assignment后续可占14。
不重建当前状态、不逆拼ResourceGrant、不用无关variant替代复合返回。

父需让PG序列化新结果并做真实重启/原快照回放；Register应用尚未接线。Pending策略、
秘密receipt ID、实际peer/represented actor双重binding后续独立冻结，不混入本批。
文档影响：新增直接模块细节doc02-core-identity-registration说明收据/索引及未接线状态；
doc02总入口当前由SessionControlPeer批次冻结，故不混改，后续追加链接。proto/API既有
公开响应未改变，保护doc18不动。实际校验结果随后记录，不将类型测试声称为PG验收。

实际校验：严格C++20/Wall/Wextra/Wpedantic/Werror编译并运行新snapshot测试及原
persistence_contract_test通过；固定format16 dry-run通过；boundary、inner与diff检查
通过。未运行PG/RPC/全仓build：对应新variant持久支持和路由尚未接入，由父协调。
