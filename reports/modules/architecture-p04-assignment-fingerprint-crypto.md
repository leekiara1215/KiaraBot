# P04 成熟授予请求指纹

目标 infrastructure/identity_crypto。新增公开factory、EVP实现与独立unit test，
扩展本库CMake source/test列表，同步doc05和本报告。Core端口由8837be7冻结，
本树对应b913e16；没有修改端口、AssignmentApp、SQL、proto/API或保护基线。

输入为公开typed spec及target/expected。输出SHA256普通收据内容身份；结构无效
InvalidArgument/CorrectInput，EVP/资源异常UpstreamUnavailable/Diagnose/NotAccepted。
无数据库/网络/权限规则/密钥，线程并发不共享可变digest。每次独占EVP上下文，
RAII回收，工厂探测实际SHA256可用。没有自写哈希算法或以摘要代替身份检查。

编码及40/128/1024边界在doc05精确说明；域、长度、presence和sorted集合消除拼接
歧义并保留集合重排等价。独立Python hashlib根据文档格式生成Set/Revoke golden
vectors，C++实际EVP结果逐字节比对。测试还覆盖改变target/owner/basis/role/kind/
expected即摘要变化，create/update区分，重复/unknown/空/半对/NUL/超界拒绝，以及
资源128成功/129失败、uint64最大正expected无截断。

文档影响：doc05需要说明新端口实现和格式，已更新；Core接口doc02已随端口同步，
没有wire或错误映射变化，proto/API/doc01/运维/其他模块无需重复修改。CMake正式
注册p04/unit/security及保留assert，沿既有crypto库成熟链接，不额外重复Crypto archive。

实际验证：C++20 Wall/Wextra/Wpedantic/Werror，以固定OpenSSL3.5 debug静态库实际
编译链接执行；ASan/UBSan/no-recover通过。两TU完整clang-analyzer+bugprone检查
warnings-as-errors通过；初轮test main异常逃逸诊断以函数try/catch修复，没有禁checker。
format16、边界/inner、git diff --check通过。全根构建/编译数据库覆盖由父集成后执行，
未将本次手工两TU构建冒称全工具链或PG验收。没有生成物、第三方源码或秘密提交。
