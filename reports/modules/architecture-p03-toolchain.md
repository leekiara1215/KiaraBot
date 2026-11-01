# P03 工具链与 P00 收尾

工作目录仅 kiarabot-toolchain，初始干净；从父已审查 `15a6a83` 建立
`codex/p03-toolchain`，保留P02分支。首次Git默认执行因共享.git只读失败，按实际环境规则
重试后分支创建成功；没有在其他worktree写源码。目标仅工具链/锁/验证/CMake/fixture接线，
Core四库端口与PG实现等待父给已审查提交，不自行发明接口。

## 能力一：Python3.11.18独立预期归档摘要

未再次盲试HTTP403。检查固定uv0.13.6已安装二进制，发现其内嵌Zstandard压缩下载清单；
解压得到6736项JSON，其sha256为98e270b3d5140b27abd1ab2e1c364f30f3f3f00f5dc58cd269cb55aedeb043ba。
uv原生darwin-arm64二进制sha256为3996850e6d85e60b0142b40af4bd984822d65f736b52209921c47312450685cb，
与固定PyPI包RECORD逐字节匹配，安装包已受quality requirements归档hash约束。

按明确metadata_key匹配版本、build、官方源URL，再确认UV CLI镜像URL的release/文件路径相同，
不是从邻近字节猜digest。得到：

- macOS arm64：f3e1cd83a7f1d70e58b0d69102c18bd6867d2f973068dc5cbfd5822e445c66df
- Linux x86_64 glibc：17c4978a36c496dc53c08899ef8dad1ad2734f6325f08d6033a1e66419e0af26

这些是固定uv下载器使用的预期归档SHA-256；原HTTP403与未独立下载归档体状态保留，
不把元数据验证说成归档体校验成功，也不使用安装目录hash冒充发行归档hash。

新增verify-python-metadata.py重放提取/来源digest检查，仅读本机已锁二进制与显式zstd库；
metadata/二进制/源URL/版本/build漂移均失败。check-dependency-lock.py新增Python摘要完整性
检查，并允许--archive对将来取得的这两个归档作真实流式校验。

实际检查：元数据提取验证通过；python_metadata_test.py 3 tests通过（含4类漂移、缺失、
二进制篡改拒绝）；dependency_lock_test.py 3 tests和离线依赖检查通过；git diff --check通过。
未运行新PG业务/fixture测试：接口和实现尚未交付。本次未改C++/proto/API，无需生成/构建；
原归档体下载仍未验证，不重试无限网络请求。

文档影响：doc/11同步hash来源/复验/未验边界，本报告登记执行。无需更新Core/infra业务模块、
运行语义、协议/API：接口、状态、数据库与业务行为未变；doc18只读，CI workflow未修改。

## 能力二：统一入口使用实际 CMake 依赖上下文

修复实际硬编码：安装元数据检查与standalone基础设施配置均改读full-locked缓存中的
installed-dir/triplet/toolchain，不再读取当前source默认prefix或用外部环境替换已配置图。
新cmake-check-context.py只输出四个允许字段及派生本机构建目录，不泄露任意cache变量。
同时检查内部/公开prefix一致；路径空格/等号保留；缺失/相对/重复/冲突/triplet拒绝。
独立build目录按上下文隔离，避免复用旧Protobuf/SQLite find_package缓存。没有清理他人目录。

实际检查：4项上下文回归通过，包含其他worktree prefix与空格/等号、上下文变更后缓存隔离、
5类拒绝和等价归一化路径。读取现有真实full-locked缓存得到既有固定prefix；按输出真实
配置standalone infrastructure及构建/CTest的结果在后文记录。不重复P02六进程全套。
脚本bash -n及git diff --check通过。doc/11与本报告同步；无C++/proto/API/PG业务变化，
不更新Core/infra模块运行语义；CI workflow留父协调，doc18未改。

主对话接管补记：旧工具链对话停在历史权限审批，未继续CMake接线。主对话在同一工具链
worktree实际运行 ctest --test-dir build/infrastructure-locked --output-on-failure
--no-tests=error -R '^http_model_provider$'，1/1通过、0.65秒；这是主对话代跑结果，
不是子对话亲跑。主对话审查缓存上下文补丁后移入集成树、实际4项上下文回归、锁校验、
bash语法与真实root full-locked缓存解析通过。后续P03构建接线改由新对话接管，旧对话
无需继续改共享构建文件；不通过处理审批按钮推进。
