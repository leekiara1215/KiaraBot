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

## 能力七：四typed PG工厂与关系仓储测试接线

接入已审查b25b40c（本树ee4cc92）及88bfeae（本树6a05fce），保留doc/08两侧追加段落，
版本行采用新schema v3，不改生产/测试源码。PG库增加factories.cpp，driver增加
 typed_work_checks.cpp，libpqxx/Threads及原fixture命令/120秒超时不变。
输入输出、Catalog枚举与schema语义由源提交负责；本轮只保证真实实现纳入构建/验收。

文档影响：更新doc/11和本报告的源与测试清单；Core/infra/运维语义已由源提交同步，
协议/API及doc/12-ci-cd不另改，doc/18保持只读。四mains/daemon测试未提交，未复制接线。
固定外部prefix/Python full-locked配置与完整构建成功；两个新TU逐项确认实际compile
command，全覆盖/inner/六目标图/p03注册均通过。全仓format16、新增两TU精确DB的
 tidy22 clang-analyzer-* warnings-as-errors通过。实际P03 fixture结果在提交前追加。
未重复原作者八TU全量tidy/ASan、旧测试统一入口，父将在下一批集成后统一验收。
本树没有将原作者结果冒认为亲跑；文档检查为路径/末尾换行与git diff --check。

本树P03 CTest 5/5通过，总19.99秒，其中真实PG15.58秒。

## 后续接线：P04 Identity契约测试

接入父已审查PG补强79f8f2a（本树4668b83）和Core d1ba916（本树8b2f010），
未修改生产/测试源码。Core CMake独立注册identity_contract_test为p04/unit，10秒。
输入为内层安全契约及纯规则，失败非零，不依赖实际认证/PG业务。
文档影响仅doc/11和本报告新增构建说明，Core与PG语义由原提交已有模块文档同步；
API/proto及doc/12-ci-cd不另改，doc/18只读。尚不把p04加为统一入口强制阶段，
其完整业务尚未实现；本次使用实际注册JSON单独检查p04。

固定外部prefix/Python配置与完整full-locked构建成功；新身份头独立编译、全部TU覆盖、
inner/六目标图/p04注册检查通过；全仓format16、新契约TU精确DB的tidy22
clang-analyzer-* warnings-as-errors通过。相关CTest选择p03全部及p04契约，结果后附。
未重复全仓统一入口（父负责最终）；未验证密码/JWS或生产认证，不宣称P04完成。
文档路径/末尾换行与git diff --check为本次文档检查。

本树相关CTest 6/6通过，总18.82秒；Identity契约0.42秒、真实PG16.14秒。

## 能力八：四daemon PG组合根链接及真实生命周期CTest

接入父已审查78917d1（本树a0477d0），doc/02冲突只加入真正新增的实际数据库装配节，
已有数据库组合输入/配置后组合钩子不复制；未修改mains/helper/测试正文。
PG外层CMake PRIVATE链接四owner daemon，纯application不链接PG；dev关闭proto，
六daemon本就不创建，不新增宏或改变dev依赖。新增p03_service_database使用原runner，
输入为固定工具/initializer及真实脚本；ENV从cache/target导出PG工具、服务目录和
Python/OpenSSL PATH，p03/integration/fault、timeout120；失败非零，无skip/替身。

文档影响：doc/11和本报告同步构建与测试运行方式；业务装配语义由原提交doc/02/08说明，
API/proto与doc/12-ci-cd无变化，冻结doc/18不改。
固定full-locked配置和完整构建通过；全TU覆盖/inner/六目标图/p03注册通过，全仓format16
通过；dev构建/CTest及相关full CTest、四mains精确DB tidy结果在提交前追加。
未重复完整统一入口，由父在集成后执行；本轮不表示后续认证/消息业务ready或P03全阶段完成。
文档校验为路径/末尾换行/重复章节检查与git diff --check。

实际dev配置/完整构建与15/15 CTest通过（13.90秒）；full相关8/8通过（40.43秒），
其中真实四daemon数据库测试6.04秒、存储fixture15.19秒。四main精确compile DB的
 tidy22 clang-analyzer-* warnings-as-errors通过，helper随四main模板实例一起分析。

## 后续接线：受审RPC适配器装配测试

接入20c16d9（本树6008a2a），doc/02与08自动合并，无重复旧章节。
仅修改platform.cmake两处：composition_driver PRIVATE显式链接kiarabot_proto，
其原CTest追加真实rpc_client参数。原p03/integration/fault标签及30秒超时保持。
输入为已冻结组合hook/driver/client，输出为实际RPC固定拒绝结果；默认N/I不算通过。
文档影响仅更新doc/11和本报告的构建说明；源提交已有Core/运维说明，无需另改
proto/API、其他模块或doc/12-ci-cd，doc/18只读。未补取不完整Model/Runtime契约链，
本树验证当前已授权源；没有改主树及其正在运行的统一入口快照。

固定外部prefix/Python配置及构建、相关CTest和四TU tidy结果在提交前追加；
全TU覆盖/inner/六目标图、直接边界、全仓format16已通过。文档路径/末尾换行/章节唯一
及git diff --check为文档验证。未重复全仓统一入口，由父负责最终集成后验证。

完整full-locked构建成功；相关CTest 3/3通过，总33.62秒：composition8.21秒、
原p02进程18.63秒、真实四PG daemon6.77秒。四TU精确compile DB的tidy22
clang-analyzer-* warnings-as-errors通过，p03实际注册标签检查通过。

## 后续接线：P04 Password/Random与Token HMAC外层适配器

接入02955e3/c6a11f8（本树c5f83d2/c1956ff），不改实现/测试源码。
根GENERATE_PROTO外层添加identity_crypto子目录；新库kiarabot_identity_crypto编译两生产TU，
PUBLIC适配器include与core/include，PRIVATE官方unofficial-sodium::sodium。
两真实测试均p04/unit/security、30秒；通过-UNDEBUG（MSVC /UNDEBUG）保持源码assert在
Release也生效。无PG/协议/crypto依赖进入纯application，不调用尚未实现的receipt工厂。
文档影响：doc/11和本报告增加工具链说明；infra/运维已由源提交同步，其他模块/API/proto
与doc/12-ci-cd无需变化，doc/18保持只读。

固定prefix/Python full-locked配置与完整构建成功；全TU覆盖/inner/六目标图/p04注册通过；
全仓format16及四新增TU精确DB tidy22 clang-analyzer-* warnings-as-errors通过。
P04真实CTest、dev注册隔离结果在提交前追加。未重复统一入口（父固定快照运行中），
未运行后续AEAD/JWT/PHC修订，不复制未提交源，不宣称完整P04或生产认证完成。
文档检查为源/链接路径存在、末尾换行与git diff --check。

P04 CTest 3/3通过，总1.88秒；password1.39秒、token0.49秒；dev配置与注册JSON
确认两外层测试未注册。文档路径/末尾换行检查及diff检查通过。
