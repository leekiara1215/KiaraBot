# 后续 Agent 任务提示词与交接入口

交接日期：2026-11-04。用户决定将后续工作交其他 agent；本轮停止继续实现，保留当前工作区。
下列文件均是可以直接复制的任务提示词。

## 如何分配

- 只交给一个 agent：复制[统筹提示词](agent-prompts/00-coordinator.md)，让它接管全局并分派。
- 分给多个 agent：分别复制 [WS](agent-prompts/10-control-ws.md)、
  [模型 streaming](agent-prompts/20-model-streaming.md)、[Host](agent-prompts/30-host-management.md)、
  [bootstrap](agent-prompts/40-bootstrap-delivery.md)。共享文件按下述顺序移交，不能同时独立改。
- 实施结束后交给[集成验收 agent](agent-prompts/90-integration-acceptance.md)。

## 所有提示词的共同上下文

仓库：`/Users/betta/Workspace/KiaraBot`；HEAD 为
`0496b90053d046fd61d7f106090bb87fff172f8a`，但真正交接对象是**包含大量未提交改动的当前工作区**。
不要重新从 HEAD 开始实现，不要 reset、revert、stash 或覆盖既有改动，不自动提交、推送或发布。
先读根 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md 与所属模块文档。
`doc/18-architecture-baseline.md` 及其保护清单只读。

用户已批准五项扩展及自主执行，无需逐步重复确认：WS、模型 streaming、HostRequest 错误分类、
Host operation、bootstrap 安全交付。两主机/断网演练仍延期。不要调用真实 QQ、付费 Provider 或发布。
HERR 已局部实现验收并登记，后续保留其回归；其他能力还没有整链验收。

所有接手者先读：

- [批准范围与34项任务](../29-approved-capability-expansion-plan.md)。
- [v1/v1.1契约冻结及实测](../30-capability-expansion-contract-freeze.md)。
- [执行台账](2026-11-04-capability-execution-ledger.md)。
- [WS 源码交接](2026-11-04-control-ws-handoff.md)。
- [Model 源码交接](2026-11-04-model-streaming-handoff.md)。
- [Host/bootstrap 源码交接](2026-11-04-host-bootstrap-handoff.md)。

### 交接时的真实状态

- v1.1已冻结，119 RPC / 378种消息 / 27组 golden 通过跨语言校验；
  `CheckHostGateAuthorization` 只是契约通过，其生产身份链仍在实施。
- WS-02真实PG journal通过；WS-03真实R/S owner + Identity当前授权通过。
  WSS protocol/source新文件只是半成品，网络接线与Panel尚未完成。
- WS余下 logs/tasks/messages 缺完整可靠数据源，已核定需要v1.2四个查询RPC。
  **v1.2尚未改wire或registry，也未冻结，不能把119 RPC的旧证据用于新声明。**
  当前公共wire仍为已验的v1.1；暂停共享生成的旧锁已随交接释放，新修改时重新取得锁。
- Model Provider/parser已通过，旧Chat/新流应用/真实PG三项通过。
  新STR06 codec/RPC/fixture有未接CMake、未构建的源码，不能当作已验收实现。
- HERR真实TLS raw-wire通过，只有target两条相关语义登记implemented；reference未翻转。
- BOOT-03文件交付通过，Runtime签发/重发/Control生产接线与消费链仍未完成。
- HOST-02真实maintenance测试仍失败。最后精确诊断：TERM后、KILL阶段errno=1，
  随后的同marker进程已不存活；原HostProcess测试通过。`host_process.cpp`交接时仍可能含临时诊断，
  必须核对并收敛。不能将进程不明状态直接改成成功。
- 新Identity应用/RPC/CLI/config改动尚未经统一构建，不沿用旧daemon结果。
- 旧167/167 CTest只属于此前工作区；当前代码不承诺全量可编译或全绿。

### 文件锁与依赖顺序

1. WS agent先收口v1.2：`observations.proto`、`identity.proto`、`runtime.proto`、`scheduler.proto`、
   registry/spec/goldens/相关契约测试由它单一编辑。保留v1.1与HERR登记。
2. Model agent负责Model RPC到Runtime Bridge/SDK；不改其他owner wire或PG通用schema。
   `model.proto`/`plugins.proto`若确需改契约，先交统筹评审，不能边实现边暗改。
3. Host agent先修真实终止竞态、完成Identity身份链，再接Runtime gate/Host RPC/Control。
   service_config与Host配置由它单一编辑，WS所需origins等通过它合入。
4. WS新增PG feed journal和BOOT Runtime持久化都要改共享schema/manifest/UoW，必须串行。
   默认先WS feed schema实测落盘，再把Runtime schema写锁移交BOOT；Host gate若需DDL也统一排队。
5. BOOT在Runtime资源应用、owner_gateway、Control组合根等共享文件上的写锁，须等Host/Model相关切片释放。
   如果没有统筹agent，按上述次序串行运行，不要让四个独立agent直接并发覆盖这些文件。
6. `cmake/platform.cmake`当前由WS交接，`core/CMakeLists.txt`与service_config由Host交接，
   根/infra CMake已有Provider接线；新增片段交当前编辑者合入。共用decoder也只保留一位编辑者。
7. `build/full-locked`、公共生成目录、全量CTest/编译数据库由唯一统筹管理；
   worker只用独立构建目录，或向统筹请求有限目标构建。不要同时跑重型tidy与限时PG/HTTP测试。

### 锁定环境与基本命令

```bash
cd /Users/betta/Workspace/KiaraBot
source build/env/p00-env.sh
export PATH="$PWD/build/toolchain/node-v22.14.0/bin:$PWD/build/format-venv/bin:$PWD/build/architecture-installed/arm64-osx/tools/protobuf:$PWD/build/architecture-installed/arm64-osx/tools/grpc:$PATH"
cmake --preset full-locked -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build --preset full-locked --parallel 4 --target <本次确实存在的目标>
ctest --preset full-locked -R '<精确测试名>' --no-tests=error --output-on-failure
```

Python用`sdk/python/.venv/bin/python`；Node22.14.0、clang-format16.0.6、clang-tidy22.1.8。
需要Python新wire时运行`sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py`，不提交生成物。
PG测试通过既有owned fixture执行，不连接用户数据库；保留诊断脱敏，不能打印DSN、令牌或秘密。

历史本机日志在`build/logs/capability-expansion/`，有些文件不随Git传输。
开工`/tmp`副本已丢失；原始descriptor仍在该日志目录，续执行哈希为`continuation-checkpoint.json`。
不能把新快照冒充开工基线。跨机器接手需同时复制当前未提交源码和必要证据，
缺失历史产物时明确说明，不能从新descriptor自比伪造兼容通过。
只移交必要的脱敏日志与descriptor，不能把本机环境文件、CA私钥或运行秘密夹带到交接包。

结构探索依AGENTS用codebase-memory skill；图项目`Users-betta-Workspace-KiaraBot`。
旧generation为2026-11-04T08:26:54Z，仅作定位提示；接手重查freshness/coverage。
`application/target`和Control target曾被排除，必须读源，不能据图缺节点声称未实现。

### 共同验收与交付要求

代码前写文档影响判断；可见接口、状态、错误、配置、运行行为必须同步直接文档和测试。
公共契约先改唯一来源，再应用/适配/测试。不得删除断言、吞错或用空流/假授权代替目标实现。
运行注册检查不等于测试通过；实际日志记录命令、退出码、环境、作用域，并保留首轮失败。
新feature/registry只在其实际整链证据满足时翻转。
完成后报告精确文件、行为、实际通过/失败/未运行检查、文档更新与无需更新理由、剩余依赖。
