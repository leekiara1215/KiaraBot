# 开发工具链与本地质量检查

前部Txx及旧dev/proto流程保留为reference历史；本轮目标构建、固定环境和实际验收以本文
P00/P02章节及模块执行报告为准。reference通过不表示六服务业务已经实现。

T06c 时区转换使用 Protobuf/gRPC 已依赖的 Abseil CMake 包（absl::time）和系统 zoneinfo；Linux 验收机安装 tzdata。独立 infrastructure 构建也显式查找该包，Core 领域代码只依赖 ITimeZoneResolver。新增 task_scheduler CTest 和 SDK test_tasks.py 覆盖跨进程恢复。

T06b 基础设施构建新增 libcurl >= 7.85（CMake CURL::libcurl）；JSON 编解码复用已固定的 Protobuf，不引入模型厂商 SDK。macOS 本轮验收使用系统 libcurl 8.7.1。协议注释/功能协商名修改仍需运行生成、descriptor 和契约测试；Provider 验收只使用本机 HTTP 假服务，不调用外部模型。

T03b Python SDK 固定 Python 3.11.18，依赖以 sdk/python/uv.lock 为准；从唯一 proto 源生成到 sdk/python/build/generated。运行 `uv sync --project sdk/python --locked`、`sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py` 和 `sdk/python/.venv/bin/python -m pytest -q sdk/python/tests`。真实进程测试要求先构建启用基础设施的 proto Core，不跳过缺少的二进制。Core 使用 POSIX posix_spawn 管理外部解释器，不链接 Python 库；当前仅支持 POSIX 进程管理。详细启动配置见 doc/08-operations.md。

本文件规定本仓库的工具版本和可重复的本地检查。不要把本机绝对路径写入仓库配置；通过 `PATH` 选择工具，使用 `CXX` 选择 C++ 编译器。

## 要求

| 工具 | 要求 | 检查方式 |
|---|---|---|
| Python | POSIX 进程测试需要 Python 3.9+，统一检查的边界/契约脚本也直接调用 `python3` | CMake 查找解释器、CTest 进程测试及脚本门禁 |
| ripgrep (`rg`) | 近期版本即可 | `scripts/quality/check-format.sh` 用它在 `.gitignore` 约束下枚举手写 C++；缺失时统一检查在构建开始前失败 |
| CMake | 3.24+ | 版本、dev/proto/full 配置和构建 |
| GNU Make | 3.81+ | 版本；预设使用 Unix Makefiles |
| C++ 编译器 | AppleClang 15+、Clang 16+ 或 GCC 13+，支持 C++20 | 预定义宏和 C++20 编译、运行 |
| protoc | 25+ | 版本、C++ 和 descriptor 生成 |
| grpc_cpp_plugin | 与 gRPC runtime 使用同一 release 系列 | 实际生成 gRPC C++ 文件；插件不保证支持 `--version` |
| protobuf runtime / gRPC runtime | 分别为 25.0+ / 1.60.0+ | proto CMake 配置、编译和 CTest |
| Node.js / npm | Node 20.19+ 且低于 21，或 22.12+；npm 10+ | 版本；此范围满足固定版本 Redocly 的运行要求 |
| clang-format / clang-tidy | 各 16+ | 版本及实际格式、静态分析检查 |
| Redocly CLI | 2.72.1 | `npx` 执行固定版本 OpenAPI lint |
| AsyncAPI parser | 3.6.6 | npm 安装固定版本到临时目录，解析和校验 AsyncAPI |

protoc 与 protobuf runtime 必须遵守官方跨版本兼容窗口；优先使用同一 Protobuf release。`grpc_cpp_plugin` 无通用的可解析版本输出，因此代码生成检查不能证明它与 gRPC runtime 属于同一 release 系列：安装或升级时仍须通过包管理器核对。CMake 的 proto 配置会检查 runtime 最低版本。网络或 npm 缓存不可用导致固定版本工具无法取得时，检查失败，不会跳过 lint。

## CI 固定工具链

GitHub Actions 的 dev 构建使用 GCC 13 和 Clang 16；proto job 使用固定到 `ce613c41372b23b1f51333815feb3edd87ef8a8b` 的 vcpkg registry/tool，解析 Protobuf 29.3（C++ runtime 5.29.3）和 gRPC 1.71.0。CI 工具锁文件为 `scripts/ci/package-lock.json`，其中包含 Redocly CLI 2.72.1 和 AsyncAPI parser 3.6.6；Node.js 使用 22。actionlint 固定为 1.7.7，Gitleaks 固定为 8.28.0。详见 [CI/CD 流程方案](12-ci-cd.md)。

CI 的 C++ 静态分析 job 使用 `build/dev/compile_commands.json` 分析 dev 预设的全部翻译单元（`clang-analyzer-*`，warning 即失败）；`control/api` 与 `infrastructure` 的完整覆盖仍由本机 `scripts/quality/check-tidy.sh` 在 full 预设构建后执行；该脚本先校验编译数据库覆盖全部待分析文件，缺条目直接失败。CI 另对跟踪的 `.sh` 文件执行 `bash -n` 语法检查，工作流 YAML 使用 python3-yaml 解析。**待 CI 配合**：`.github/workflows/ci.yml` 应改用 full 预设或等价的 proto+infrastructure 编译库，使 CI 与本机 static analysis 覆盖同一文件集（由 ci-cd 分支负责，本分支只提供 full 预设与脚本）。

## 统一检查

在仓库根目录运行：

```bash
./scripts/check-toolchain.sh
```

脚本遇到第一个错误即退出，缺少工具、版本不满足、生成失败或任何检查失败都返回非零状态。它依次检查工具版本（含 `rg`、`python3`、`ruby`）、C++20 编译和运行、protoc/gRPC 实际生成、全部跟踪 `.sh` 的语法、边界检查及其自测、dev 预设构建/CTest，再用 `full` 预设（proto + infrastructure + `CMAKE_EXPORT_COMPILE_COMMANDS=ON`）构建并运行 CTest，随后独立构建 infrastructure 并跑 CTest，最后执行协议契约、格式、clang-tidy、OpenAPI/AsyncAPI lint。统一检查覆盖 C++ 与契约侧；面板（npm）、Python SDK/适配器（uv/pytest）、`tests/acceptance/run.sh` 属于独立入口，不在本脚本内，以免缺少 venv 的环境被误判为全量通过。生成物只由构建写入忽略的 `build/` 或临时目录，不提交。

格式检查扫描仓库手写 C++ 头文件和源文件，排除 `build/`、`node_modules/` 和 `.pb.*` 生成文件。clang-tidy 显式启用 `clang-analyzer-*` 检查，并使用 `full` 预设的编译数据库（默认 `build/full/compile_commands.json`，可用 `KIARABOT_TIDY_BUILD_DIR` 覆盖）检查 `core/`、`proto/tests/`、`control/api/`、`infrastructure/src/` 与 `infrastructure/tests/` 的 C++ 源文件及其仓库头文件（覆盖范围以 `scripts/quality/check-tidy.sh` 实际 `find` 目录为准），并将诊断警告视为错误；运行前先校验编译数据库覆盖全部待分析文件，缺条目直接失败，因此须先完成 full 预设构建。边界检查同时覆盖 C++ include、Python import（测试文件豁免）、面板 TypeScript import 方向、CMake target 方向与 Core 头文件一跳传递依赖。macOS 的 AppleClang 构建使用 `SDKROOT` 或 `xcrun` 查找 SDK。CMake 会缓存已配置的编译器，切换 `CXX` 时须重新配置构建目录。AsyncAPI parser 输出所有诊断，severity 0 的错误或没有解析结果时失败；信息和提示仍会显示。

可以单独运行下列检查以定位失败，运行 clang-tidy 前需先完成 full 预设配置和构建：

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --no-tests=error
cmake --preset full
cmake --build --preset full
ctest --preset full --no-tests=error
cmake -S infrastructure -B build/infrastructure
cmake --build build/infrastructure
ctest --test-dir build/infrastructure --no-tests=error --output-on-failure
./proto/tests/check_contracts.sh
./scripts/quality/check-format.sh
./scripts/quality/check-tidy.sh
python3 ./scripts/quality/check-boundaries.py
python3 ./scripts/quality/tests/boundaries_test.py
./scripts/quality/check-openapi.sh
./scripts/quality/check-asyncapi.sh
```

各脚本均在仓库根目录运行，也可从其他当前目录调用其路径。OpenAPI lint 使用 `npx --yes @redocly/cli@2.72.1 lint api/openapi.yaml`；AsyncAPI 脚本使用 `@asyncapi/parser@3.6.6`，不依赖全局安装的 parser。升级这两个版本时应同时更新脚本、本文和版本兼容要求，并重新运行统一检查。

## 升级规则

- C++ 标准固定为 C++20，不随单个编译器升级改变；升级编译器、CMake 或协议工具后运行统一检查。
- 协议变更还需要提供上一版本 descriptor，按 `proto/tests/README.md` 运行兼容性比较；统一检查不会在未提供基线时声称完成跨版本兼容性验证。
- Python 3.11+ 与 Python 协议生成属于适配器项目的检查（`adapters/nonebot/`），不由本仓库统一脚本代替。

## 其他模块和 CI 的后续接入

本次没有要求 Core、协议、API 或其他业务模块修改公共接口；已修复的 Core 和协议测试改动仅涉及格式。以下事项在对应模块或流程落地时处理，不影响当前本地检查结果：

- **CI/CD**：CI 工作流位于 `.github/workflows/ci.yml`，安装本文件列出的工具和运行库，按 job 分别执行 dev 构建与 CTest、协议兼容与跨语言闭环验收、workflow lint（含跟踪 `.sh` 文件的 `bash -n`）、C++ 格式、clang-tidy 静态分析（覆盖 dev 预设全部翻译单元）、API 契约、依赖审计和密钥扫描，并将非零退出码作为失败。CI 不直接调用 `./scripts/check-toolchain.sh`，上述检查以该工作流为准；`doc/12-ci-cd.md` 描述完整流程方案。
- **协议发布和 CI/CD**：保存上一发布版本的 descriptor；协议变更的检查中设置 `KIARABOT_BASELINE_DESCRIPTOR`，按 `proto/tests/README.md` 执行兼容性比较。未提供基线时，本地统一检查只验证当前 descriptor 和契约，不证明与旧版本兼容。
- **新 C++ 模块**：手写 C++ 需通过统一格式检查。当前 clang-tidy 的编译数据库检查覆盖 `core/`、`proto/tests/`、`control/api/`、`infrastructure/src/` 和 `infrastructure/tests/`（以 `scripts/quality/check-tidy.sh` 的 `find` 目录为准）；引入其他 C++ 模块时同步扩展 `scripts/quality/check-tidy.sh` 和对应构建目标。
- **Python 适配器和 SDK 项目**：适配器位于 `adapters/nonebot/`，SDK 位于 `sdk/python/`；各自维护 Python 依赖、代码生成和测试，本仓库的统一 C++ 检查不覆盖其实现。

## SQLite 可选构建

T05b 新增 SQLite3 开发库依赖，仅在基础设施独立构建或根构建 `KIARABOT_ENABLE_INFRASTRUCTURE=ON` 时需要。本次 macOS 使用系统 SQLite 3.54.0；依赖版本不硬编码到 Core。发布镜像固定版本仍由 T11 执行。根 proto 集成验收：`cmake --preset proto -DKIARABOT_ENABLE_INFRASTRUCTURE=ON && cmake --build --preset proto && ctest --preset proto`。

## T02a 请求指纹

gRPC Core 构建显式链接 OpenSSL Crypto 3.0+，使用 EVP SHA-256 计算适配器请求指纹，不自行实现密码算法。沿用本机/构建环境提供的 OpenSSL；发布镜像的精确依赖锁定仍在 T11b 完成，不能把本机验收当成发布可复现性验收。

## T08b–T11a 控制服务工具链

新增 nlohmann/json **3.11.3** 单头文件，由 CMake 下载至忽略的构建目录并校验 SHA-256 `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`；不提交第三方源码。HTTP/WebSocket 使用 Boost.Beast/Asio，密码与加密使用 libsodium。CI 沿用固定 vcpkg registry `ce613c41372b23b1f51333815feb3edd87ef8a8b`：boost-beast 1.87.0#0、boost-asio 1.87.0#1、libsodium 1.0.20#3。本机验收为 Homebrew Boost 1.92.0_1、libsodium 1.0.22；版本记录不同，不声称已在 CI 固定环境执行。CMake 要求 Boost 1.86+，不得把本机路径写入仓库。

控制验收环境：`uv sync --project control/api/tests --locked --python 3.11.18`，固定 pytest 8.3.5、jsonschema 4.23.0、PyYAML 6.0.2、websockets 15.0.1 与 uv.lock 全部传递依赖；网络测试只连接本机临时进程。执行 `control/api/tests/.venv/bin/python -m pytest -q control/api/tests`。环境存在且启用基础设施时注册到 CTest；统一 tests/acceptance/run.sh 始终要求此环境，缺失会失败。面板 `npm test` 也使用该 C++ fixture，需先构建 proto + infrastructure；Node 22.12+ 支持原生 WebSocket。

格式脚本通过 rg 尊重 .gitignore，排除本机构建树和第三方 `.venv`、`.tools`、`.ci-vcpkg`，静态分析扩展到 control/api 与 infrastructure 手写 C++。本轮未安装 clang-tidy 时仍由统一检查明确失败，不能把格式/编译替代静态分析。

## P00 目标架构依赖锁（2026-11-01）

本节描述新架构的独立开发锁，不声称旧 full 构建已切换或发布镜像已验证。
锁位于 `dependencies/architecture/vcpkg.json` 和 `dependencies.lock.json`。
registry/tool 沿用 CI 的 `ce613c41372b23b1f51333815feb3edd87ef8a8b`；全部传递
port 由该提交的 baseline 决定，直接依赖额外使用精确 override。每项记录官方版本数据库
的 git-tree 和 portfile SHA-256；上游归档的 SHA-512 由固定 portfile 校验。

| 用途 | 固定版本（# 为 port revision） | 获取/用途 |
| --- | --- | --- |
| PG 服务端 | PostgreSQL 16.4 | 官方 tar.bz2；锁内 SHA-512，独立编译 server |
| PG 客户端 | libpq 16.4#0，client/openssl features | vcpkg，含 psql；不包含可运行 server |
| 唯一中央 C++ PG 驱动 | libpqxx 7.10.0#0 | Repository/UoW 外层使用，不进入业务内层 |
| JWT/JWS | jwt-cpp 0.7.0#1 | 固定 `jwt::algorithm::ed25519`，不自动允许其他算法 |
| TLS/CA 工具 | OpenSSL 3.5.0#0 | libssl/libcrypto；同版 openssl CLI 用于离线根/受限中间 CA |
| 密码/秘密 | libsodium 1.0.20#3 | 不自行实现密码算法 |
| 协议 | gRPC 1.71.0#0、Protobuf 5.29.3#0 | protoc 对应 29.3，plugin 与 runtime 同源构建 |
| 本地日志 | SQLite 3.49.1#0 | 仅 Host/Adapter；不作中央 PG 后备 |
| HTTP/WS | Boost.Asio 1.87.0#1、Beast 1.87.0#0 | 传输边界 |
| Provider HTTP | curl 8.13.0#1 | 基础设施 |
| 时区/JSON | Abseil 20250127.0#0、nlohmann-json 3.11.3#1 | 仅外层适配器 |
| Python | 3.11.18 | 与现有 CI 一致，由 uv 管理；各项目 uv.lock 锁包与归档 hash |
| Python 管理器 | uv 0.13.6 | 与现有 CI 一致；安装包来源 PyPI |
| Node | 22.14.0 | 官方 linux-x64 / darwin-arm64 归档，锁内官方 SHA-256 |

以上沿用现有 registry 的开发基点，不表示完成发布安全审计；发布前应以协调后的新
registry 统一升级并重验，而不能独立替换某个系统库后仍宣称同一环境。
[官方 registry](https://github.com/microsoft/vcpkg/tree/ce613c41372b23b1f51333815feb3edd87ef8a8b)
保存完整获取/补丁/hash 规则；[jwt-cpp 固定源码](https://github.com/Thalhammer/jwt-cpp/blob/v0.7.0/include/jwt-cpp/jwt.h)
定义 Ed25519 接口；[Node 官方校验清单](https://nodejs.org/dist/v22.14.0/SHASUMS256.txt)
是两个 Node 归档 hash 的来源。锁不是下载的第三方源码。

获取及验证（命令从仓库根运行；网络/安装需遵守环境权限）：

```bash
# 将官方 vcpkg checkout 放到忽略的 build/，先检出上表固定提交，再 bootstrap。
python3 scripts/quality/check-dependency-lock.py
python3 scripts/quality/tests/dependency_lock_test.py
python3 scripts/quality/check-dependency-lock.py --registry build/vcpkg
build/vcpkg/bootstrap-vcpkg.sh -disableMetrics
build/vcpkg/vcpkg install --x-manifest-root=dependencies/architecture --triplet=x64-linux --x-install-root=build/architecture-installed
# macOS arm64 使用 arm64-osx；不能把它当作 Linux 验收。
# 下载锁内 PG/Node URL 后，先校验，禁止未校验就解包执行。
python3 scripts/quality/check-dependency-lock.py --archive build/postgresql-16.4.tar.bz2
python3 scripts/quality/check-dependency-lock.py --archive build/node-v22.14.0-linux-x64.tar.xz
uv python install 3.11.18
uv sync --project sdk/python --locked --python 3.11.18
uv sync --project adapters/nonebot --locked --python 3.11.18
uv sync --project control/api/tests --locked --python 3.11.18
```

PG 服务端从已校验的同一官方源码归档独立配置编译，例如在忽略目录中执行
`./configure --prefix="$PWD/../pg-install" --with-openssl --without-readline --without-icu`、
`make -j2`、`make install`；构建机需 C 编译器、make、OpenSSL 开发包和 zlib。
这不启动服务、不建库、不授予生产权限。CA CLI 也须核对 `openssl version` 为 3.5.0，
不能默认使用 macOS PATH 的其他 OpenSSL；vcpkg 不保证提供 CLI，缺失时从固定
openssl portfile 的同一上游源码/hash 构建 CLI。证书引导及签发自动化留 P04/P14。

离线锁检查只验证清单一致性；`--registry` 额外核对提交、port tree、portfile 和工作树，
`--archive` 流式校验指定已下载归档。三者都不证明依赖已经安装或 ABI 相容；错误非零退出。
Python/uv 安装器本身尚无本仓库归档 hash，只有精确版本及现有项目包锁；P14 镜像锁需
补全所选平台 Python/uv 二进制及基础 OS digest，不能把目前清单称为完整镜像复现。
本轮不更改 CI 或根 CMake；P02 由主对话协调 toolchain/manifest 接线。

## P02 构建与测试接线规格（待 P01 集成）

本节是实施规格，六入口尚未建立。主对话先集成 P01 契约/内部端口，再协调根
`CMakeLists.txt`、`core/CMakeLists.txt`、`core/cmake/bridge.cmake`、
`control/api/CMakeLists.txt`、`infrastructure/CMakeLists.txt` 和入口源码。不得提前
猜 RPC 名、在入口中实现业务或用旧 `kiarabot-core` 改名冒充拆分完成。

| 可执行目标 | 拟定入口 | 独立内层库 | 外层组合内容 |
| --- | --- | --- | --- |
| kiarabot-control | control/api/app/main.cpp | kiarabot_control_application | HTTP/WS、所有者 RPC 客户端，不链接 PG Repository |
| kiarabot-identity | core/app/identity_main.cpp | kiarabot_identity_application | 身份传输、PG 身份库、JWT/mTLS |
| kiarabot-runtime | core/app/runtime_main.cpp | kiarabot_runtime_application | 注册/业务传输、Runtime PG、远程服务端口 |
| kiarabot-plugin-host | core/app/plugin_host_main.cpp | kiarabot_plugin_host_application | 受控进程组、本地 SQLite、Runtime 客户端 |
| kiarabot-scheduler | core/app/scheduler_main.cpp | kiarabot_scheduler_application | Scheduler PG、时区、Runtime 交接客户端 |
| kiarabot-model | core/app/model_main.cpp | kiarabot_model_application | Model PG、HTTP Provider、秘密适配 |

库名和入口路径是本轮构建规格；P01 接口的实际名称和 namespace 以契约提交为准。
各内层库只包含所属 domain/application/ports，C++20、无生成头/JSON/SQL/HTTP 类型，
不通过 PUBLIC/INTERFACE 链接向内层传播 grpc/protobuf、数据库/JSON/TLS 库。协议生成
目标只链接传输适配器；外层基础设施通过 Core ports 依赖内层，组合根连接二者。
目标级 link 图审计及独立头编译需在 P02 接线时补齐；现有静态 include 检查不证明
任意间接依赖/预处理宏/运行时 SQL 都已被禁止。

生产旧目标待替换清单：`kiarabot-core` 的一体组合、`kiarabot_bridge` 同进程业务聚合、
控制进程直连旧内存对象/快照接线及中央 SQLite 生产链接。按能力迁移旧行为用例后移除
失效 wiring，不在本轮工具链提交删除旧源码/测试；`kiarabot-migrate` 的旧快照入口不能
自动成为新四库初始化入口。保留 FileStorage 独立实现也不代表批准文件服务。

输入：明确服务配置、受信证书/密钥引用、所属 schema 版本和依赖地址；输出：独立
进程、结构化诊断、live/ready/停止结果。配置或证书无效非零退出；未初始化/依赖
不可验证 ready=false，未实现方法稳定拒绝。进程活着不意味着业务 ready。初始化为
单独显式入口，生产 daemon 不自动建库；本轮不定义任何新 proto/HTTP 字段或状态码。
六入口生命周期测试需覆盖配置拒绝、依赖未就绪、独立崩溃隔离、优雅关闭；缺任一个
可执行/测试不能宣布 P02 完成。

`full` 保留现有配置行为并导出编译数据库。新增 `full-locked` 仅选择独立 manifest 与
固定 registry checkout，需先设置 `VCPKG_ROOT` 并验证；尚未执行该环境的配置构建。
它不改变默认 full、不自动下载 registry、不代表六目标已接线：

```bash
python3 scripts/quality/check-dependency-lock.py --registry "$VCPKG_ROOT"
cmake --preset full-locked
cmake --build --preset full-locked
ctest --preset full-locked
```

所有 test preset 均将无测试视为错误。`check-tidy.sh` 改用全仓 `rg --files` 手写 C++
清单（含新目录和 .cxx，尊重 .gitignore，排除 `.pb.cc`），先逐一验证 compile command，
再分析；header-filter 覆盖整个仓库路径。不能以只有 dev 的编译数据库冒充 full 覆盖。
若某个平台有真正不可构建的源文件，需单独协调有依据的目标/平台策略，不能随意忽略。

新增可独立执行的门禁：

```bash
python3 scripts/quality/tests/architecture_test.py
python3 scripts/quality/check-architecture.py --database build/full/compile_commands.json
# 以下是目标架构门禁：当前旧实现预期失败，P02 后必须接入统一检查/CI。
python3 scripts/quality/check-architecture.py --inner
# 先在 build/full/.cmake/api/v1/query/ 下创建 codemodel-v2 查询，再重新 configure。
python3 scripts/quality/check-architecture.py --codemodel build/full/.cmake/api/v1/reply/codemodel-v2-REPLACE.json
ctest --preset full --show-only=json-v1 > build/full/ctest-registration.json
python3 scripts/quality/check-architecture.py --stage p03 --ctest-json build/full/ctest-registration.json
ctest --preset full -L '^p03$' --no-tests=error
```

`--codemodel` 检查每个 configuration 的六个目标名，不证明它们已构建、是可运行进程
或有正确依赖。`--inner` 是内层直接 include/SQL 语句词法门禁，报告现有架构差异；
完整传递 link 图、头编译和运行验证仍是 P02 验收。不得只跑自测就报告业务门禁通过。
P02–P14 每个测试必须同时标记对应 `pNN` 与 `unit`/`integration`/`fault`；阶段检查
在 label 没有注册或缺测试分类时失败。已有旧 CTest 不在本轮擅自贴 pNN 标签。

### P03 真实 PostgreSQL fixture 接口规格

归属 `infrastructure/tests/` 及 CI/验收协调分支，本轮仅规格，未实现 fixture。
fixture 的 setup/cleanup 与每个测试使用 CTest `FIXTURES_SETUP`/`FIXTURES_REQUIRED`/
`FIXTURES_CLEANUP` 关联，另以 `RESOURCE_LOCK` 隔离共享测试实例；`TIMEOUT` 必填。

- 输入：专用临时 PG 实例（精确 server 版本、loopback/显式测试 host 白名单）、测试
  run UUID、受限建库管理员的秘密文件路径、CA 和客户端证书路径。不可从默认
  PGDATABASE/PGHOST 或生产应用配置推断目标，不在命令行/日志输出 DSN 或密码。
- 安全门禁：连接前核对显式测试 enable、host 白名单与 run 前缀；连接后验证测试
  实例 marker、server_version_num=160004 和管理员权限范围。任一不符停止，不能自动
  drop 一个已有同名库。远端测试环境须明确列白名单和 TLS verify-full。
- 输出：四个 `kiarabot_test_<run>_{identity,runtime,scheduler,model}` 私有逻辑库，
  各自运行/迁移账户、最小权限和仅属本 run 的 0600 fixture 清单。Control 不发 PG 凭据。
  清单含版本、run ID、资源所有权证明、秘密文件引用；测试从文件读，不写入构建日志。
- setup：创建空库、角色和权限隔离，调用各服务唯一初始化实现，再断言 schema 版本。
  非空/半初始化失败必须可诊断；不给运行账户 DDL/superuser/cross-db 权限。
- cleanup：只清理由本 fixture 创建且 marker/所有权/run ID 全部匹配的资源；收尾失败
  非零并报告脱敏资源 ID，不以大范围 DROP/删除本机数据修复。setup 中途失败同样收尾。
- 验证：p03 integration/fault 覆盖真实 PG 唯一约束、锁/并发、COMMIT 结果不明、断线、
  schema 拒绝、运行账户 DDL 拒绝和跨库拒绝。缺 PG、驱动、证书或秘密文件直接失败，
  不 skip、不回退内存或 SQLite。内存单元测试另标 unit。

## P02 reference 隔离实施接口

P01 已集成，P02 的旧实现隔离与构建规格见
[本轮工具链执行记录](../reports/modules/architecture-p02-toolchain.md)。旧 Core 内层文件
按 blob 不变原则迁入 `core/reference/`，保留 `target/` 和 `runtime/platform/`；旧测试不删
断言，所有 reference TU 继续进入 full compile DB/tidy。六个新进程只能链接新内层及
自己的外层装配，不能通过旧桥接库复用快照业务。构建的 reference 通过不代表目标
业务实现。具体实施/命令和未验项在上述记录随独立提交更新。

### P00 固定质量工具环境收尾

`dependencies/quality/requirements.txt` 记录 PyPI 归档 SHA-256，固定 clang-format 16.0.6
及 clang-tidy Python 包 22.1.8（其二进制报告 LLVM 22.1.8）。PyPI 不存在 tidy 16.0.6，
因此不虚构同版本包；本机初选19.1.0.1后按SDK兼容性验证升级22.1.8。现有CI tidy16与本机版本差异需保留。

```bash
uv --cache-dir build/uv-cache venv .tools/quality
uv --cache-dir build/uv-cache pip install --python .tools/quality/bin/python --require-hashes -r dependencies/quality/requirements.txt
PATH="$PWD/.tools/quality/bin:$PATH" ./scripts/quality/check-format.sh
PATH="$PWD/.tools/quality/bin:$PATH" ./scripts/quality/check-tidy.sh
```

工具环境/缓存均忽略，不提交。macOS arm64 已实际安装上述包并验证版本，固定 format
对基点手写 C++ 全仓检查通过；之前系统 format 23 的差异不是该固定版本的失败。
固定 vcpkg checkout 的 revision/tree/portfile 校验也已通过，依赖安装与运行能力仍需
另行记录；不能将 checkout 校验当成 PG/JWT 验收。

### Reference 机械迁移已落地

42 个旧 Core 文件已迁入 `core/reference/`，迁移 manifest 保存基点和 Git blob；
`python3 scripts/quality/check-reference.py` 验证内容逐字节保留，防止旧路径重新进入
生产树。旧 Control fixture/credentials 与一体 Core 输出仅位于
`build/<preset>/reference/{control/api,core}/`；既有 HTTP 测试的 build 根由 CTest 指向
`reference/`。旧 infrastructure 目标也在 reference 输出树，仍纳入全仓 tidy。
13 个 proto 全部进入生成/descriptor；现有 target_roundtrip 与两个纯内层断言均纳入
full compile DB。此时六入口源尚待统一集成，不把 reference 全构建通过称为 P02 完成。

### P01 完整往返与 P02 纯应用图

`cmake/platform.cmake` 将六个纯 application 库、各内层头独立翻译单元、配置校验驱动
接入 full。纯库不链接协议/JSON/TLS/数据库/reference；配置校验属于外层并单独链接。
CMake 输出 `architecture-targets.json`，从实际 LINK_LIBRARIES 和 INTERFACE_LINK_LIBRARIES
递归验证依赖，包含 PRIVATE static link 的 LINK_ONLY 表达式。门禁自测覆盖间接协议依赖
和间接 reference 链接，不以仅 grep 目标名替代配置图。

full 由 SDK 锁定解释器生成全部 Python 协议，`p01_target_roundtrip` 使用 CMake 生成的
fixture 执行 77 RPC/269 消息验证。fixture 以 WHOLE_ARCHIVE 链接生成协议库，确保动态
descriptor 注册对象不被静态链接器丢弃；仅构建成功不代表往返通过。

```bash
ctest --preset full -R '^p01_target_roundtrip$' --output-on-failure
ctest --preset full -L '^p02$' --output-on-failure
python3 scripts/quality/check-architecture.py --links build/full/architecture-targets.json
```

P02 纯应用/配置目前已有三项 unit 标签测试。六入口和 runner 的完整构建仍需各自已审查
提交接线；完整链接门禁默认要求六个 EXECUTABLE，不会因为只有纯库就通过。

### P02 六入口与黑盒接线已验证

六个可执行文件统一输出到 `build/<preset>/bin/kiarabot-{control,identity,runtime,plugin-host,scheduler,model}`。
Host 实际入口为 `core/app/host_main.cpp`，独立库为 `kiarabot_plugin_host_application`。
`kiarabot_service_process`/`kiarabot_service_config` 是外层支撑，只有它们链接生成协议、
JSON/TLS；六个 application 的实际配置图与独立头编译仍保持纯 C++。

测试输入必须明确提供：

```bash
cmake --preset full \
  -DKIARABOT_TARGET_PYTHON="$PWD/sdk/python/.venv/bin/python" \
  -DKIARABOT_TEST_OPENSSL="$PWD/build/openssl-3.5.0-install/bin/openssl"
cmake --build --preset full
ctest --preset full -L '^p02$' --no-tests=error --output-on-failure
python3 scripts/quality/check-architecture.py --inner --links build/full/architecture-targets.json
```

Python 必须先用固定 uv0.13.6 同步3.11.18 SDK环境（TLS黑盒同时验证grpcio1.85.0）。
测试CA CLI本机已从固定OpenSSL3.5.0源码/hash构建，需执行 `make install_sw install_ssldirs`，
缺标准 `openssl.cnf` 会使证书生成明确失败。仅执行 `openssl version` 不证明CA工具能签证书。
CMake p02标签共11项，包括真实六服务lifecycle/TLS/并行故障、Core进程集成及夹具/应用
单测；本机以固定Python/CA输入已实际全部通过。C++ runtime仍使用本机Homebrew依赖，
此证据与完整full-locked/PG环境分开；ready=false/UNIMPLEMENTED不等于后续业务已实现。

P02 reference 补充：HTTP/WS CTest 的 pytest 路径规范化为实际 `control/api/tests`，
保留已存在 venv 的真实套件；其27项用例已在固定依赖构建上实际通过。
`architecture_p02_reference_manifest_test` 将清单的6个失败/路径回归自测注册为p02/unit。
独立头翻译单元改用file(GENERATE)，重复configure不再无条件改写文件触发全量重编译。

## P00 固定依赖的实际环境验证（本机 macOS arm64）

固定registry的13项直接依赖及全部传递port已真实安装在忽略的
`build/architecture-installed/arm64-osx`；`--registry`和`--installed --triplet arm64-osx`
来源/版本检查通过。新增`build-tools.lock.json`固定实际取得的GNU automake1.17、
GNU libtool2.5.4及官方归档SHA-256；它们解决libsodium构建缺aclocal的失败，不替换系统工具。
OpenSSL CLI固定3.5.0的官方源码SHA-512与库锁一起记录，独立构建共享库与CLI；
GNU/PG/OpenSSL/Node下载归档均可用`check-dependency-lock.py --archive FILE`流式验证。

```bash
python3 scripts/quality/check-dependency-lock.py --registry build/vcpkg \
  --installed build/architecture-installed --triplet arm64-osx
VCPKG_ROOT="$PWD/build/vcpkg" cmake --preset full-locked \
  -DKIARABOT_TEST_OPENSSL="$PWD/build/openssl-3.5.0-install/bin/openssl" \
  -DKIARABOT_TEST_PG_BIN="$PWD/build/pg-16.4-install/bin"
cmake --build --preset full-locked --parallel 4
ctest --preset full-locked --output-on-failure
```

full/full-locked包含依赖smoke手写TU，不能从编译覆盖中排除。full-locked的Python3解释器
明确使用SDK锁定环境；Protobuf/gRPC生成器优先使用对应CMake imported executable，
防止PATH混入系统protoc。首次完整固定构建与51项CTest均已实测通过，含27个Control
HTTP/WS用例。仅macOS本机证据，不是Linux/CI/发布镜像验收。

`p00_dependency_smoke`只创建自己的临时PG16.4集群，SCRAM密码、TLS verify-full与
channel_binding=require，测试libpqxx7.10.0和JWT Ed25519签名/验签/错误audience拒绝；
固定版本宏与实际runtime版本均校验，结束停止集群并删除临时私钥。没有外部DSN参数，
不创建业务schema/角色/UoW，不代表P03 Repository与COMMIT故障矩阵通过。
TLS使用EC证书，JWT使用独立Ed25519密钥：PG16.4对Ed25519 TLS证书的通道绑定摘要
不支持已实际检出，未通过关闭绑定/TLS来修复。PG服务端须链接同版共享OpenSSL3.5.0；
静态嵌入crypto导致上游libpq exit符号检查失败，保留检查并换共享库后构建通过。

GNU工具从锁中官方URL下载并校验后解压于build/，分别configure到`build/gnu-tools`；
automake使用`--prefix`，libtool另加`--program-prefix=g`（macOS提供glibtoolize）。
重新安装port时将该bin加入PATH。OpenSSL从锁中官方归档构建`shared no-tests no-docs`，
执行`make install_sw install_ssldirs`；PG configure设置同版共享库的CPPFLAGS/LDFLAGS、
`--with-openssl --without-readline --without-icu --without-zlib`，再make/install到上述PG路径。
构建前置仍需系统C/C++编译器、CMake、make、Perl、autoconf、m4；镜像级OS/编译器锁留P14。

Python3.11.18已经由固定uv0.13.6实际安装并通过SDK/黑盒测试，Node22.14.0官方归档校验
通过。Python两平台的精确20261026下载URL来自固定uv内嵌目录，已记录在锁中；其独立
归档SHA-256尚未取得（直接官方镜像HTTP403），明确记null，不能称完整二进制镜像锁。
uv本身则已纳入quality requirements的精确版本/归档hash。不得用安装目录hash冒充归档hash。

质量工具有效锁更新为format16.0.6、tidy22.1.8、uv0.13.6。tidy19.1.0在新版libc++/Boost
调用链报告unique_ptr泄漏，同一个http_server.cpp、编译数据库、SDK参数及全部
clang-analyzer检查用22.1.8实测通过；没有改源码、关闭NewDeleteLeaks或降低warnings-as-errors。
全仓22.1.8静态分析结果须以执行报告为准，不以单文件通过替代全仓。

### 最终验证入口与范围

`KIARABOT_CHECK_PRESET=full-locked ./scripts/check-toolchain.sh`在最低版本检查之外，要求
Python3.11.18、Node22.14.0、format16.0.6、tidy22.1.8、protoc29.3；工具路径由PATH明确提供。
它执行dev/full-locked、实际图/编译覆盖/内层/p02标签、固定standalone infrastructure、
契约登记/descriptor/既有Python reader、格式/tidy和API/Ajv门禁。缺任何前置返回非零。
OpenAPI/AsyncAPI脚本现在只复用`scripts/ci/package-lock.json`的本地安装，并验证工具版本；
先执行`npm ci --prefix scripts/ci`，检查阶段不再临时解析未锁的传递依赖或重复下载。

header-filter由所有非忽略手写头生成，仍枚举全部手写TU（含reference和P00 smoke）；
下载到build/的第三方头不进入“仓库手写头”清单，但调用链的analyzer诊断仍会失败。
未通过过滤或NOLINT压制报告：tidy22的全仓首轮检出了smoke默认picojson后端的释放诊断，
遂明确选择jwt-cpp官方支持的nlohmann-json traits（同样固定3.11.3），重验Ed25519/PG
断言和分析；生产Identity后续也应对选定traits做完整claims/错误测试。

统一入口已实跑到原contract_status证据路径失败并非全绿；旧登记定义“implemented=生产
已接线”，reference机械移动后需由主对话协调状态口径，不能自动用reference冒充新业务。
Core所有者的format16两处纯格式修正已在主分支审查验证后同步（本分支52d7fa3），本机全仓format16检查通过；没有自行编辑业务实现。
完整静态分析和后续恢复结果见模块执行记录，不以51项CTest通过代替这些独立门禁。

## P00 Python发行摘要来源收尾（P03工具链准备）

此前HTTP403限制保留，但已从固定uv0.13.6原生二进制中提取其实际使用的压缩下载清单，
取得Python3.11.18 macOS arm64/Linux x86_64 glibc预期归档SHA-256，填入依赖锁。
锁同时记录uv二进制、解压清单的digest与metadata_key；原生文件与固定PyPI包RECORD
已实测匹配。并非猜测hash，也不是安装目录hash。

```bash
python3 scripts/quality/verify-python-metadata.py --uv .tools/quality/bin/uv --zstd-library /path/to/libzstd
python3 scripts/quality/tests/python_metadata_test.py
python3 scripts/quality/check-dependency-lock.py
# 取得归档后再执行真正的归档体校验，不能用上面的元数据校验替代：
python3 scripts/quality/check-dependency-lock.py --archive /path/to/cpython-3.11.18+20261026-PLATFORM-install_only_stripped.tar.gz
```

提取复验绑定已记录darwin-arm64 uv原生文件，解压库路径显式提供，不向系统安装工具；
Linux归档的摘要同样来自该完整跨平台清单。本轮没有取得独立归档体，HTTP403不记成功，
也不宣称Linux运行/CI/镜像digest已验收。执行证据见
[本轮P03工具链记录](../reports/modules/architecture-p03-toolchain.md)。

## P03 工具链准备：实际依赖上下文

锁定统一入口不再假定当前source tree的`build/architecture-installed`。它从选定
`build/<preset>/CMakeCache.txt`读取实际`VCPKG_INSTALLED_DIR`、内部归一化值、triplet和
CMAKE_TOOLCHAIN_FILE，来源检查与standalone基础设施构建使用同一上下文。缺项、相对路径、
重复/相互矛盾的路径或非法triplet直接失败；不回退系统库，不从未知环境变量猜路径。
路径含空格或等号保持为单一参数，显式传递triplet，支持已验证prefix被其他worktree复用。

```bash
python3 scripts/quality/cmake-check-context.py --build-dir build/full-locked
python3 scripts/quality/tests/cmake_check_context_test.py
```

独立基础设施构建缓存放在选定build目录内，以安装目录/toolchain/triplet上下文指纹区分，
防止换prefix后仍沿用原find_package缓存。该指纹只隔离本机构建缓存，不是依赖归档digest。
所有源码、编译/测试要求保持；未改变PG fixture、端口或业务接口。

## P03 构建接线

根 proto/full/full-locked 构建增加外层 `kiarabot_postgres`（固定 libpqxx7.10.0），
显式初始化 CLI `kiarabot-pg-initialize` 和真实 PG driver。PG 不链接六个纯应用库，
不会改变生产 daemon 的 ready 判定。Core 内层新增头继续自动独立编译。
`p03_persistence_contract` 标记 p03/unit；`p03_postgres_database` 标记
p03/integration/fault，120秒有界超时，使用已有显式 PG/openssl/Python cache 参数。
fixture 单进程拥有临时集群的创建、故障和安全清理，独立测试不共享集群，因而无需跨
CTest 的 setup/cleanup/resource lock；它不接收外部 DSN，缺少工具明确失败。
这替代上文尚未实施的拆分 fixture 规格，不代表 Repository/UoW 或 P03 全阶段完成。

```bash
cmake --build --preset full-locked --parallel 4
ctest --preset full-locked -L '^p03$' --no-tests=error --output-on-failure
```

实际命令和未验项见 [P03构建报告](../reports/modules/architecture-p03-build.md)。
