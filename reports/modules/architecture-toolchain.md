# Architecture toolchain 执行记录

工作目录：`KiaraBot.worktrees/kiarabot-toolchain`；分支 `codex/architecture-toolchain`。
起点 `6d64b2ae9a2f05ed8c01893f55be4bc77bf2b8fc`，初始工作树干净。

## 能力一：P00 依赖锁

范围：独立依赖配置、质量检查及其自测、doc/11、本报告。未修改业务源码、CI、
根/模块 CMake、协议/API、受保护 doc/18；不启动 PG、不清库、不安装第三方源码进仓库。
输入为官方固定 registry、已有 CI 版本；输出为 13 项精确 override、完整 registry
基点、port tree/portfile hashes、PG source hash 和两个 Node 归档 hashes。
错误：缺项、版本漂移、非法 hash、registry 不匹配/被改写、归档校验失败均非零。
验收：离线锁一致性和故障自测通过；安装/编译及平台运行验收单独列示。

文档影响判断：doc/11 必须同步依赖版本/获取方式/校验语义；本报告记录执行证据。
无需更新 doc/01–08、proto/API：本次没有业务接口/状态/配置运行行为变更；doc/12-ci-cd
待主对话统一接线，本轮未改 CI；doc/18 只读。新架构与 doc/18 的进程/中央 PG/内层
隔离差异来自已确认 doc/19–23，本轮未实现这些业务差异。

实际检查：

- `python3 scripts/quality/check-dependency-lock.py`：通过，明确 offline only。
- `python3 scripts/quality/tests/dependency_lock_test.py`：2 tests 通过，覆盖正常及四类漂移。
- 官方网络读取：经环境授权读取固定 vcpkg 版本表、portfiles/trees 和 Node SHASUMS；
  无编造版本/hash。源码归档本体未下载；未执行 archive/registry checkout 验证。
- 本机盘点：CMake 4.4.4、protoc 36.2、Node 26.12.1、OpenSSL 4.1.0、uv 0.13.5；
  PATH 缺 psql/pg_config/clang-tidy，不能当作锁定环境。

未完成的环境验收：vcpkg 目标依赖安装、PG server/client/driver 连接、JWT 实际签验、
CA CLI、Python/uv 二进制平台 hash、Linux 构建。版本/来源锁已落地，P00 全环境验收仍未关闭。
发布基础镜像 digest/安全升级留 P14，当前沿用旧 registry 不代表发布安全审计通过。

## 能力二：P02 构建规格与可测门禁

范围：`CMakePresets.json`、`scripts/check-toolchain.sh`、`scripts/quality/check-tidy.sh`、
`check-architecture.py` 及其自测，doc/11 和本报告；未创建六个业务入口。
输入为文件清单/compile DB、CMake codemodel、CTest JSON；输出为覆盖/内层依赖/
六目标/阶段标签诊断。无输入选择、无源码、缺 compile command、缺目标/阶段/分类
均失败。内层检查是直接 include/SQL 词法检查，非完整传递/link/SQL 行为审计。

文档影响判断：doc/11 同步构建规格、全仓 tidy 范围、label 和真实 PG fixture 接口。
协议/API 与业务模块文档无需同步，因未改业务契约/行为；doc/00 和 CI 文档需由主对话
在 P02 真实接线时统一更新；doc/18 未动。`full-locked` 选 manifest 但未实际构建，
不能宣称目标依赖环境通过。默认 full 的业务接线保持现状。

实际检查：

- architecture 自测 4 tests 通过（首次 macOS /var→/private/var 断言差异已修正）。
- dependency lock 自测 2 tests、现有 boundaries 自测 7 tests、现有边界门禁通过。
- `cmake --list-presets=all`：dev/proto/full/full-locked 四组解析通过。
- `bash -n scripts/check-toolchain.sh scripts/quality/check-tidy.sh`：通过。
- `cmake --preset dev`、`cmake --build --preset dev -j 2`：通过；AppleClang 21.0.0，
  CMake 找到 Python 3.12.15，不是锁定 Python。
- `ctest --preset dev`：8/9 通过，deploy_healthcheck 被沙箱拒绝 loopback bind；经环境
  授权 `ctest --preset dev --rerun-failed --output-on-failure`：该 1 项通过。
- `./scripts/check-toolchain.sh`：失败，缺 clang-tidy；无伪全绿。
- `./scripts/quality/check-format.sh`：失败，现有 C++ 基线多处格式差异（系统 clang-format）；
  本轮未改 C++，未越界格式化；不是新增脚本导致的源码变化。
- `check-architecture.py --inner`：失败，检出旧 domain/application/ports 的 pb/JSON/
  runtime/TLS 依赖，符合待 P01/P02 替换现状。
- 对 dev CTest JSON 执行 `--stage p03`：明确失败 `no tests registered for p03`。

下一阶段依赖：主对话集成 P01 后统一根/模块 CMake、六入口、外层 transport、独立
内层库与新生成目标；将目标架构门禁和 pNN wiring 接入统一检查/CI，更新 doc/00、
02、06、08、11、12-ci-cd。P03 fixture 的具体实现/数据库环境留专属模块，不能由本报告
假装已完成。full/tidy/锁定环境构建、PG/JWT 运行验收仍未运行：缺完整固定依赖环境，
且本轮尚未授权接线六入口。此状态不能关闭整体 P00 环境验收或启动 P02 业务实施。

补充验证：两份改动文档的本地 Markdown 链接检查通过；所有跟踪 `.sh` 的 `bash -n`
通过；`git diff --check` 通过。dev 打开编译数据库后，真实 codemodel 正确检出六目标
全缺，真实 dev compile DB 正确检出 48 个未覆盖翻译单元；因此未误报 full/P02 覆盖。

提交索引：第一能力 `5f43135`（依赖锁、来源校验及自测）；第二能力为本节所在提交
（架构门禁、full-locked/空测试失败预设、全仓 tidy、P02/PG fixture 规格）。精确 SHA
以分支 git log 和交付消息为准，避免为自引用 SHA 再造无意义提交。
