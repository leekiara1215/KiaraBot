# P03 构建与统一验收接线

基点0d5fd94，分支codex/p03-build，专属kiarabot-ci-cd工作区。
允许根/模块CMake、cmake、scripts质量门禁、doc/11与doc/12-ci-cd和本报告；
不修改Core/Control/PG源码、协议/API、锁或冻结doc/18。不push/部署。

## 文档影响与验收口径

本轮输入是已审查PG数据库基础实现和Core端口契约测试，输出为可构建库、初始化CLI、
真实PG测试及p03分类；缺依赖/测试失败非零，不引入业务错误或状态。
同步doc/11的构建与fixture生命周期；doc/12-ci-cd无需改动（未改远端流程），
doc/02、05、08无需重复修改（业务/权限/运行接口不变）；proto/API/运行语义无变化。
doc/18保持只读；不宣称生产PG已装配或整个P03完成。

## 能力一：CMake与CTest

根proto构建增加独立infrastructure/postgres子目录，库私有链接libpqxx::pqxx7.10.0，
初始化CLI和driver复用该库。Core契约测试独立注册p03/unit。
PG测试p03/integration/fault、timeout120秒；Python fixture自己拥有完整临时集群
生命周期，测试之间无共享实例，不需跨CTest的fixture/resource锁。
内层头仍自动枚举，数据库依赖不传入纯application。

实际配置复用工具链树已验证vcpkg prefix（无下载），命令：

```bash
TCHAIN=/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain
VCPKG_ROOT="$TCHAIN/build/vcpkg" cmake --preset full-locked \
  -DVCPKG_INSTALLED_DIR="$TCHAIN/build/architecture-installed" -DVCPKG_MANIFEST_INSTALL=OFF \
  -DPython3_EXECUTABLE="$TCHAIN/sdk/python/.venv/bin/python" \
  -DKIARABOT_TARGET_PYTHON="$TCHAIN/sdk/python/.venv/bin/python" \
  -DKIARABOT_TEST_PG_BIN="$TCHAIN/build/pg-16.4-install/bin" \
  -DKIARABOT_TEST_OPENSSL="$TCHAIN/build/openssl-3.5.0-install/bin/openssl"
cmake --build --preset full-locked --parallel 4
ctest --preset full-locked -L '^p03$' --no-tests=error --output-on-failure
python3 scripts/quality/check-architecture.py --inner \
  --database build/full-locked/compile_commands.json --links build/full-locked/architecture-targets.json
ctest --preset full-locked --show-only=json-v1 > build/full-locked/ctest-registration.json
python3 scripts/quality/check-architecture.py --stage p03 --ctest-json build/full-locked/ctest-registration.json
```

配置通过；p03 2/2通过（真实PG3.21秒，契约0.48秒）；编译覆盖、内层、六目标实际图、
p03分类检查通过。全仓format16通过，四个新TU tidy22.1.8 clang-analyzer-*且warnings-as-errors
通过，使用真实compile DB及Apple SDK/libc++参数。首次tidy命令未引用星号被zsh拒绝，
引用后重跑成功，未关闭分析。构建结果在提交前追加。
全旧51项与全仓tidy不重复执行：父集成层安排统一验证，本轮仅新增四TU及新标签。
远端CI/Linux/部署未执行，本机macOS不能替代。文档校验为路径/链接存在与git diff --check。

完整full-locked构建成功（含reference、六服务、PG、自动内层头）。
