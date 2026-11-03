# P05b-12：安全评审 F-1/F-2 修复

日期：2026-11-03；基线 main=`9798083`。修复 P05b-11 评审的 Medium 防御纵深问题，不改契约/
proto/配置字段。

## F-1 provider 缓存 key 缺 installation（已修复）

- 原状：task/model cache key 仅 kind/instance/model/permission；缓存命中在 sealed 文件读取前
  返回，跳过本次 installation 复核。
- 修复：key 加入 installation；`Cached` 记录 installation。跨 installation 必定 miss，重新
  `acquire*`/Exchange；跨 installation 由 sealed envelope 与下游 scope 继续拒绝。
- 测试：provider 测试新增同 instance 改 installation（task+model）不命中缓存并 fail-closed。

## F-2 sealed reader TOCTOU（已修复）

- 原状：`regular_private_file()` 以 O_NOFOLLOW open/fstat 后关闭，随后 `std::ifstream` 按路径
  重开，校验与读取之间存在替换窗口。
- 修复：`read_private_file()` 单次 O_RDONLY|O_NOFOLLOW|O_CLOEXEC open；fstat 校验 regular/
  0600/owner/1..64KiB；从该 fd 完整读取；读取后再次 fstat 复核 dev/inode/size；不再按路径重开。
- 测试：sealed source 测试新增“source 构造后路径被替换为符号链接 → 拒绝”；既有 symlink/
  mode/owner/大小/过期/tamper 矩阵保持通过。

## 验证

- 全量构建 exit 0；`ctest -R 'p05|p10|p11|p03_postgres'` 运行中 `p10_timer_e2e` 一次既有
  时序 flake，隔离重跑通过；其余全部通过。
- `p05_sealed_delegation_source`/`p05_instance_delegation_provider` 通过；format16/diff clean。
- F-3（refresh token 老化/single-flight）未在本任务修改，由 task-156 处理。
