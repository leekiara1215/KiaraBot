# P03 主集成与本机验收

## 范围和交付口径

用户授权持续完成主线、按能力独立提交；本节只关闭P03持久基础的本机门槛，不把
尚未完成的Identity、消息业务、面板或发布验收标为完成。受保护doc18未改。
模块输入/输出、错误和允许范围分别记录在Core持久端口、PostgreSQL、服务组合根及
构建报告；此协调记录不替代其协议和文档。无跨库事务、自动DDL或备用业务存储。

四私有逻辑库的typed factory使用同一writer session，业务live关系行、普通收据的
不可变结果、审计、outbox/inbox与operation按本地短事务提交。普通保留7天，容量
拒绝不删除已受理义务，控制lane独立；COMMIT回执丢失后旧实例停止，以原身份查结果。
四真实进程使用runtime身份，监听前验证schema/writer，依赖失效停止旧进程；其他
服务与库隔离，新进程重新验证取得锁。秘密独立、TLS verify-full/SCRAM channel binding。

## 集成提交

各开发提交已分别cherry-pick保留，未squash，未推送/部署。当前验收快照a712867包含
四typed工厂b8e6ffb、存储错误/损坏回执补强900225d、typed构建6f22079、四daemon
组合根644e3c5及其构建a712867。更早端口/时间/规则/关系收据均为独立提交，可从模块
报告和git历史追溯。P04首接口eac4576及新契约15a1d37/730ef3f也已纳入当前门禁，
仍明确未实现目标业务；新RPC装配与crypto增量在本快照后另行验收。

## 实际检查

主工作区执行固定环境的scripts/check-toolchain.sh，KIARABOT_CHECK_PRESET=full-locked，
退出0，末行为All toolchain and quality checks passed。本机日志
/tmp/kiarabot-root-p03-final-unified.log，未提交日志/本机配置/材料。

- dev构建及15/15 CTest通过，7.50秒。
- full-locked完整构建及58/58 CTest通过，202.63秒。包括真实PG存储fixture15.09秒、
  四daemon私有集群测试5.30秒，以及原有reference行为断言；reference仍不计目标业务完成。
- 独立infrastructure构建及10/10 CTest通过，12.70秒。
- 真实PG/JWT依赖smoke、固定registry/installed来源、六入口实际链接图、内层依赖、
  全93手写TU编译覆盖、阶段注册、format16和全93TU tidy22通过。
- 完整协议检查、descriptor兼容性正反场景和显式legacy reader通过；生成目标跨语言
  往返在CTest执行。46个HTTP操作映射/引用、目标schema、OpenAPI/AsyncAPI检查通过。
  OpenAPI仍报告12条warning，AsyncAPI有information；不宣称文档lint零提示。
- diff检查通过，doc18与本轮初始基点无差异。没有删除失败断言或放宽安全校验。

全链路测试在真实owned临时PG16.4、固定OpenSSL3.5/Python3.11/Node22/vcpkg前缀上
运行。测试自行生成和回收私有集群及材料，不接生产DSN。故障包括跨库/DDL拒绝、
重复writer、CAS、事务预算、Unknown提交恢复、坏schema/权限/材料、数据库断连隔离。
具体内存/存储故障的仿真与真实重现边界以模块报告为准，不能当作实际宿主磁盘写满。

## 文档影响判断与限制

主协调只更新doc23的状态与本记录，模块变更已在各独立提交同步doc02/05/08/11及
相关协议/API文档；本协调不改外部契约/错误语义，其他模块说明无需重写。doc18只读。
文档检查为路径存在、diff、完整协议/API解析及lint，以上命令结果已记录。

P04继续实现真实认证应用/安全仓储/crypto/Control闭环。Linux远端CI、真实跨主机、
QQ/外部模型和生产恢复部署未运行，归P14或外部联调；本机P03通过不能替代这些验收。
