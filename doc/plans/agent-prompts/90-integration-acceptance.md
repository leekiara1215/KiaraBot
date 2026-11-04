# 提示词：最终集成验收

你负责KiaraBot五项扩展的最终集成验收，目录`/Users/betta/Workspace/KiaraBot`。
先读`doc/plans/2026-11-04-next-agent-prompts.md`、各交接/后续实现报告、AGENTS与规范。
你不是唯一编辑者，不回滚已有差异；先确认其他agent已冻结源码和共享契约/构建锁已交你。

不要拿旧167/167、注册成功、reference或局部fake证明本次完成。
先核对本次最终源码清单、新TU的CMake覆盖、临时Host诊断去除、文档与协议一致、未提交生成物/秘密。
按最终contract版本生成并校验descriptor；从真实起始descriptor验证兼容，不能新旧自比。
最新119/378/27只属于v1.1，v1.2须重新记录RPC/消息/golden数量及状态/evidence结果。

在唯一共享构建窗口执行：
source build/env/p00-env.sh
export PATH="$PWD/build/toolchain/node-v22.14.0/bin:$PWD/build/format-venv/bin:$PWD/build/architecture-installed/arm64-osx/tools/protobuf:$PWD/build/architecture-installed/arm64-osx/tools/grpc:$PATH"
cmake --preset full-locked -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build --preset full-locked --parallel 4
sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py
ctest --preset full-locked --no-tests=error --output-on-failure

同时验收完整contracts（legacy reader与原始baseline启用）、OpenAPI/AsyncAPI/schema/status/evidence、
新版本跨语言roundtrip、Python SDK、Panel contract/lint/test/build，以及全部新增真实链路。
API脚本使用仓库现成入口，新增测试必须实际运行；生成Python与使用的descriptor必须同版本。

架构检查至少包含：
python3 scripts/quality/check-boundaries.py
python3 scripts/quality/check-architecture.py --inner
python3 scripts/quality/check-architecture.py --database build/full-locked/compile_commands.json
python3 scripts/quality/check-architecture.py --links build/full-locked/architecture-targets.json
git diff --check

运行改动C++格式/专项tidy，按风险决定全仓门禁；完整入口为scripts/check-toolchain.sh，
全量tidy可用KIARABOT_TIDY_BUILD_DIR=build/full-locked scripts/quality/check-tidy.sh。
不要并行运行重型tidy与限时PG/HTTP套件。
此前全仓格式27条未改失败、专项tidy37/40（3基线失败）属于历史证据，不当作当前计数；
重新区分当前新增问题与确实可复现的基线问题，仍失败的门禁必须如实报告。

逐项核对：WSS全频道与Panel恢复；streaming受管插件全链；HERR回归；
Host真实身份/gate/drain/recover/Control；BOOT签发/reissue/秘密文件/消费及字段链；
restore epoch轮换、普通restart保留、拒绝/撤权/超时/依赖故障/崩溃/重复/冲突路径。
发现缺陷发回责任agent修复；保留首轮失败，不能删断言或只重跑孤立项便称全量通过。

更新doc/27与执行台账、必要doc/30增补和直接模块文档。HERR/其他capability只按实际证据登记，
保护doc18不变。交付实际命令/退出码/环境/日志及未运行原因、文档影响与更新清单、剩余外部环境限制。
不自动发布、调用真实QQ、付费Provider或追加已延期的两主机演练。
