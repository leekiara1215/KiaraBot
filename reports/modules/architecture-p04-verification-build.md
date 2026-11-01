# P04 验签材料构建接线

## 范围与基点

专属kiarabot-ci-cd工作树，从父准确256d57f建立codex/p04-verification-metadata，保留旧分支。
只改CMake、doc/11和本报告，不修改其他作者源码/配置、proto/API或冻结doc/18。
输入为公开验签材料/可信metadata及独立Core应用，输出为真实构建与独立测试；
缺依赖/测试失败保持非零，不能因公钥可读就授予权限或声明整个P04完成。

文档影响：doc/11同步实际构建/测试入口、本报告登记证据；源提交已同步doc/02、05、08
及各模块报告，接口/配置语义无需重复写入其他模块。proto/API、doc/12-ci-cd不变，
保护基线只读。本报告文档校验为路径、末尾换行、无冲突标记与git diff --check。

## B crypto / PG / production metadata

成组仅取父授权49fd926、9cf880f、38082d4、157498e，本树对应6de16ad、c1f7e79、
0bdf568、805ac4a；doc冲突保留双方真正新增章节，不复制未提交材料或改实现。
crypto库增加verification_keys.cpp和原循环verification_keys_test，p04/unit/security、
30秒、所有配置保留assert；测试仅取官方OpenSSL头路径，不再提前重复链接archive。
PG metadata仍原identity_application_test TU，配置与生产仍原clock/HTTPS目标，源码清单
保持准确。未重复注册纯A端口测试。按父后续分工，撤回本树曾准备但未提交的
identity-security.cmake expiry变体，由父独立接线；本提交不改该文件。

### 实际命令和结果

沿doc/11既有固定环境，以外部已验证toolchain vcpkg/prefix、SDK Python3.11.18、
PG16.4/OpenSSL3.5精确cache参数配置full-locked，VCPKG_MANIFEST_INSTALL=OFF复用。

```bash
cmake --preset full-locked -DVCPKG_INSTALLED_DIR=<verified-prefix> -DVCPKG_MANIFEST_INSTALL=OFF -DPython3_EXECUTABLE=<locked-sdk-python>
cmake --build --preset full-locked --parallel 4
ctest --preset full-locked --no-tests=error --output-on-failure -R '^(p04_verification_keys|p04_key_files|p04_delegation_crypto|p04_identity_security_clock|p04_identity_daemon_http|p04_identity_application|p02_service_config)$'
python3 scripts/quality/check-architecture.py --inner --database build/full-locked/compile_commands.json --links build/full-locked/architecture-targets.json
./scripts/quality/check-format.sh
ctest --preset full-locked --show-only=json-v1 > build/full-locked/ctest-registration.json
python3 scripts/quality/check-architecture.py --stage p04 --ctest-json build/full-locked/ctest-registration.json
```

实际完整构建通过；7/7 CTest通过15.77秒，其中正式配置2.75秒、clock0.47秒、真实daemon
HTTPS7.00秒、真实PG应用3.76秒、新材料0.59秒。11个受影响TU（crypto新源/test、
delegation/keyloader源/test、service_config、composition、clock源/test、真实PG应用test）
均用最终compile DB和Apple SDK/libc++参数执行tidy22 clang-analyzer-* warnings-as-errors，
exit0；全仓format16、全部TU覆盖/inner/实际图/p04标签通过。

完整构建观察到一条既有peer_certificate_evidence_test重复libcrypto archive warning，
来自非本次crypto库接线的直接链接；没有抑制或改作者实现，不声称无warning。
没有重复已经通过的129TU全库静态或统一入口，不宣称Linux/远端CI/部署验收。
父另行处理own-TLS-expiry变体，本树本轮不冒认该分支运行通过。
