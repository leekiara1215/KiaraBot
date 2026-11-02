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
2. **两主机拓扑待用户确认**：Host-A（PG + Identity/Runtime/Scheduler/Model/Control/Panel）+
   Host-B（Plugin Host/Adapter）是提案默认值，尚未经用户确认；见
   `reports/analysis/p14-deployment-change-proposal.md`（codex/p14 worktree，gitignored）。
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
| `compose.exec.yml` | Plugin Host + Adapter；Host SQLite/包/凭据卷；Adapter P06c `BRIDGE_*` 配置与 bootstrap secret；REST/WS 只走 Control |
| `target.env.example` | 两代/主机拓扑变量模板（镜像、端口、material 路径）；不包含秘密 |
| `target/README.md` | 部署生成材料布局（config/tls/secrets/data/packages/backups） |
| `../scripts/deploy/target-initialize.sh` | 单个私有库的显式空库初始化（P03 CLI 包装；无清理/无自动迁移） |
| `../scripts/deploy/target-smoke.sh` | 本地结构检查 + 有 Docker 时的 compose config/构建入口 |

## 在 Docker 可用 Linux 主机上的执行顺序（示例，未验证）

```bash
cp deploy/target.env.example deploy/target.env && chmod 600 deploy/target.env
# 1) 生成 PG 角色/库、TLS/CA、密码文件、config、identity enrollment、adapter bootstrap
#    （布局见 deploy/target/README.md；由部署脚本/运维生成，不提交）
# 2) 启动 PostgreSQL（不自动建库）
docker compose --env-file deploy/target.env -f deploy/compose.infra.yml up -d postgres
# 3) 逐库显式初始化（四个非原子，逐库检查）
for svc in identity runtime scheduler model; do
  docker compose --env-file deploy/target.env -f deploy/compose.infra.yml run --rm "init-$svc"
 done
# 4) 离线身份 bootstrap（admin/enrollment）
docker compose --env-file deploy/target.env -f deploy/compose.infra.yml run --rm identity-bootstrap
# 5) 启动数据/控制面
docker compose --env-file deploy/target.env -f deploy/compose.infra.yml -f deploy/compose.core.yml up -d
# 6) 执行面（同一主机 dev；两主机时在 Host-B 用 compose.exec.yml）
docker compose --env-file deploy/target.env -f deploy/compose.exec.yml up -d
```

停止顺序按 doc/08 P14a 章节；P13 的 drain 语义尚未合入，当前顺序是临时口径。
