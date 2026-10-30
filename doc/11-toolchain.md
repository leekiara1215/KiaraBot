# 开发工具链与本地质量检查

T06c 时区转换使用 Protobuf/gRPC 已依赖的 Abseil CMake 包（absl::time）和系统 zoneinfo；Linux 验收机安装 tzdata。独立 infrastructure 构建也显式查找该包，Core 领域代码只依赖 ITimeZoneResolver。新增 task_scheduler CTest 和 SDK test_tasks.py 覆盖跨进程恢复。

T06b 基础设施构建新增 libcurl >= 7.85（CMake CURL::libcurl）；JSON 编解码复用已固定的 Protobuf，不引入模型厂商 SDK。macOS 本轮验收使用系统 libcurl 8.7.1。协议注释/功能协商名修改仍需运行生成、descriptor 和契约测试；Provider 验收只使用本机 HTTP 假服务，不调用外部模型。

T03b Python SDK 固定 Python 3.11.18，依赖以 sdk/python/uv.lock 为准；从唯一 proto 源生成到 sdk/python/build/generated。运行 `uv sync --project sdk/python --locked`、`sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py` 和 `sdk/python/.venv/bin/python -m pytest -q sdk/python/tests`。真实进程测试要求先构建启用基础设施的 proto Core，不跳过缺少的二进制。Core 使用 POSIX posix_spawn 管理外部解释器，不链接 Python 库；当前仅支持 POSIX 进程管理。详细启动配置见 doc/08-operations.md。

本文件规定本仓库的工具版本和可重复的本地检查。不要把本机绝对路径写入仓库配置；通过 `PATH` 选择工具，使用 `CXX` 选择 C++ 编译器。

## 要求

| 工具 | 要求 | 检查方式 |
|---|---|---|
| Python | POSIX 进程测试需要 Python 3.9+，仅构建测试依赖 | CMake 查找解释器及 CTest 进程测试 |
| CMake | 3.24+ | 版本、dev/proto 配置和构建 |
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

## 统一检查

在仓库根目录运行：

```bash
./scripts/check-toolchain.sh
```

脚本遇到第一个错误即退出，缺少工具、版本不满足、生成失败或任何检查失败都返回非零状态。它依次检查工具版本、C++20 编译和运行、protoc/gRPC 实际生成、脚本语法、dev 和 proto 预设的配置/完整构建/CTest、协议契约检查、C++ 格式和静态分析、OpenAPI 与 AsyncAPI lint。proto 配置启用 `CMAKE_EXPORT_COMPILE_COMMANDS`，供 clang-tidy 使用。生成物只由构建写入忽略的 `build/` 或临时目录，不提交。

格式检查扫描仓库手写 C++ 头文件和源文件，排除 `build/`、`node_modules/` 和 `.pb.*` 生成文件。clang-tidy 使用 proto 预设的编译数据库检查 `core/`、`proto/tests/`、`control/api/` 与 `infrastructure/` 的 C++ 源文件及其仓库头文件，并将诊断警告视为错误；macOS 的 AppleClang 构建使用 `SDKROOT` 或 `xcrun` 查找 SDK。CMake 会缓存已配置的编译器，切换 `CXX` 时须重新配置构建目录。AsyncAPI parser 输出所有诊断，severity 0 的错误或没有解析结果时失败；信息和提示仍会显示。

可以单独运行下列检查以定位失败，运行 clang-tidy 前需先完成 proto 配置和构建：

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
cmake --preset proto -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build --preset proto
ctest --preset proto
./proto/tests/check_contracts.sh
./scripts/quality/check-format.sh
./scripts/quality/check-tidy.sh
python3 ./scripts/quality/check-boundaries.py
./scripts/quality/check-openapi.sh
./scripts/quality/check-asyncapi.sh
```

各脚本均在仓库根目录运行，也可从其他当前目录调用其路径。OpenAPI lint 使用 `npx --yes @redocly/cli@2.72.1 lint api/openapi.yaml`；AsyncAPI 脚本使用 `@asyncapi/parser@3.6.6`，不依赖全局安装的 parser。升级这两个版本时应同时更新脚本、本文和版本兼容要求，并重新运行统一检查。

## 升级规则

- C++ 标准固定为 C++20，不随单个编译器升级改变；升级编译器、CMake 或协议工具后运行统一检查。
- 协议变更还需要提供上一版本 descriptor，按 `proto/tests/README.md` 运行兼容性比较；统一检查不会在未提供基线时声称完成跨版本兼容性验证。
- Python 3.11+ 与 Python 协议生成属于适配器项目的检查（目标 `adapters/`，当前暂存于同级检出），不由本仓库统一脚本代替。

## 其他模块和 CI 的后续接入

本次没有要求 Core、协议、API 或其他业务模块修改公共接口；已修复的 Core 和协议测试改动仅涉及格式。以下事项在对应模块或流程落地时处理，不影响当前本地检查结果：

- **CI/CD**：CI 工作流位于 `.github/workflows/ci.yml`，安装本文件列出的工具和运行库，运行 `./scripts/check-toolchain.sh`，并将非零退出码作为失败。`doc/12-ci-cd.md` 描述完整流程方案。
- **协议发布和 CI/CD**：保存上一发布版本的 descriptor；协议变更的检查中设置 `KIARABOT_BASELINE_DESCRIPTOR`，按 `proto/tests/README.md` 执行兼容性比较。未提供基线时，本地统一检查只验证当前 descriptor 和契约，不证明与旧版本兼容。
- **新 C++ 模块**：手写 C++ 需通过统一格式检查。当前 clang-tidy 的编译数据库检查覆盖 `core/`、`proto/tests/`、`control/api/` 和 `infrastructure/`；引入其他 C++ 模块时同步扩展 `scripts/quality/check-tidy.sh` 和对应构建目标。
- **Python 适配器和 SDK 项目**：适配器目标位于 `adapters/`（当前暂存于同级检出），SDK 位于 `sdk/python/`；各自维护 Python 依赖、代码生成和测试，本仓库的统一 C++ 检查不覆盖其实现。

## SQLite 可选构建

T05b 新增 SQLite3 开发库依赖，仅在基础设施独立构建或根构建 `KIARABOT_ENABLE_INFRASTRUCTURE=ON` 时需要。本次 macOS 使用系统 SQLite 3.54.0；依赖版本不硬编码到 Core。发布镜像固定版本仍由 T11 执行。根 proto 集成验收：`cmake --preset proto -DKIARABOT_ENABLE_INFRASTRUCTURE=ON && cmake --build --preset proto && ctest --preset proto`。

## T02a 请求指纹

gRPC Core 构建显式链接 OpenSSL Crypto 3.0+，使用 EVP SHA-256 计算适配器请求指纹，不自行实现密码算法。沿用本机/构建环境提供的 OpenSSL；发布镜像的精确依赖锁定仍在 T11b 完成，不能把本机验收当成发布可复现性验收。

## T08b–T11a 控制服务工具链

新增 nlohmann/json **3.11.3** 单头文件，由 CMake 下载至忽略的构建目录并校验 SHA-256 `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`；不提交第三方源码。HTTP/WebSocket 使用 Boost.Beast/Asio，密码与加密使用 libsodium。CI 沿用固定 vcpkg registry `ce613c41372b23b1f51333815feb3edd87ef8a8b`：boost-beast 1.87.0#0、boost-asio 1.87.0#1、libsodium 1.0.20#3。本机验收为 Homebrew Boost 1.92.0_1、libsodium 1.0.22；版本记录不同，不声称已在 CI 固定环境执行。CMake 要求 Boost 1.86+，不得把本机路径写入仓库。

控制验收环境：`uv sync --project control/api/tests --locked --python 3.11.18`，固定 pytest 8.3.5、jsonschema 4.23.0、PyYAML 6.0.2、websockets 15.0.1 与 uv.lock 全部传递依赖；网络测试只连接本机临时进程。执行 `control/api/tests/.venv/bin/python -m pytest -q control/api/tests`。环境存在且启用基础设施时注册到 CTest；统一 tests/acceptance/run.sh 始终要求此环境，缺失会失败。面板 `npm test` 也使用该 C++ fixture，需先构建 proto + infrastructure；Node 22.12+ 支持原生 WebSocket。

格式脚本通过 rg 尊重 .gitignore，排除本机构建树和第三方 `.venv`、`.tools`、`.ci-vcpkg`，静态分析扩展到 control/api 与 infrastructure 手写 C++。本轮未安装 clang-tidy 时仍由统一检查明确失败，不能把格式/编译替代静态分析。
