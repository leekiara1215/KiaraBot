# 开发交接与换机恢复

记录日期：2026-11-01。盘点基点为主集成分支 `codex/architecture-rollout` 的
`8e9974a9de943a20c03895f7b1b91617b092cca5`。第二次核对主集成已到
`97976f7a8542a625ee1e0bc1a276b0e5b4435363`，下表更新了已审取映射；主协调仍在
收尾构建，迁移前须用最终干净提交更新本页。原分支 SHA 与 cherry-pick 后的集成 SHA 不同，
不能仅凭某个原 SHA 不是祖先就认定能力没合入。

## 先做什么

用户最新优先级是单机最小「收消息 → 处理 → 回复」闭环。今晚复用隔离的 reference
单机流作为临时交付轨；这不表示新 Runtime、P05–P14 或新架构消息闭环已经完成。
插件机制、完整账号管理、多主机部署及其他非核心管理功能后移，不继续开启 P04 扩展。
MVP 使用固定 SDK handler，不启动 PG 或六服务；使用独立本地 data，不双写、不导入旧库。
最新 MVP 目标、启动入口和验收顺序由主协调维护的 `doc/24-*.md` 定稿；本次补充时该文档
仍待父验证定稿，不能据此猜测启动器名称或参数。

先阅读 [AGENTS.md](../AGENTS.md)、[C++ 主规范](12-cpp-coding-standard.md)、
[边界](00-boundaries.md)、[工具链](11-toolchain.md) 和本页；原阶段依赖仍见
[分步计划](23-implementation-plan.md)。[doc/18](18-architecture-baseline.md) 为人类维护的
受保护只读基线，不能以换机、同步或收缩 MVP 为由修改。

## 已完成到哪里

| 范围 | 已证实内容 | 尚不能宣称 |
| --- | --- | --- |
| P00–P03 | 计划记录本机固定环境门禁通过；依赖锁、目标契约、六进程构建/生命周期/分层图、四类私有 PG 库及 UoW/收据/交接基础已经本地验证 | 其他电脑、Linux/CI/发布镜像已通过；完整业务机器人已完成 |
| P04 部分 | 身份/会话规则、PG 与 crypto、真实当前 mTLS 认证、Issue/Exchange、公开验证键及部分 Control 认证入口有独立实现和测试证据；纯在线授权及实际 TLS/JWT/PG 集成已有提交 | 整个 P04 已完成；全部凭据/赋权/失效流/管理 RPC 均生产可用 |
| P05–P14 | 有计划、协议与部分基础类型；P05–P14 新服务业务未实现 | 新 Runtime 账号/插件、目标投递/Action、Host、存储、Scheduler、Model、完整 Panel 管理或多机恢复已实现 |
| reference 临时 MVP | 保留旧单机行为及对应验收；入口源位于 `core/reference/`，二进制仍叫 `kiarabot-core` | 将旧入口改名即可完成新服务；reference 通过等于目标架构通过 |

证据入口：[P02 验收](../reports/modules/architecture-p02-acceptance.md)、
[P03 构建](../reports/modules/architecture-p03-build.md)、
[P03 服务数据库装配](../reports/modules/architecture-p03-service-database-composition.md)、
[P04 Identity](../reports/modules/architecture-p04-identity.md)、
[P04 生产公开键](../reports/modules/architecture-p04-production-verification-keys.md)。
各报告的基点、fixture、实际命令和未运行项只证明其标注范围；早期报告中的「待实现」
可能被后续独立提交补齐，应结合提交和当前接口核对，不能直接拼成阶段完成结论。

主集成基点最近的可定位证据包括：`da3b2bb` 生产公开键真实 mTLS 门禁，
`d0c9c09` 纯在线授权，`deec7c5` 最终时钟跨期限拒绝，`0fa4148` 实际 TLS/JWT/PG 授权集成，
`bf2fb4e` 原 Control 当前权威，`b3aea5c` 各 actor 显式委托能力，`9938f7a` 共用原 Control
权威 helper，`03c3974` Exchange RPC，`6fe4b4d` 新 crypto 端口严格替身。
这些是局部证据，不提升 proto/API 的生产实现状态。

## 分支与尚待审取的能力

以下为本次只读盘点的 owner 分支与原提交。表中「待审取」不是要求今晚合并；
先保存成果，是否纳入 MVP 由主协调决定。不要整条旧分支盲目 merge 或重复 cherry-pick。

| owner / 分支 | 准确原 SHA | 状态与交接内容 |
| --- | --- | --- |
| 主协调 `codex/architecture-rollout` | `97976f7` | 二次核对已集成 PG state 与完整收据 codec；仍有少量 CMake/严格替身组装待提交，未启用注册/赋权生产路由 |
| RPC `codex/p04-authorization-rpc` | `9bfadca`、`2d1170a`；分支头 `2d1170aa336eca4fec9a15ee198b8faf729f9d98` | CheckAuthorization 可选 `.authorization`、500ms/1s 和 decision/Failure 映射；后者仅给 Issue/Exchange fixture 补原 Control binding；基点尚未审取这两项 |
| RPC `codex/p04-exchange-rpc` | `7714ed25ee3ef35bfc0b6de84a7ff1f3328c9f4c` | 已由主集成 `03c3974` 承接；严格新 crypto fixture 原 `958aa5e` 对应集成 `6fe4b4d`，勿重复取 |
| Core `codex/p03-core-ports` | `15fd1195cd6540150dae9d1f04cae15b38c08565` | 完整原注册成功 snapshot 已由 `4d7976f` 审取；凭据 WIP 已另存 `codex/p04-identity-wip` 的 `938c9d2`，未纳入主集成/MVP |
| Core 冻结 `codex/p04-identity-wip` | `938c9d2` | 从 `15fd119` 保存14文件的未完成凭据能力；工作树已 clean；Register/Rotate 仍仅声明，Pending/delivery 的 PG/crypto 未接，不启用生产/MVP |
| PG `codex/p04-complete-result-codec` | `0428f0b6443967c4e31f643625055fb8045cfc6a` | 已由 `97976f7` 审取；配套完整注册 snapshot 为 `4d7976f`，ExactAssignment 收据 index14 为 `fae5772`，仍未启用相应生产路由 |
| PG `codex/p04-assignment-state-pg` | `eacf42d` | 永久撤销墓碑已由 `a030394` 审取；后续统一构建结果由主协调补充，不再列为未合入 |
| Core `codex/p04-assignment-result` | `71acedd96a18c5401774df1226c50e7ad7640ee8` | 明确标记 WIP 的 Assignment mutation app/tests，已提交保存但仍有未解 tidy 诊断；非完整可用 factory，不启用路由；相关公开头 `68d3b06` 仍须独立核对 |
| Control `codex/p04-authorization-method-policy` | `a79ad51` | 此分支头是 current delegator 检查修复，不是生产 method-policy 完成证据；主集成已有对应修复，scope annotation/resolver 尚未提交完成 |
| crypto `codex/p04-assignment-fingerprint-crypto` | `5f8ee3c8261ea578c21c24373e59bde2fd94636a` | typed fingerprint 已由主集成 `575a514` 承接，不需 RPC 自己哈希或让客户端提交摘要 |
| 工具链 `codex/p03-toolchain` | `dc6677bba7a7ab6b0c593a784abed23b05d5961a` | 已提交的固定 Python 来源摘要修复；另有未提交检查入口 WIP，不能把该头当这些 WIP 已保存 |
| MVP 启动 `codex/local-mvp-launcher` | 观察时 `da3b2bb` | 该 SHA 是既有 Identity build 门禁，不是新启动器提交；等实际独立 cap 再更新 |
| MVP smoke `codex/mvp-nonebot-smoke` | `67f91e562e6d54e2230017b9ff898ec079df55dd` | 已提交真实 NoneBot/reference Core/固定测试 Sender 与 OneBot 本地替身闭环；作者反馈相关3/3通过；公开 Sender/launcher 尚待独立提交 |
| MVP Core / 平台 `codex/mvp-local-reply`、`codex/mvp-onebot-demo` | 盘点期间刚切换的新分支 | 新窄任务尚无本页可确认的独立 cap，不从工作树内容推断完成 |
| Panel / SDK 保留分支 | `kiarabot-control-panel`：`56ca38b`；`kiarabot-plugin-sdk`：`c386493` | 旧行为成果保留；不是 P12 或新插件机制的完成证据，今晚不扩展 |

可重新核对：`git worktree list --porcelain`、`git branch -vv`、
`git log --oneline <基点>..<owner分支>`。`git cherry <基点> <owner分支>` 只能辅助比较补丁；
冲突修正后的 cherry-pick 可能 patch-id 不同，最终仍需按文件和原/集成映射确认。

## 未提交内容：换机前必须另行保全

本节只记录 `git status --short` 的路径，没有读取或把未提交源码当成实现证据。
状态会变化，迁移前由各 owner 再确认；不能因为另一 worktree 干净就断言全仓都已保存。

| 工作分支 / 观察状态 | 路径与处理 |
| --- | --- |
| Core 凭据 WIP 已冻结 | 原 tracked/untracked 并集14文件已保存到独立 `codex/p04-identity-wip` / `938c9d2`，冻结前基点 `15fd119`，工作树 clean。精确清单用 `git show --name-only 938c9d2` 及 `git show 938c9d2:reports/modules/p04-wip-freeze.md` 查看；不再列作未提交内容，不把仅声明的 Register/Rotate 或未接 PG/crypto 认作可用能力 |
| `codex/p03-toolchain`，tracked 修改 | `scripts/check-toolchain.sh`、`doc/11-toolchain.md`、`reports/modules/architecture-p03-toolchain.md` |
| 同分支，untracked | `scripts/quality/cmake-check-context.py`、`scripts/quality/tests/cmake_check_context_test.py`；这是该 owner 的未提交差异，不据此断言主基点没有同名已提交文件 |
| `codex/mvp-nonebot-smoke` | 初次观察 smoke 为 untracked；二次已提交为 `67f91e5` 且工作树干净，不再列未保存 WIP |
| 主集成临时冲突态 | 观察时 `doc/05-infrastructure.md`、`doc/08-operations.md` 为 UU，并有 `infrastructure/postgres/` staged 改动及 Assignment state PG 报告；该 cherry-pick 在二次核对时已完成至 `97976f7`；当前剩余 `cmake/platform.cmake`、`core/CMakeLists.txt` 和两个 Core architecture test 的组装/严格替身修改待主协调验证提交，不复制未提交工作目录 |

优先让 owner 对明确代码/文档范围做独立 WIP commit 并说明未验证项，再做 git 迁移；
不要 `git add .`，不要把 `.env`、数据库、密钥、build 或虚拟环境带入提交。
如果必须保留未提交状态，应分别安全备份 staged/unstaged patch 和 untracked 文件，
记录所属分支/基点；普通 `git bundle` 不包含它们，`git diff` 也不包含 untracked 内容。

## 未完任务与恢复时的验收建议

| 优先级 / 目标 | 文件范围 | 前置 | 建议退出条件 |
| --- | --- | --- | --- |
| 今晚：单机启动、登录和最小配置 | MVP launcher 的 `scripts/`、示例及直接运行说明；reference 组合入口按 owner 分工 | 主协调定稿 doc24、干净集成点、锁定工具链 | 新空目录创建本地配置；启动/停止可复现；能登录；错误配置明确失败，不暴露秘密 |
| 今晚：一账号收消息并自动回复 | `core/reference/` 最小回复处理；`adapters/nonebot/` 与本地 OneBot 协议替身 | 已有 AdapterBridge/Action 路径；唯一最小处理方案由 MVP owner 决定 | 启动真实本地进程，替身送一条事件并收到预期回复，记录退出/拒绝，不能只测函数或仅接收事件 |
| 今晚：可复现 smoke 与启动步骤 | `adapters/nonebot/tests/`、验收脚本、doc24/本页 | 前两项独立 cap 合并 | 新建 venv/build 后可重复执行；失败非零；只有 loopback 替身，无真实 QQ/付费模型 |
| 待恢复：在线授权生产接线 | `core/runtime/platform/`、`control/api/` 组合、`proto/policy.proto` 的授权 scope 元数据及验证，由各 owner 分工 | Core 已冻结 API；RPC `9bfadca`；Control 单一 metadata 来源 | 未 annotation 方法 fail closed；不从客户端构造 permission、不复制方法表；真实 TLS/JWT/PG 测试保持 |
| 后移：P04 凭据生命周期/登记 | 冻结 `938c9d2` 的14文件与直接文档；再按已冻结端口协调 crypto/PG/RPC | 先同步主集成新端口/共享 helper，核定当前契约/错误/收据；不把草稿当公共冻结接口 | 正常/拒绝/过期/撤销/依赖故障、原收据恢复和真实组合分别通过 |
| 后移：P04 完整 Assignment 管理 | `identity_assignment_application.*`、typed receipt、PG state/codec 及将来的 RPC | state/fingerprint/完整结果契约依次审取；factory 完整实现 | 不复活墓碑、旧/新 scope 当前授权、幂等返回原 typed snapshot；实际 PG 与 RPC 测试，不启用 WIP |
| 后移：完整 P04 失效流及管理面 | Identity/Control 对应模块与唯一契约 | 确认尚未完成的逐方法清单与当前权威 | 当前权威失败关闭、快照/增量水位和恢复验收；不能靠局部 crypto 测试关闭阶段 |
| 后移：P05–P14 新服务业务 | 按 doc23 的各模块独立范围，先协议/端口后实现 | P04 阶段退出及各阶段依赖；用户重新排定优先级 | 每阶段真实行为/错误/故障矩阵分别通过；插件机制、完整账号管理、多机部署不加入今晚 MVP |

## 换机：先迁移 git，再重建环境

截至本快照，新增成果只在本地 git，未执行 push；不能假定远端已有这些分支。
单独复制某个 worktree 工作目录不能保全共享 git 对象、分支和其他 worktree 的 WIP。
本页不执行 push、部署、清理 worktree 或搬运秘密。

1. 在原机逐 worktree 检查 `git status --short`，完成上节 WIP 保全；记录最终集成 SHA、
   `git branch -vv` 与原/集成映射。不要在未完成 cherry-pick/冲突的工作树创建交接基线。
2. 在任一正常 worktree 创建包含所有本地 refs 的 bundle，路径由使用者设置，勿放入仓库提交：

   ```bash
   : "${KIARABOT_GIT_BUNDLE:?设置仓库外的安全 bundle 路径}"
   git bundle create "$KIARABOT_GIT_BUNDLE" --all
   git bundle verify "$KIARABOT_GIT_BUNDLE"
   ```

   bundle 是源码/提交备份，不是数据库、密钥、ignored 文件或未提交 WIP 的备份。
   本地业务数据和秘密另走安全备份；reference SQLite 的数据库/加密密钥必须配套保存，
   Adapter outbox 与 `.actions` 也需按 [运维说明](08-operations.md) 成套保全，禁止入 git。
3. 将验证过的 bundle 与明确清单传到新机，按使用者选定目录 clone，核对精确提交后再建继续分支：

   ```bash
   : "${KIARABOT_GIT_BUNDLE:?设置新机收到的 bundle 路径}"
   git clone "$KIARABOT_GIT_BUNDLE" KiaraBot
   cd KiaraBot
   git branch -a
   : "${KIARABOT_HANDOFF_COMMIT:?设置交接时最终确认的干净提交 SHA}"
   git switch -c codex/continue-mvp "$KIARABOT_HANDOFF_COMMIT"
   git status --short
   ```

   clone bundle 后的 origin 指向 bundle，并不证明已连接原远端。远端地址由用户核定；
   不自动 push，也不从旧机 shell/CLI 配置推导认证。
4. 重新安装 [doc11](11-toolchain.md) 与 `dependencies/architecture/` 锁定的工具和依赖。
   不复制旧 `build/`、`.venv/`、`.tools/`、CMakeCache 或生成源码；不引用旧机绝对路径。
   新机编译器、SDK、PATH、PG/OpenSSL CLI 都须重新确认，macOS 本地证据不自动覆盖 Linux。

## MVP 最低验收与后续完整环境恢复

先核对主集成已提交的 MVP 源码：`762d6e1` 固定 SDK handler（`plugins/local-reply/`），
`407dd8c` 本地 OneBot demo（`scripts/local/onebot_demo.py`），`c37df48` smoke 与
`994a8b4` 公开 factory 的端到端 smoke。它们不表示 launcher 正常流程已验收。
父已提交的 `ecf3cdc` 是 schema10/typedResult 组的构建冻结，不提升 P04 整阶段状态。

今晚最低验收只使用 reference 单机、固定 SDK handler 和 loopback 协议替身；不启动
PG/六服务。按父实际验证后的 doc24/launcher/README 重建其所需环境并执行；当前没有
最终已验证的 launcher CLI，不在本页猜 flags。以下完整锁定环境步骤是以后恢复新架构
开发的独立入口，尤其 PG server、六服务和全仓门禁不是今晚启动的先决条件。

以下从仓库根执行，是换机后的建议步骤，本次文档任务没有在新机执行。
使用锁文件指定来源/hash；首次下载与构建需要网络及本机工具，不能把旧机已装依赖当作前提。

```bash
# 先按 doc11 安装固定 uv、Node、format/tidy、编译器及系统构建前置。
uv python install 3.11.18
uv sync --project sdk/python --locked --python 3.11.18
uv sync --project adapters/nonebot --locked --python 3.11.18
uv sync --project control/api/tests --locked --python 3.11.18
npm ci --prefix scripts/ci

# 按 doc11 从官方来源取得并 checkout 固定 vcpkg registry，再 bootstrap。
export VCPKG_ROOT="$PWD/build/vcpkg"
: "${KIARABOT_VCPKG_TRIPLET:?按新机平台选择，例如 arm64-osx 或 x64-linux}"
python3 scripts/quality/check-dependency-lock.py --registry "$VCPKG_ROOT"
"$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
"$VCPKG_ROOT/vcpkg" install --x-manifest-root=dependencies/architecture \
  --triplet="$KIARABOT_VCPKG_TRIPLET" --x-install-root=build/architecture-installed
```

后续完整目标架构开发才需要的 PG16.4 服务端与 OpenSSL3.5.0 CLI 按 doc11 的已校验归档独立构建；libpq/vcpkg 安装不等于
已经有 PG server。将新机实际安装位置显式放入环境变量，再配置全新构建：

```bash
: "${KIARABOT_TEST_PG_BIN:?设置新机固定 PG16.4 bin 目录}"
: "${KIARABOT_TEST_OPENSSL:?设置新机固定 OpenSSL3.5.0 CLI 路径}"
cmake --preset full-locked \
  -DVCPKG_TARGET_TRIPLET="$KIARABOT_VCPKG_TRIPLET" \
  -DKIARABOT_TARGET_PYTHON="$PWD/sdk/python/.venv/bin/python" \
  -DKIARABOT_TEST_PG_BIN="$KIARABOT_TEST_PG_BIN" \
  -DKIARABOT_TEST_OPENSSL="$KIARABOT_TEST_OPENSSL"
cmake --build --preset full-locked
sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py
adapters/nonebot/.venv/bin/python adapters/nonebot/tools/generate_proto.py proto
KIARABOT_TARGET_PYTHON="$PWD/sdk/python/.venv/bin/python" proto/tests/check_target.sh
```

执行前须显式确认 `protoc`、`grpc_cpp_plugin` 和 `pkg-config` 指向新机固定依赖，
例如检查 `protoc --version` 与 `pkg-config --modversion protobuf grpc++`；不要沿用旧机
PATH 或拿系统另一版生成器混用。

C++ wire 由 CMake 生成；Python wire 由各自匹配的生成器/runtime 生成，禁止复制旧 `.pb.*`
或关闭生成器版本检查。proto 当前唯一来源是 `proto/`，不是旧工作树缓存。

在主协调审取 `67f91e5` 后，可先运行它明确提供的窄闭环；这是测试入口，固定 Sender
目前内置于测试中，不是公开用户启动器：

```bash
CORE_TEST_BINARY="$PWD/build/full-locked/reference/core/kiarabot-core" \
  SDK_SOURCE="$PWD/sdk/python/src" \
  adapters/nonebot/.venv/bin/python -m pytest -q adapters/nonebot/tests/test_mvp_local_smoke.py
```

reference 基线与完整阶段门禁分开执行，缺失二进制/依赖应失败而非跳过：

```bash
ctest --preset full-locked -L '^reference$' --no-tests=error --output-on-failure
KIARABOT_ACCEPTANCE_BUILD="$PWD/build/full-locked" \
  KIARABOT_ADAPTER_DIR="$PWD/adapters/nonebot" bash tests/acceptance/run.sh
# 恢复目标架构开发时再运行完整锁定门禁；这不是今晚 MVP 的阶段扩张要求。
KIARABOT_CHECK_PRESET=full-locked ./scripts/check-toolchain.sh
```

最后按已提交的 doc24/启动器完成真正的最低启动验收：新独立 data/本地配置启动、登录、使用一个
测试账号、本地协议替身注入一条消息、收到恰当且可核对的自动回复、正常停止，并记下精确
提交/命令/退出码。缺任何一项就不能叫 MVP 完成。二次核对已有 smoke `67f91e5`，但尚无已提交的公开新 MVP 启动器，
因此不能给出虚构「一键启动」命令。

真实 QQ/NapCat、用户账号和外部模型仍需用户端配置与另行授权；本轮只做本地协议替身，
不调用真实 QQ、付费模型或外部部署。换机后也不能自动继承旧机账号、CLI 凭据或本机数据。
