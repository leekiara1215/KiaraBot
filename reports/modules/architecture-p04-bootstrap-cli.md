# P04 离线 Identity 引导组合根

目标：core/app 中可信离线命令，将部署metadata、受限密码文件和真实Control证书转换为
既有 OfflineIdentityBootstrap；不提供网络入口，不在内层解析JSON/证书/SQL。
允许文件：identity_bootstrap_main.cpp、platform同名Python夹具、PG测试runner最小额外
参数、doc08和本报告。CLI仅用公开安全工厂；业务、once-only marker和事务归Core Admin。

输入输出、错误/退出、确切JSON与运行命令已同步doc08。密码经安全loader+成熟Argon2id；
Control链由成熟OpenSSL解析元数据，PeerCertificatePort完整验证。初始授权须明确给出，
无隐式all/catalog。--status只读持久引导marker，用于提交Unknown后原状态查询。

文档影响：doc08新增运行配置/命令/恢复说明；Core公开接口、proto/API、SQLschema和稳定
错误码均未改变，doc01/doc02/doc05无需重写；doc11的构建/测试注册由后继构建cap同步。
冻结doc18保持只读。

已执行：C++20/Wall/Wextra/Wpedantic/Werror语法编译（正式生成proto与固定安装头）、
format16、Python语法、boundary和diff检查通过。尚未在本提交前运行完整链接、真实PG CLI
fixture或全仓统一门禁：新增executable/CTest由构建cap接线后立即执行，不能把语法检查
视为可用引导。Python真实夹具已包含后端data_directory所有权校验后才DDL、错误SAN、
不安全密码文件、重复JSON、原子引导/重复拒绝/status和秘密/路径脱敏断言。
