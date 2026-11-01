# P04 Identity PostgreSQL 安全持久化（实施中）

## 范围与文档影响判断

目标模块为 `infrastructure/postgres/`，仅修改该目录、`doc/05-infrastructure.md`、
`doc/08-operations.md` 和本报告；CMake 由工具链任务接线，Core 端口由 Core 任务维护。
输入为已确认的 `IdentitySecurityUnitOfWork` 及安全值类型，输出为同一物理本地事务内的
关系仓储。仓储失败阻止部分提交；错误沿用 Core 的 Failure，不引入新稳定码。

完整 P04 将同步 doc/05 的仓储行为和 doc/08 的 Identity 初始化版本、引导与恢复要求。
本次第一步仅增加私有 SQL 借用入口，无外部可观察变化，因而当前无公共文档影响：
不修改 proto、API、运行语义或模块边界；不编辑受保护的 doc/18。

## 同事务基础

私有 `DatabaseAccess` 在创建具体 CommonWork 时同时返回拥有的 Core UoW 和非拥有
SQL 借用接口，不对旧 Identity 工厂或 UoW 做向下转换，也不启动第二个事务。
借用回调仅允许基础设施的同步本地 SQL/codec，禁止远程、crypto、用户回调与提交。
它复用原事务的 2 秒总预算、250ms 锁预算、失败污染和连接失效处理。
终态后接口拒绝回调；借用生命周期严格小于拥有的 UoW。

## 验收与检查

必须以真实 PG16.4 TLS/SCRAM fixture 验证：继承仓储写入在 SQL 借用中可见；
借用返回 Failure 后继承写入整体回滚；终态不能再执行 SQL；下一事务可继续取得 permit。
已执行并通过：

- 本地固定依赖 `build/p03/verify_build.py`：初始化 CLI、普通 driver、ASan+UBSan driver，
  C++20 `-Wall -Wextra -Wpedantic -Werror`。
- `run_database_test.py --test build/p03/database_test` 与 sanitized driver：
  两次独立自有临时 PG16.4/TLS/SCRAM 四库 fixture 全部通过并清理。
- clang-tidy22 对 common_work.cpp、database.cpp、common_work_checks.cpp，
  `clang-analyzer-*` 且 warnings-as-errors，通过。
- clang-format16 对全部六个变更 C++ 文件及 `git diff --check`，通过。
- 报告文档检查：UTF8、末尾换行、范围与无文档影响理由存在；doc/18 无差异。

未重跑根完整 CMake/CTest/toolchain：本步骤未改接线，完整集成由主任务和工具链任务执行。
本地固定构建和真实数据库测试覆盖本步骤的全部变更；不将其声称为根门禁通过。
本步骤不改变 schema；后续 Identity 安全 schema 使用独立版本 4，其余三库保持版本 3。

## 安全 schema 实施范围

下一步新增 Identity v4 关系和约束；doc/05 与 doc/08 已补充初始化版本、安全投影、
恢复窗口与显式引导标记说明，状态均为实施中。Core 端口是唯一内部语义来源，
不改 proto/API、稳定码或保护基线。验收要求四库逐库版本/manifest 严格匹配，
凭据头与历史/证书当前查找、会话和授予复合 FK、24h 收据/refresh 约束及标记初始化。

### schema 实际验证

Identity v4 的固定 PG16.4 descriptor 为 `d1aa0822c7286c329a60802278aaf62c`，
关系对象 100；另三库的版本、对象数和 descriptor 保持原值。通过独立自有临时库的
开发 probe 获取实际描述，再以正式严格等值校验运行全部 fixture，未放宽启动验证。

已执行 `build/p03/verify_build.py` 三种固定依赖 -Werror 构建，普通及 ASan+UBSan
`run_database_test.py` 各自独立创建四库，全部通过并安全清理。
新增真实约束验证覆盖 Pending 材料/状态、head 历史 FK、当前证书 fingerprint FK、
会话 version/rotation/expiry/revoked 一致、grant 版本一致、Permission40 接受/41 拒绝、
AEAD 纳秒与00ff密文、24h收据上限、refresh原peer/key关联与期限、清理级联和初始引导标记。
clang-tidy22 对 database.cpp/database_test.cpp、clang-format16、git diff --check 通过。
文档校验检查三个已更新 Markdown 的 UTF8/换行与 P04 说明；保护基线无差异。
本步无新增翻译单元、不改 CMake；根完整集成仍由主任务/工具链任务执行。
安全仓储与生产接线仍在实施，schema 通过不代表 P04 用例已完成。

## 凭据与引导仓储（实施中）

新增私有 `src/identity_security.cpp` 和 `.hpp`，由 SqlWork 借用同一事务；尚未提供
完整的安全工厂，不能声称安全应用已接线。凭据写入先校验 Core 纯规则/文本表示，再以
expected head version 条件推进并追加独立类型历史，登录索引也使用 CAS。证书查找仅
通过唯一 current-head fingerprint，随后核对当前版本实际材料；历史证书仍可按明确版本
读取但不能解析为新的当前头。引导查询缺失标记为 Internal，消费仅 Required+expected
条件更新；继承仓储和 audit 同一原子提交边界不变。

Audience 使用 position 关系列无损保留 Core vector 的顺序与内容，避免仓储擅自重排或
去重验证材料；相应 Identity v4 descriptor 更新为 `d74a1a8ae6ec77c1178dfae43d904edc`，
对象数仍为100。本变更仍为新空库 schema，无既有库自动升级。
需同步工具链库源清单加入 `infrastructure/postgres/src/identity_security.cpp`；本任务不改 CMake。
已执行并通过固定依赖初始化 CLI/普通/ASan+UBSan 三种 -Werror 构建、两套独立真实
PG 四库 fixture，以及新仓储/公共测试两 TU 的 clang-tidy22。clang-format16 与
文档 UTF8/换行/章节检查、git diff --check 通过；保护基线无差异。
测试覆盖三种已发行材料与 Pending、纳秒、audience顺序/重复内容、同材料撤销追加、
版本历史保留、证书轮换新旧指纹、登录CAS、错误CAS、文本NUL、材料损坏Internal、
引导条件消费/回滚/错误版本/标记缺失Internal。
开发中真实测试捕获 bytea 重复hex解码，已修正为复用既有摘要bytea编码并重跑全部检查。
根构建未在本工作树运行：CMake源接线由工具链任务负责；新增源需完成该接线再跑根门禁。

## 会话与 refresh 索引（实施中）

新增私有 `identity_sessions.cpp`，公共私有 typed query/失败封装移入 `identity_access.hpp`。
会话查询分别使用 access/refresh 验证索引，列表仅查询元数据且以 trusted max+1 单次
有界读取检测溢出，返回 RateLimited 而不截断。更新匹配 expected version 和 rotation，
created_at 不可改变；撤销可保留 rotation，轮换只前进一步，禁止整数回绕。
安全 save 同步基础 sessions 的相同版本/rotation/expiry/revoked；若调用方已在同一事务
写入完全一致的基础元数据可复用，但不接受不同内容。
refresh 恢复只返回当前或原 control peer/幂等键消费记录，期限按实际 DB UTC 裁决，
不信任外部 now；相同消费内容不延长窗口，改变内容 Conflict。
所有路径仍借用一个 CommonWork。已执行并通过最终固定依赖三种 -Werror 构建、
普通及 ASan+UBSan 两套真实 PG 四库 fixture；3 TU clang-tidy22 同时启用
clang-analyzer-* 与 readability-braces-around-statements，warnings-as-errors 零诊断。
修复当前任务凭据文件原有单行分支缺少大括号的问题，并复查格式/差异与文档。
新增测试验证 inherited session 元数据同步、current verifier、原peer/key消费恢复、
重复消费不延期、caller now不能复活过期输入、列表max+1拒绝、不变created_at、
错误rotation CAS与uint64回绕拒绝。所有检查用最终重建产物运行。
新增库源 `infrastructure/postgres/src/identity_sessions.cpp` 需由工具链任务加入；
公共测试仍在既有 TU，无新测试源。不改 schema/manifest/API/稳定码或 doc/18。
根完整构建/CTest留给主任务接线后的集成检查，本地不声称通过该门禁。

## 加密成功收据（实施中）

新增私有 `identity_receipts.cpp`，scope 三种明确分支和请求参数分别存储。查找按实际
DB UTC 排除过期；相同 scope+keyed commitment+参数返回原始 AEAD binding/密文，
不续期；改变承诺或参数为 Conflict。验证新的 binding 后，在独立容量锁下统计全部保留
记录（含待清理过期行，保证物理行数/关联历史有界），满额 RateLimited、不删除有效义务。只允许复用同 scope 的过期行，显式清理
每次1..1000，历史通过 FK 级联清理。存储不包含 token/proof/密码、不运行 crypto。
收据创建/保留时间原样保存，以数据库 UTC 再检查 created<=now<retain 与最多24h，
不得借用公共7日收据逻辑。nonce/ciphertext为bytea，包含NUL可无损往返。
最终固定依赖三种 -Werror 构建及普通/ASan+UBSan 真实 PG 四库 fixture 均通过，
2 TU clang-tidy22（分析器+强制大括号）warnings-as-errors、clang-format16、
文档UTF8/换行/章节校验和 git diff --check 通过，保护基线无差异。
测试覆盖原始ns/00ff、容量与过期未清理占用、相同请求重放不续期、变化承诺/参数Conflict、
24h+1ns和过期输入拒绝、三类scope区分、caller now不影响find/prune、清理释放容量与NUL。
真实TLS relay 丢弃 COMMIT 回执后，UoW为Unknown且旧实例停用，独立新连接按原scope
恢复同一原始AEAD binding/密文；非仅模拟断线，也不再次生成秘密。
开发期间ASan在增量 pqxx 参数列表扩容路径报告container-overflow；改为一次完整构造
18参并预留最终大小后，未关闭任何sanitizer检查，最终全量fixture通过。
新源 `infrastructure/postgres/src/identity_receipts.cpp` 需工具链接入；根完整门禁仍待
该任务接线后运行，本地不声称已跑根CMake/CTest。schema/manifest不变。

## 收据结构复用冻结Core规则（实施中）

5a1f5eb新增validate_secret_receipt后，本模块用同一纯规则取代较宽的本地结构判断，
保留PG TEXT/NUL表示检查和数据库UTC条件。Login rotation=1、Refresh input+1、
Delivery expected version+1由Core确定，正常/三个非法分支及真实Unknown恢复一起验证。
只改receipt实现/测试/doc05/本报告，无schema/API/稳定码/CMake变化；doc08现有保留与
清理说明不变，无须同步，doc18只读。最终固定依赖三种-Werror构建、普通/ASan+UBSan真实PG四库fixture、2 TU
analyzer+braces clang-tidy22和clang-format16全部通过；真实Unknown提交恢复仍通过。
三个新请求的错误Login轮次/Refresh跨轮次/Delivery结果版本均InvalidArgument且整体回滚。
文档UTF8/末尾换行/新增规则章节与diff检查通过，保护基线无差异。
无新TU；根完整CMake/CTest仍由主任务集成接线后运行，不声称本地已跑。

## 完整普通会话结果（实施中）

输入为冻结 Core a7d5d72/b45241d：SessionMetadata 原样移至共享service_records、
ReplayResult末尾追加index12，旧0..11不变；5a1f5eb纯policy/state规则也已取入。
本步骤仅改postgres公共结果schema/codec/文本校验/测试、doc05/doc08与本报告；
不改Core接口、proto/API/稳定码、CMake或保护基线。唯一来源为已确认Core typed接口。
Identity新空库版本5、其他三库4；新typed表保存created_at等完整事实，所有owner一致
更新manifest。文档影响为doc05结果类型/存储与doc08初始化版本；无须改协议/API，
外层响应本已有完整会话字段。不得以回查live行/默认now补created_at。
验收包括13种结果真实PG往返、纳秒/uint64边界、不变快照/NUL、严格schema和四库回归。

### 完整会话结果实际检查

固定依赖初始化CLI/普通/ASan+UBSan三种 -Werror 构建、两套独立真实PG四库fixture
通过并清理。3 TU clang-tidy22（分析器+大括号）、clang-format16及diff检查通过。
真实typed往返覆盖13种、极值纳秒/uint64、相同请求更改proposed数据仍原结果、NUL拒绝；
额外真实live会话从v1变v2/revoked/expiry+731ns后，独立新事务重放仍v1旧完整事实。
此前scope/secret Unknown提交、四库权限/角色/TLS/故障/锁/结构损坏测试仍通过。

| owner | 新版本 | 关系对象 | 固定PG16.4 descriptor |
| --- | --- | --- | --- |
| Identity | 5 | 102 | bee8eac8a5db285e43dc76af6280b35a |
| Runtime | 4 | 56 | 4d7533b0e3dcf560efc4c41c3f20d0a6 |
| Scheduler | 4 | 55 | 2d627dd9a143a5bf9f2f88d715584717 |
| Model | 4 | 50 | 94f2b4b0522c1328853bf98852f567e3 |

全部指纹由自有临时PG创建真实DDL获取，正式启动仍严格等值验证，未放宽门禁。
文档检查验证doc05/doc08/本报告UTF8、换行、当前版本/13种结果章节，doc18无差异。
无新增TU、不改CMake；根完整构建/CTest由主任务接入冻结提交后执行，本工作树未运行。

## 全局有效会话计数（实施中）

输入为冻结Core ba8e56c的count_unexpired()端口及既有唯一4096上限；输出为同一SqlWork
实际DB UTC、全库非撤销且refresh未来的size_t计数，失败沿用Failure、不返回假0。
只允许修改identity_sessions实现、既有PG测试、doc05/本报告；不改SQLschema、密文行为、
公共API/稳定码或CMake。Core负责最终新建事务的上限决策，不把授权规则放进存储。
文档影响为doc05仓储查询；doc08初始化/运行方式和其他契约未改变，无须同步，doc18只读。
验收为真实同事务/新事务计数、跨主体、过期/撤销排除、回滚，以及断后端故障不伪0。

### 全局计数实际检查

固定依赖三种-Werror构建、普通/ASan+UBSan真实PG四库fixture全通过；新增计数测试在
同一真实UoW写入两个主体的未来有效会话、过期/撤销会话，确认只增加2，之后新事务重读、
撤销的即时可见/回滚、新事务提交后过期和撤销排除均通过。
独立fixture管理员真实pg_terminate_backend断开持锁连接后，count_unexpired明确
UpstreamUnavailable，旧写入未提交、实例停用；不以成功0代替故障。
3 TU analyzer+braces clang-tidy22、clang-format16、文档UTF8/换行/章节与diff检查通过。
doc18无差异；无新TU/无schema/CMake变化。根完整CMake/CTest仍由主任务集成后执行。

## 精确赋权仓储与安全工厂（实施中）

输入为冻结Core bdc05b9元数据helper和5a1f5eb精确校验；仅改postgres实现/公共工厂声明/
既有测试、doc05/doc08与本报告。不改Core规则/API/SQLschema/manifest或保护基线。
精确关系为唯一授权权威；base grant仅Core生成的有效permission/resource元数据，
同id/subject/version CAS同步。坏形状写入InvalidArgument、坏行读取Internal、CAS Conflict。
允许新增identity_assignments.cpp由工具链任务接线；安全工厂保持P03接口兼容，用
同一CommonWork创建所有继承与新仓储，不向下转换旧工厂，不开第二事务。
验收包括真实精确tuple/元数据、非product/混合/NUL/CAS/损坏拒绝、公开工厂全部端口
同事务回滚、忙等待/生命周期/故障及既有四库回归。最终固定依赖初始化CLI/普通/ASan+UBSan三种-Werror构建、两套独立真实PG四库fixture
全部通过。4 TU（assignments/factories/common测试/typed测试）analyzer+braces clang-tidy22
warnings-as-errors零诊断，clang-format16、文档UTF8/换行/章节与diff检查通过，doc18无差异。

精确仓储测试覆盖：三个独立Assignment保持边界，role-A与explicit-B不混配、Catalog不
继承child、Core metadata字段一致、完整permission×scope product往返、非product/混合/
NUL InvalidArgument、version CAS与损坏product Internal。保存逐tuple写入，不按全表展平。
公开open_identity_security测试覆盖：同一个UoW经全部继承/安全端口写入，故意文本失败
后principal/credential/login/security session/base session/exact/base grant/authorization/
ordinary receipt/sealed receipt/operation与bootstrap消费整体回滚；正向共同提交并新事务
读取，journal审计也经同一CommonWork。busy begin返回Conflict；工厂销毁后活动UoW仍可用，
持锁期间新工厂被拒绝，UoW结束后新工厂可打开并采样。

无schema/manifest改变，仍Identity5/其他4。工具链只需新增库源
`infrastructure/postgres/src/identity_assignments.cpp`；其他均既有TU，公共头为
`kiarabot/infrastructure/postgres/security_factory.hpp`。根完整CMake/CTest由主任务接入冻结
提交后执行，本工作树未运行该门禁；上述真实库验证使用最终重建产物，不是mock。
该工厂为存储依赖，认证/crypto/实际HTTP应用的联调属于主任务，不能将仓储通过等同应用就绪。

## 真实公开工厂的重复会话写入复现（实施中）

检查已冻结App7f9a197调用组合风险，不修改App/crypto/Core接口/schema/CMake。
本步仅改typed_work_checks真实PG测试及doc05/本报告；文档影响是明确自动同步责任。
既有session_security.save已同步base sessions，因此再调用相同create-only或旧expected
base save应Conflict、污染并回滚整个UoW；不为错误调用放宽CAS。doc08/协议/API/稳定码
未变，无需同步，保护基线只读。验收为Login/Refresh/Revoke三个真实publicfactory组合。

### 重复会话写入实际结果

真实公开open_identity_security三种组合均复现：security save成功后，相同create-only
Login base写或旧expected Refresh/Revoke base写返回Conflict，commit为RolledBack。
Login两类会话行均不存在；Refresh/Revoke两类行保持原version/rotation/未撤销状态。
本步不改实现，未放宽CAS。已执行固定依赖三种-Werror构建、普通/ASan+UBSan真实四库
fixture全通过，typed测试TU analyzer+braces clang-tidy22、clang-format16、diff和文档
UTF8/换行/新增责任说明检查通过；doc18无差异。根完整CMake/CTest由主任务集成执行。
Core端口原注释“Base sessions() metadata shares this transaction/version”未明说自动写入，
建议由Core作者在其修复提交中明确“security save owns base synchronization; do not save twice”；
本任务未跨模块编辑端口或App。后续App+crypto+PG集成应取冻结修复后再验证，不能用本
负向存储复现冒充应用通过。

## 真实应用/crypto/PostgreSQL 集成（本地实测完成）

从冻结root d256908切出本工作树的codex/p04-identity-integration，保留87c0fa7负向证据，
只取已授权冻结App7f9a197/0d0aa22/38055e2和Admin7623d94/LoginReplay d6f1059；
不复制未提交源。此能力只新增postgres/tests集成fixture、最小runner登记与直接文档，
不改App/crypto/config/生产main/CMake或保护基线。

fixture使用已有自有PG16.4/TLS/SCRAM runner，连接前检查root所有权/普通0600秘密文件，
连接后在任何DDL前核对backend data_directory确为该root/data且非symlink；禁止外部DSN。
真实Admin.bootstrap_offline一次提交初始登记，再用真实Admin.authenticate_peer查询/验签。
Control证据来自内存BIO双端TLS1.3的SSL_get1_peer_certificate，临时CA/叶证书由成熟
OpenSSL构造和验证，非请求JSON或手造VerifiedPeer。真实libsodium负责密码/随机/HMAC/
AEAD，真实jwt-cpp/OpenSSL Ed25519负责委托；直接注入安全typed工厂，无fake crypto。
测试clock读真实本机UTC/steady，仅在同主机自有DB时间比较通过后声明本fixture可信，
不作为生产部署时钟证据或clock适配器实现。

已实际通过正常/错误密码、原键Login/Refresh真实密文恢复、Access/GetPrincipal、真实Issue、
保持page_size=1的token-only分页、Revoke普通快照重放、撤销后拒绝与新工厂持久状态。
未运行任何真实外部服务；上层HTTP/进程组合门禁不在本fixture声称范围。
文档影响仅doc05集成验收与本报告；协议/错误码/运维配置未改，doc08无需更新。

### 真实组合发现的缺口与冻结修复

最初普通及ASan在真实IssueDelegation处同样InvalidArgument：真实DB/clock带亚秒，
旧crypto要求NumericDate整秒。未伪造clock、未改签名端口、未跳过真实Issue。
父任务独立冻结7b3200b保守取整后再取入：iat/nbf=floor(actual now)，exp下取整且
不延长源有效期，实际checked_at与deadline仍原真实时间。另取冻结6eceee1/cea4bdf/
80beb54（同key Refresh恢复、无效snapshot码、NotFound审计），无本任务production改动。
此前3处重复base写由38055e2修复；87c0fa7负向真实CAS证据仍保留，不放宽存储CAS。

### 本地实际构建和检查

`build/p04/verify_application.py`（本地忽略脚本）实际执行两次固定依赖C++20编译，
`-Wall -Wextra -Wpedantic -Werror`，第二次附加
`-fsanitize=address,undefined -fno-omit-frame-pointer`，均通过。对应可重现命令如下；
KIARABOT_LOCKED_PREFIX指向工具链锁定的architecture-installed/arm64-osx：

```sh
c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -Iinfrastructure/postgres/include -Iinfrastructure/identity_crypto/include -Icore/include \
  -isystem "$KIARABOT_LOCKED_PREFIX/include" \
  infrastructure/postgres/src/{database,common_work,record_codec,factories,identity_security,identity_sessions,identity_receipts,identity_assignments}.cpp \
  infrastructure/identity_crypto/src/{password_crypto,token_crypto,receipt_crypto,peer_certificate,delegation_crypto}.cpp \
  core/src/application/target/{identity_admin_application,identity_delegation_application,identity_security_application}.cpp \
  infrastructure/postgres/tests/{identity_application_tls,identity_application_test}.cpp \
  -L"$KIARABOT_LOCKED_PREFIX/lib" -lpqxx -lpq -lpgcommon -lpgport -lsodium -lssl -lcrypto \
  -framework Security -framework CoreFoundation -o build/p04/kiarabot_identity_application_test
```

sanitized构建使用上述两个sanitizer参数及输出名kiarabot_identity_application_test_sanitized。
两个产物分别运行下列自有PG fixture，均完整通过并安全清理；backend mismatch负测先于
任何DDL且必须精确拒绝，runner仍保持秘密/异常正文脱敏：

```sh
python infrastructure/postgres/tests/run_database_test.py \
  --pg-bin "$KIARABOT_TEST_PG_BIN" --openssl "$KIARABOT_TEST_OPENSSL" \
  --test build/p04/kiarabot_identity_application_test \
  --initializer build/p03/kiarabot-pg-initialize
```

sanitized运行只替换--test产物名。已执行2新TU clang-tidy22，checks为clang-analyzer-*
与readability-braces-around-statements、warnings-as-errors，零诊断；clang-format16对
3新C++文件dry-run、runner py_compile、git diff --check、doc05/报告UTF8/换行/章节校验
通过，保护doc18无差异。无生成物/密码/key提交。

### 工具链接线清单与未运行检查

新增executable应只含tests/identity_application_test.cpp和identity_application_tls.cpp，
公共测试头identity_application_tls.hpp；链接现有Postgres、identity_crypto、Identity
Admin/Delegation/Security应用及OpenSSL::SSL/OpenSSL::Crypto/libpqxx::pqxx。
复用run_database_test.py，binary名kiarabot_identity_application_test（及_sanitized）
已登记安全backend mismatch负测；建议外层CTest TIMEOUT120。
本任务未改根或模块CMake，因其归工具链任务；因此未运行根CMake/CTest/全工具链，
不将上述直接编译的真实组合实测声称为根门禁通过。也未声称HTTP/部署配置/外部平台集成。

## 真实fixture显式签名材料metadata（独立test-only增量已实测）

从授权干净root c83078f建立codex/p04-identity-key-metadata-fixture；它已含冻结A47c23d8
及Exchange/loader前置，再取冻结B9cf880f。保留9393061只读缺口报告分支，不reset或复制
其他工作树未提交源。本能力只修改identity_application_test.cpp与本报告；不改任何
production/App/crypto/config/main/schema/CMake。doc05/08现有行为说明不变，无需另改，
保护基线只读。

原三字段DelegationKey和两字段Config在B后会因缺失真实metadata拒绝构造，测试不绕过。
fixture明确设置key.version=1、config.version=1、有限valid_until=实际受信clock now+1小时、
自有固定UUIDv4 Catalog锚点60000000-0000-4000-8000-000000000001；仍只有一个临时active
Ed25519私钥。使用make_delegation_crypto_bundle的同一配置构造成熟签名/验签与public
material端口，实际snapshot核对reference/version/finite expiry/Catalog一致。
每次真实Issue返回的proof还校验expires_at不超过签名key实际到期，未伪造clock或crypto。

忽略构建脚本在此前完整真实命令中加入identity_crypto/src/verification_keys.cpp，根CMake
由父/工具链成组接线。固定依赖普通和ASan+UBSan两次C++20 -Werror完整编译均通过；两个产物各自运行
run_database_test.py真实自有PG/TLS/crypto/App完整链，包含backend mismatch安全负测，
均通过并安全清理。已有bootstrap/Login/Refresh/Validate/GetPrincipal/Issue/分页/Revoke/
真实密文和普通快照重放链保持通过，签名和public snapshot确实来自同一成熟bundle。
changed TU analyzer+braces clang-tidy22零诊断、clang-format16 dry-run、文档UTF8/换行/
metadata章节与diff检查通过，doc18无差异。无secret/key/生成物提交。
本cap依赖冻结A47c23d8+B9cf880f（本地B为e22b5c5）；只交test/report差异，CMake未改，
根CMake/CTest由父成组接线后检查，本地不声称已跑。GetVerificationKeys RPC不在本测试
通过范围，也不以此次正常组合通过关闭P04。
