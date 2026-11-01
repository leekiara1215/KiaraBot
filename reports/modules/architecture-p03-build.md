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

## 能力二：统一入口阶段注册门禁

统一入口在既有完整CTest后同时验证p02/p03分类，防止漏接PG测试后剩余旧测试仍全绿。
仅脚本门禁变化，无业务/协议文档影响；同步doc/11和本报告即可。
实际执行bash -n、两阶段真实注册JSON检查、architecture自测、git diff --check。
不重复父统一全仓入口，也不以门禁自测冒充业务测试。

## 能力三：Core持久化规则测试注册

接入父已审查b2761da（本树3ab1bba），同时接入e86d66a工厂facade声明（本树279aabc），
不修改这些源码。Core CMake以同一循环为contract/rules创建独立目标，均p03/unit、10秒。
文档影响仅工具链新增测试清单，更新doc/11和本报告；Core业务说明由原能力提交同步，
协议/API/infra/运维与doc/12-ci-cd无需变化，doc/18只读。

实际沿能力一的外部固定依赖命令重新配置并完整full-locked构建成功；
首次仅运行preset覆盖Python3_EXECUTABLE为本树不存在的venv导致配置失败，
显式补回工具链SDK Python与installed prefix后成功；未下载或改锁。
编译数据库覆盖全部手写TU（包含新增rules），内层和六目标链接图通过；p03注册分类通过。
新rules头/测试format16 dry-run通过，rules TU tidy22 clang-analyzer-*及warnings-as-errors通过。
P03三项CTest全部通过，包含既有真实PG故障fixture；未重复旧51项/全仓tidy，父正在对
固定快照运行统一入口，本工作树未修改主树或其他对话源码。
文档校验：路径存在、末尾换行、git diff --check通过。

## 能力四：外层组合生命周期测试接线

接入已审查5633bf4（本树93cee26），不修改其源码。platform.cmake在service_process
定义后新增service_composition_driver，私有链接该外层库；对应Python测试
p03/integration/fault、timeout30，并复用固定openssl目录的PATH设置。
输入为driver及本次临时TLS配置，失败非零；不增加业务行为或数据库装配。
文档影响仅doc/11及本报告；业务说明已由5633bf4更新doc/02与08，API/proto、其他
模块文档与doc/12-ci-cd不变，doc/18只读。

沿本报告固定外部prefix/Python命令配置、完整full-locked构建成功；新增TU进入compile
DB，全覆盖/inner/六目标link与p03注册检查通过。新helper format16 dry-run、精确DB的
tidy22 clang-analyzer-*及warnings-as-errors通过。dev配置及注册JSON确认该测试不存在。
相关CTest选择p03全部加p02_service_config/p02_core_process，结果在提交前追加。
未重复完整统一入口：父刚完成固定快照验收，新一批代码统一验证由父负责。
文档路径/末尾换行及git diff --check作为本次文档校验。

相关CTest 6/6通过，总22.92秒；composition2.96秒，真实PG1.96秒。

## 能力五：目标纳秒时间测试注册

接入父已审查a7d5577（本树e483f16），仅doc/02末尾追加冲突保留双方章节。
不改时间helper/测试/配置源码，Core CMake将time加入p03纯测试循环，p03/unit、10秒。
文档影响仅新增构建测试说明，更新doc/11和本报告；时间语义及Core说明已由源提交负责，
API/proto、其他模块、doc/12-ci-cd不变，冻结doc/18不改。

沿固定外部prefix/Python配置并完整full-locked构建成功（正常-Wall/-Wextra/-Wpedantic）；
全TU覆盖/inner/六目标链接图/p03注册均通过；新time头/测试format16 dry-run通过，
新time TU精确compile DB的tidy22 clang-analyzer-* warnings-as-errors通过。
相关CTest为全部p03加p02_service_config，结果在提交前追加。
不重复源所有者已做sanitizers或父统一全仓；不宣称Linux/CI/完整PG精度验收。
文档校验为路径、末尾换行和git diff --check。

相关CTest 6/6通过，总9.29秒；time0.52秒、配置1.63秒、真实PG3.03秒。

## 能力六：PG公共事务源码构建接线

接入父审查f33c98e（本树0655a5a），仅解决doc/08追加冲突并保留双方段落。
库追加common_work.cpp/record_codec.cpp，PRIVATE Threads::Threads；driver追加
common_work_checks.cpp。fixture命令/120秒超时不变，不接未提交factories.cpp。
输入输出及失败语义由原能力实现负责，构建失败/fixture故障保持非零。

文档影响仅构建来源说明，更新doc/11与本报告；schema v2/事务行为由源提交已同步
infra/运维文档，proto/API、Core模块及doc/12-ci-cd不另改，doc/18保持只读。
固定外部prefix/Python full-locked配置及完整构建成功；全TU覆盖/inner/六目标图通过，
逐项确认三个新source有真实compile command；p03注册分类通过，全仓format16通过。
P03 fixture与六TU tidy结果在提交前追加。原作者ASan/UBSan为其证据，本树不冒认亲跑；
未重复ASan/UBSan、旧测试全仓与统一入口：父将在新一批集成后验收。
文档检查为路径存在、末尾换行及git diff --check。

P03 CTest 5/5通过，真实PG15.29秒；本树实际六TU tidy22 clang-analyzer-*、
warnings-as-errors通过（沿精确compile DB和Apple SDK/libc++参数）。
