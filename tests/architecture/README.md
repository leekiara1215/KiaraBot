# P02 六进程黑盒验收

本目录只拥有验收与临时夹具，不修改生产实现、契约或构建。共享CLI/配置/probes/退出码以
`reports/execution/2026-11-01-architecture-p02.md` 为准，未来由工具链任务接入CTest的p02标签。
任何前置缺失均失败；夹具自测通过不表示六个服务已通过。

输入：显式指定六个二进制或逐服务环境变量、OpenSSL路径及可用的本机临时端口权限。
输出：逐能力断言和明确失败退出；CLI/配置预期2、运行/证书/绑定故障1、正常信号关闭0。
负面用例必须检查具体退出码及对应诊断/传输状态，不能把任意非零视为通过。
验收期限：启动最多10秒、单次HTTP/RPC最多2秒、TERM/INT退出最多5秒，超时直接失败。

证书自测：`python3 tests/architecture/fixture_test.py --openssl /path/to/openssl`。
TestPki只在TemporaryDirectory生成0700测试CA目录/0600私钥，六服务serverAuth证书含
localhost/127.0.0.1 SAN，单独生成clientAuth、错误CA及固定2000年已过期证书。
不读取生产CA，不关闭证书/主机名校验，不等待系统时间或修改全局时钟。退出清理全部产物。
OpenSSL路径必须明确给定；记录实际版本，本机不同版本自测不能冒充doc/11锁定环境。

计划分组：配置/live/ready/关闭；mTLS/UNIMPLEMENTED RPC；六进程并行/绑定冲突/崩溃隔离。
帮助必须0且不监听；缺/未知参数、错误schema/service及未知配置字段必须2；缺/错/过期
证书或端口冲突必须1。探针live200/HEALTHY、ready503/UNHEALTHY，未知路径404。
Control只提供回环HTTP探针且拒绝业务REST/WS；其他五服务mTLS gRPC未实现方法返回
UNIMPLEMENTED，不产生业务状态。证书缺失客户端拒绝不能仅用RPC超时证明。
