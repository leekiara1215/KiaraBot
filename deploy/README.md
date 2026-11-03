# KiaraBot 部署目录

本目录同时存在两代部署物：

- **reference（历史，仍可构建）**：根 `Dockerfile`、`deploy/compose.prod.yml`、
  `deploy/compose.staging.yml`、`deploy/env.example`、`deploy/control/`、`deploy/systemd/`。
  它们是单 Core + SQLite 的 reference 交付，**不是** P14 目标六服务部署。
- **target（P14a，新增）**：`deploy/docker/suite.Dockerfile`、`deploy/compose.{infra,core,exec}.yml`、
  `deploy/target.env.example`、`deploy/target/README.md`、`scripts/deploy/target-{smoke,initialize}.sh`。
  目标拓扑、端口、健康、秘密与未验证边界见 [doc/08-operations.md](../doc/08-operations.md)
  的“P14a 目标部署面”章节。

## 重要声明（必须阅读）

1. **本机没有 Docker**：P14a 只完成文件/结构/脚本实现；镜像构建、`docker compose config`、
   Compose 启动、healthcheck、初始化与两主机运行**均未执行、未验证**。
2. **默认拓扑已确认：单主机 Docker Compose**（PG + 控制面 + 执行面同机，各服务独立
   容器/TLS/凭据/DB 权限边界）。两主机是可选拓扑，跨主机断网/重连演练**延期并单独记录**；
   见下方 P14a-2 段与 doc/08。
3. **不自动初始化/迁移**：`compose.infra.yml` 的 `init-*` 与 `identity-bootstrap` 是显式
   profile/一次性命令；daemon 不执行 DDL、不清库、不导入旧快照。空库初始化与身份 bootstrap
   必须按顺序人工执行并逐库检查（跨库非原子）。
4. **秘密/证书/配置不入镜像**：config/TLS/密码/root key/enrollment/bootstrap 由部署生成，
   通过只读挂载提供；`deploy/target/` 下除 README 外全部 gitignore。
5. **无签名/无镜像仓库**：默认镜像名是 `:dev` 本地标签；发布必须换成 digest 并走 P14b/P14d
   CI/发布清单。

## 快速检查（无 Docker 可运行）

```bash
./scripts/deploy/target-smoke.sh                # 结构/YAML/shell 检查，Docker 缺失时输出 UNVERIFIED
./scripts/deploy/target-smoke.sh --require-docker --build   # 有 Docker 时：compose config + 构建
```

`target-smoke.sh` 不会 `docker compose up`、不会初始化数据库、不会启动任何服务。

## 目标文件一览

| 文件 | 作用 |
| --- | --- |
| `docker/suite.Dockerfile` | 多阶段构建：P00 锁定 vcpkg/Python3.11.18 → 六服务 + pg-init + identity-bootstrap → 共享 runtime base → 六个薄镜像 + tools |
| `compose.infra.yml` | PostgreSQL 16.4（四私有库由 init profile 显式初始化）；TLS/数据/备份卷；init-identity/runtime/scheduler/model + identity-bootstrap |
| `compose.core.yml` | Identity/Runtime/Scheduler/Model/Control/Panel；只读 rootfs、非 root、资源限制、healthcheck、只读 config/TLS/secrets 挂载 |
| `compose.exec.yml` | Plugin Host + Adapter；`exec`（单机默认）/`host`/`adapter` profiles；Host SQLite/包/凭据卷；Adapter P06c `BRIDGE_*` 配置与 bootstrap secret；REST/WS 只走 Control |
| `compose.target.yml` | **默认单主机入口**：include infra/core/exec；单机用 `--profile exec`，init 用 `--profile init` |
| `compose.docker-secrets.yml` | 可选 Docker secrets 覆盖（按需；只读文件挂载仍是默认） |
| `secrets.manifest.example` | 秘密文件名/mode/消费者/轮换重启顺序模板（无真实值） |
| `postgres-bootstrap.sql.example` | 四库/双角色 DBA 一次性 bootstrap 模板（无密码，psql 变量传入） |
| `target.env.example` | 两代/主机拓扑变量模板（镜像、端口、material 路径）；不包含秘密 |
| `target/README.md` | 部署生成材料布局（config/tls/secrets/data/packages/backups） |
| `../scripts/deploy/target-initialize.sh` | 单个私有库的显式空库初始化（P03 CLI 包装；无清理/无自动迁移） |
| `../scripts/deploy/target-rotate-secret.sh` | 人工秘密轮换：manifest 校验、原子替换、0600、备份、按序重启提示；KMS/热轮换延期 |
| `../scripts/deploy/target-smoke.sh` | 本地结构/YAML/profile/release digest 检查 + 有 Docker 时的 target.yml compose config/构建入口 |

## 在 Docker 可用 Linux 主机上的执行顺序（示例，未验证）

```bash
cp deploy/target.env.example deploy/target.env && chmod 600 deploy/target.env
# 1) 生成 PG 角色/库、TLS/CA、密码文件、config、identity enrollment、adapter bootstrap
#    （布局见 deploy/target/README.md；由部署脚本/运维生成，不提交）
# 2) 启动 PostgreSQL（官方 postgres:16.4-bookworm；不自动建库）
docker compose --env-file deploy/target.env -f deploy/compose.target.yml up -d postgres
# 3) 逐库显式初始化（四个非原子，逐库检查；使用 init profile，绝不 up -d）
for svc in identity runtime scheduler model; do
  docker compose --env-file deploy/target.env -f deploy/compose.target.yml \
    --profile init run --rm "init-$svc"
 done
# 4) 离线身份 bootstrap（admin/enrollment）
docker compose --env-file deploy/target.env -f deploy/compose.target.yml \
  --profile init run --rm identity-bootstrap
# 5) 启动单机默认全栈（PG + 数据/控制面 + Host + Adapter）
docker compose --env-file deploy/target.env -f deploy/compose.target.yml --profile exec up -d
# 6) 两主机可选拓扑（延期/未验证）：Host-A 用 infra+core，Host-B 用 exec --profile host --profile adapter
docker compose --env-file deploy/target.env -f deploy/compose.exec.yml up -d
```

停止顺序按 doc/08 P14a 章节；P13 的 drain 语义尚未合入，当前顺序是临时口径。

## P14a-2 单主机默认、官方 PG 镜像与秘密轮换（用户决定对齐）

### 默认单主机 Compose

- `deploy/compose.target.yml` 是默认入口，include `compose.infra.yml`、`compose.core.yml`、
  `compose.exec.yml`；需要 Docker Compose >= 2.24。
- 单机启动：`docker compose --env-file deploy/target.env -f deploy/compose.target.yml --profile exec up -d`。
  执行面两个服务在 `exec` profile 下同时启动；`init` profile 一次性运行，绝不随 `up -d` 启动。
- 各服务保持独立容器、TLS 材料、凭据文件与 DB 角色/权限边界；PG 不发布到公网。
- 两主机：Host-A 用 `compose.infra.yml`+`compose.core.yml`；Host-B 用 `compose.exec.yml`
  `--profile host --profile adapter`（也可拆 profile）。**跨主机断网/重连演练延期并单独记录，未验证。**

### PostgreSQL 官方镜像策略

- 默认 `POSTGRES_IMAGE=postgres:16.4-bookworm`，复现 full-locked CI 的 P00 PG16.4 语义。
- 生产发布使用发布时 PG16 当前修订版并**固定 digest**，例如
  `POSTGRES_IMAGE=postgres:16.4-bookworm@sha256:<64 hex>`；`scripts/deploy/target-smoke.sh --release`
  强制校验（同时要求服务镜像/UBUNTU_BASE digest 与非 `dev` 版本）。
- full-locked CI 继续保留 P00 源码构建；本部署不重跑源码构建。
- 以官方镜像验证初始化/迁移/恢复的步骤：`target-smoke --release` → 启动 postgres →
  `--profile init run --rm init-*` → 迁移工具（显式）→ P14c `scripts/ops/target-drill.sh`
  是 P00 数据层演练；**本机无 Docker，官方镜像的 pull/初始化/迁移/恢复实际未运行**。

### 秘密：文件挂载为主，Docker secrets 按需

- 默认：`config/`、`tls/`、`secrets/` 通过只读文件挂载提供；不进入镜像，不写入 env。
  `target.env.example` 增加 secrets 目录/manifest 变量；`secrets.manifest.example` 记录
  文件名、0600、消费者与轮换重启顺序（不含真实值）。
- 可选：`compose.docker-secrets.yml` 定义 Docker secrets 覆盖并将文件挂到 `/run/secrets/`；
  选择该模式时服务 config JSON 的秘密路径需指向 `/run/secrets/<name>`（运维生成配置）。
- 人工轮换：`scripts/deploy/target-rotate-secret.sh --env deploy/target.env --name <name>
  --value-file <file> --confirm`，先做 P14c 离线备份，再按 Identity→Runtime/Scheduler/Model→
  Host→Adapter→Control→Panel 顺序重启，验证新凭据可用/旧凭据失效。KMS/热轮换**延期**。
- 轮换脚本离线测试：`bash scripts/deploy/tests/target_rotate_test.sh`（manifest/原子替换/0600/备份/回滚检查）。

### 验收状态（不得混用）

| 类别 | 状态 |
| --- | --- |
| 单机数据层备份/恢复/故障演练（P00 PG16.4） | PASS（见 reports/p14c） |
| 单主机 Docker Compose（默认部署形态） | **未验证**（本机无 Docker；YAML/profile/shell/release 规则已离线实测） |
| 两主机 + 跨主机断网/重连 | **延期并单独记录**（未验证） |
| 远端 CI | 未运行远端 CI；P14b 入口存在 |
| 正式发布 | **未就绪**（官方 PG 镜像 digest、Compose 验收、主机/秘密托管、保留离站/密钥托管待确认） |

剩余待用户确认：备份保留周期、离站位置/频率、密钥托管方式（KMS/离线/分片）。

## P05b-6 instance delegation sealed file

Refresh tool: `scripts/deploy/instance-delegation-refresh`. It reads administrator access token
from stdin (`--token-stdin`) or a 0600 `--token-file`, calls Identity IssueDelegation using
Control mTLS credentials, and atomically replaces a 0600 regular file under `output_dir`. The
Runtime container receives that directory as a shared (read-only) volume on the same host.
`--interval` must be below the 56s TTL (e.g. 20s); expired/missing files must leave Runtime
fail-closed. See `reports/analysis/p05b6-sealed-delegation-file.md` for the full contract.
