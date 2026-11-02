# P14c 本地真实备份/恢复演练记录

- 日期：2026-11-02（本机 macOS，单机）
- 环境：P00 PostgreSQL **16.4**（`build/pg-16.4-install`，`server_version_num=160004`），owned 临时集群（trust 仅在 setup 前使用，实际 host 认证为 SSL + SCRAM-SHA-256，`hostssl`/`hostnossl reject`）；SQLite 3（Host journal、Adapter outbox、Adapter actions，均开 WAL）。
- 镜像/双主机/Docker：**未验证**（本任务不要求）。
- 演练脚本：`scripts/ops/target-drill.sh`；备份/恢复脚本：`scripts/ops/target-backup.sh` / `target-restore.sh`。
- 结果：**PASS**（最近一次完整运行）。

## 数据与规模

四个私有 PG 库由真实 `kiarabot-pg-initialize` 初始化：

| 服务 | 初始化 schema 版本 | 演练代表性数据 |
|---|---|---|
| identity | 14 | `kiarabot.audit_entries` 1 行 + schema_versions |
| runtime | 14 | 同上 |
| scheduler | 9 | 同上 |
| model | 8 | 同上 |

每个库关系数 ≥20（脚本断言）；SQLite：Host `user_version=3`、Adapter outbox `user_version=2`、Adapter actions `user_version=3`，各 1 行；材料包含 `tls/ca.pem`、`secrets/model-root-key.hex`（0600）、config、host credentials。

## 主流程实测

1. 初始化空库 → 写入代表性数据 → 验证（schema/关系/行）。
2. `target-backup.sh --offline`：四库 `pg_dump -Fc` + 3 个 SQLite checkpoint/VACUUM INTO + material tar；manifest 记录版本/SHA-256。
3. 破坏：四库 `DROP SCHEMA kiarabot CASCADE`、删除全部 SQLite、删除 root key/CA/部分 config/credentials。
4. `target-restore.sh --confirm ... --verify-sql` → 全量恢复。
5. 校验：四库 schema 版本分别 14/14/9/8、关系数、代表性行、sqlite integrity/user_version/行、material/root key 权限。

实测（最近一次）：**backup=2.9s，RTO=4.6s**；此前若干次 backup 3.1–9.1s、RTO 4.6–8.5s（本地临时集群、无生产负载）。
- **RPO=0s**：备份要求显式 `--offline` 且四库 `pg_stat_activity` 无其他 client 连接；备份前已停止写入，备份期间不接收写入。
- **RTO=4.6s**：从备份集恢复四库+SQLite+材料并完成校验；不含服务启动与业务观察窗口。

## 故障演练（全部显式失败/成功，均实际运行）

- 单库数据丢失（runtime `DROP SCHEMA`）：全量恢复 PASS，四库一致。
- 单服务数据库不可用（`ALTER DATABASE ... ALLOW_CONNECTIONS false`）：备份立即失败并指出 scheduler，**不生成部分备份集**；恢复连接后继续。
- 活跃 writer（pg_sleep 连接）：`--offline` guard 阻止备份，无 manifest 发布。
- 证书缺失（移走 `tls/ca.pem`）：备份显式失败，指出缺失材料。
- root key 缺失（移走 `secrets/model-root-key.hex`）：备份显式失败。
- Host journal 损坏（写入非 SQLite 内容）：备份在 SQLite checkpoint/integrity 阶段显式失败。
- 部分恢复禁止：备份集删除 `runtime.dump` 后 restore 在预检阶段失败，未触碰 live 数据；再次验证 live 四库仍正常。
- 脚本安全：无 `--offline` 备份 exit 2；无 `--confirm` 恢复 exit 2；恢复不自动迁移、不清库、不自动启动服务。

## 未完成/未验证

- Docker Compose / 双主机 / 真实离站复制：未验证（本任务范围外）。
- 真实服务进程启停下的备份窗口（P13b drain 完成到停写的具体时长）未由本演练测量；脚本只强制停写后执行。
- 备份保留周期、离站位置、密钥托管策略需用户确认。
- 真实生产数据量下的 RTO 未测（当前为本地小数据量）。

## P14 范围决定对齐（用户确认，2026-11-02）

- **默认单主机 Docker Compose**（PG + 控制面 + 执行面同机，各服务独立容器/TLS/凭据/DB 权限边界）：本演练是该单主机数据层的前提验证；Compose 容器化本轮未实际运行（本机无 Docker 验证）。
- **PG 官方镜像**：生产目标 `postgres:16.4-bookworm` 复现 P00；生产使用发布时 PG16 当前修订并固定 digest；full-locked CI 保留 P00 源码构建。本机无 Docker，本次真实演练使用 **P00 源码构建 PG 16.4（server_version_num=160004）**，与官方 16.4 语义目标一致；**官方镜像 digest/容器验证待有 Docker 环境**。
- **秘密**：只读文件挂载为主 + 按需 Docker secrets；本期人工轮换，KMS/热轮换延期。本演练已实际完成一次凭据替换：轮换 identity migration 角色密码 → 旧密码连接被拒、新密码通过同一备份/恢复脚本使用并重新备份成功；**服务进程按序重启（Identity→Runtime/Scheduler/Model→Host→Adapter→Control）在数据层演练中未启动真实 daemon，需在单机 Compose 验收补做**。
- 两主机可选部署/跨主机断网重连演练：**延期并单独记录**；本报告不声称已验证。
- 备份保留/离站/密钥托管：仍为**需用户确认项**。

## 分类验收状态

| 类别 | 状态 |
|---|---|
| 单机数据层备份/恢复/故障演练（P00 PG16.4 + SQLite） | **本机实测 PASS**（RPO=0s，RTO 4.6–12.1s，小数据量） |
| 单主机 Docker Compose（默认部署形态） | **未验证**（本机无 Docker；runbook 已给出顺序与健康口径） |
| 两主机部署 + 跨主机断网/重连 | **延期并单独记录**（未验证） |
| 远端 CI | 本文档未运行远端 CI；P14b 入口存在，此次只跑本地脚本/演练 |
| 正式发布 | **未就绪**：官方 PG 镜像 digest/秘密挂载/保留离站/密钥托管/单机 Compose 验收未完成 |
