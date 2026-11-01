# P03 服务数据库组合根

## 范围与文档影响判断

目标是Core外层进程配置/运行支撑及Identity/Runtime/Scheduler/Model组合根，允许
core/runtime/platform、四入口和对应platform测试；PG连接/事务实现仍由基础设施线程
维护，CMake由构建线程接线，Control/Host内层及业务规则不改。输入为严格数据库离散
配置和owner-only密码文件；输出为启动前工厂验证与writer锁、运行期依赖观测，不自动DDL。

需同步doc/08配置、退出与健康语义，以及doc/02最小组合责任说明；此项无新增RPC/
HTTP/WS字段、稳定业务错误或状态，proto/API和doc01运行语义无需重写。配置错误退出2，
材料/初始化/连接失败退出1，缺配置下当前未实现业务仍不ready；未来业务接线不能借此
走无数据库备用实现。doc18只读。后续分配置、runner组合钩子、实际四库装配独立提交。

## 能力一：严格数据库材料

专属control-api worktree的codex/p03-service-database基于c43b51e，避免修改主树正在
验收的快照。ProcessConfig增加外层可选DatabaseMaterial，仅四PG服务接受；单host/
端口/库名与角色严格校验，不能传DSN/TLS开关，密码由安全文件读取且不进入日志。
本提交仅配置前置，未装配PG工厂。配置诊断退出2；材料不可读/unsafe/FIFO退出1。

实际使用固定外部vcpkg/Python/CA/PG工具配置full-locked，再将本树CMAKE_CXX_FLAGS
设-Werror，构建kiarabot_service_config_driver通过；扩展的p02_service_config真实
证书/配置用例通过（0.91秒），包含四owner接受、Control/Host拒绝、未知/缺失字段、
端口/角色/host错误、owner-only/symlink/FIFO/空/多行/NUL/超限秘密和无泄露断言。
新改两个TU用同compile DB/SDK的tidy22 clang-analyzer-*及warnings-as-errors通过，
format16、Python编译与diff通过。原TLS时间/链/key/权限/FIFO用例保留。

文档实际同步doc/02和doc/08。未更新proto/API/doc01：无线协议或业务错误变化；
doc18无diff。未运行全仓：组合钩子/工厂接线另提交，最终集成统一检查由主树执行。

## 能力二：外层配置后组合与生命周期

新增ServiceComposition与run_service(service,composer,...)外层重载，原借用入口委托
同一路径。先CLI/材料校验及信号屏蔽再组装，空/错owner/显式失败/任意异常拒绝，依赖
检查监听前一次、运行后每秒，失败停止进程；RPC/socket先销毁，application再销毁，
最后释放回调持有依赖。实际启动行反映application当前readiness，本阶段仍false。

实际固定依赖-Werror构建kiarabot-identity通过；composition helper独立-Werror编译并
使用该目标精确link参数链接，真实证书/监听测试覆盖help/非法输入不调用工厂、信号先
屏蔽、空/错owner/两类异常脱敏、正常SIGTERM、依赖失效退出与析构依赖仍存活，均通过。
helper只是外层生命周期替身，不伪造PG或Identity业务。runner的真实compile DB tidy22、
helper显式C++20/SDK参数tidy22、format16/Python编译/diff通过。新TU/Ctest待构建线程
独立接线；未以人工链接替代最终全仓覆盖。doc02/08已同步，无proto/API变化或doc18变更。

## 能力三：四库真实进程装配

四入口使用postgres公开facade及对应纯factory，组合辅助只位于core/app；共享factory
覆盖application生命周期，不向domain/application/ports传递配置/数据库类型。
监听前验证writer/schema，每秒维护使用同factory短UoW，不调用无同步旁路连接。
忙Conflict不误判失锁，其他故障停止旧进程；探针仅读观测。无配置注入拒绝型工厂，
不会创建备用存储；当前业务未实现，DB成功仍不ready。

实际检查使用固定PG16.4/OpenSSL3.5/Python3.11及固定vcpkg前缀，full-locked -Werror
构建四服务和初始化CLI通过。新service_database_test.py通过拥有者私有临时TLS集群
运行四个实际daemon：九个不同秘密、迁移/runtime密码隔离、初始化后启动/live与不ready、
并发writer监听前拒绝、停掉合法writer后错误CA/密码/跨库拒绝、精确终止一个Runtime
数据库session后退出1/其他三进程隔离、全新Runtime重启、错误schema拒绝、恢复版本后
重启、四进程SIGTERM0，均通过。测试先核对实际data_directory再执行测试DDL，最终
回收集群和私有材料；不接外部DSN。日志/tmp/kiarabot-four-daemon-pg.log为本机证据，
不提交本机路径/证书/配置。未进行生产部署、真实QQ或模型调用。

五文件format16、Python编译、模块边界/inner检查通过；四入口同compile DB tidy22
结果在提交前确认。CMake临时接线只用于本树验证，提交前恢复；最终目标链接和CTest
注册由构建线程单独提交，再由主树验证。doc02/08同步；proto/API/doc01无线协议变化，
无需更新；doc18保持只读。完整统一入口在最终集成后运行，不重复宣称局部测试覆盖全仓。
