# P04 Issue与配置门禁的主树接线

目标构建/测试：RPC库链接已审Core issuer；配置CTest传Identity executable启用missingkey
TCP canary。只改cmake/platform.cmake、doc11和本报告，不改业务/唯一契约/保护基线。
文档影响为doc11实际构建命令与门禁；模块和运行语义由对应源cap同步，其他文档无需重写。

固定full-locked重新配置/完整构建成功；p04及p02_service_config相关20/20通过，41.47秒。
包含真实七方法边界、成熟JWT父proof、应用/真实PG/TLS、CLI及Control HTTPS fixture；
不是实际生产Identitydaemon→Control全部链路，也不代表整个P04完成。新Issue替身缺纯端口
已由独立testfix修复，没有删除断言。配置测试先前手工命令binary名写错而FileNotFound，
改用真实kiarabot_service_config_driver后通过，现正式CTest也覆盖。

编译成功但本轮重链暴露原crypto依赖顺序的duplicate libcrypto warning，单独构建修复处理，
不称零链接警告；不通过屏蔽flag或抬高最低CMake绕过。未重复全仓统一门禁，等待生产组合
里程碑；文档校验为diff检查和真实CTest命令/路径核对。

## Crypto archive顺序修复

成熟jwt-cpp目标本已传递SSL→Crypto。移除identity_crypto及两个测试中较早的冗余Crypto
链接；X509测试只借用官方Crypto头路径，依旧通过同一固定目标取得真实Crypto/SSL。
未关闭警告、抬高CMake最低版本或移除实际依赖。

固定重新配置、完整build及实际重链成功，最终构建日志无compiler/linker warning/error。
定向8/8通过8.22秒，包含真实parent JWT/Peer、七方法RPC、真实PG应用组合与CLI。
全TU编译覆盖、inner、六服务配置链接图、模块边界和diff检查通过。纯内部构建依赖整理，
无公共文档影响，doc11/协议/API/模块行为及运维配置无需重写；本报告记录实际修复。
未因链接顺序变化重复全仓静态分析，C++源和编译选项未改。
