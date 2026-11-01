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
