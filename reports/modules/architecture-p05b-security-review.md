# P05b-11：B1 实例委托链对抗性安全评审

日期：2026-11-03；基线 main=`8455533`（B1/P13h-3 已合入）。范围：Identity 实例父委托签发、
sealed 文件交付/reader、Runtime sealed provider、managed Host Model 链、Scheduler 链、
PluginBridge owner 派生与错误/日志面。**本评审未修改生产实现**；发现项按优先级上报 Lead。

## 1. 结论摘要

- **Critical：0，High：0，Medium：3，Low：2，Info：4**。
- 未发现可直接绕过认证的跨实例/跨模型/跨安装越权；核心授权由 Identity Exchange/CheckAuthorization
  在线校验，sealed 文件本身不是授权凭据。
- 需修复项（建议按顺序）：F-1 cache key 缺 installation、F-2 sealed 读取 TOCTOU、F-3 refresh
  admin token 生命周期；F-4/F-5 为加固。

## 2. 威胁矩阵

| 威胁 | 防护/证据 | 缺口 | 严重度 |
| --- | --- | --- | --- |
| 伪造/篡改 sealed JSON/token | reader 校验 0600 regular、非 symlink、owner、精确 schema、instance/installation/audience、权限与 scope 白名单、0<ttl<=56、proof 时间戳；`p05_sealed_delegation_source` 覆盖篡改/过期/错 scope | 无签名校验（设计上由 Identity Exchange 在线验签，非缺口） | Info |
| 替换 sealed 文件为另一实例/模型/安装 | envelope instance/installation 与已认证 context 精确匹配；model 要求唯一 MODEL scope 且 id=请求 model；`p05b_sealed_delegation_file`/C++ 测试拒绝混合/跨模型/跨实例 | 见 F-1（缓存命中绕过本次文件复核） | Medium |
| 跨实例/跨模型/跨安装越权 | Exchange 绑定真实 Runtime mTLS 叶指纹 + 父 token subject/scope/current assignment；下游 Scheduler/Model CheckAuthorization 再校验；测试覆盖跨 model/instance 拒绝 | F-1 缓存 key 只含 instance/model，不含 installation | Medium |
| 撤销/过期延迟 | 每次 Exchange 重新读取 assignment/authorization 版本；父/子/缓存到期取最早；不缓存父 token | 缓存窗口 5s 内撤销不即时生效（设计允许的短缓存）；需文档化 | Info |
| 缓存超期 | provider expiry=min(now+cache_ttl, child.expires_at, parent.expires_at, 56s)；`p05_instance_delegation_provider` 验证缓存命中减少 Exchange | 无负缓存/并发 single-flight | Low |
| Runtime 叶证书绑定绕过 | Exchange request 携带实际 Runtime 叶 SHA-256；Identity 要求 Runtime platform peer 且 `client_certificate_sha256` 等于实际 peer；child claims 绑定该指纹 | 无独立跨 Runtime 重放；sealed 文件被同 UID 读取后仍需匹配 Runtime 叶 | Info |
| admin token 泄露/重放 | refresh 工具从 stdin 或 0600 文件读取，不进 argv/日志；错误稳定；IssueDelegation 使用 Control mTLS | F-3 循环长期复用同一 token，无过期感知/重认证 | Medium |
| 日志/argv/env 秘密泄漏 | 刷新工具无 token 参数、错误不打印 token/JWT；Runtime reader/provider 错误稳定；P08 host credential helper 无明文 | sealed token 必然驻留内存/可被同机 root 或 core dump 读取（F-5） | Low |
| PluginBridge 自报 owner/installation | task/model ingress 从已认证 fence/profile 派生 instance/installation，不接受 wire owner；model HostRequest 同一规则 | 无 | Info |
| Provider 错误路径信息泄露 | grpc/stable ErrorCode 映射；父/子 proof 不进入错误文本 | 无 | Info |
| 预算耗尽/DoS | Exchange 每次带 deadline，Identity 有 delegation budget；provider 5s cache 限制重复 | F-4 无 per-instance 并发/速率限制，cache miss 可放大 Identity 负载 | Low |
| 并发刷新竞争 | 刷新器单进程原子写 sealed；Runtime reader 校验完整文件；provider 缓存加锁 | 多刷新进程/多 writer 无租约；cache 与文件替换无 generation 绑定（F-1/F-2） | Low |
| 父 token 形状/admin 授权 | Identity P05b-2 校验 instance principal/binding、exact assignment、parent hops/audience/TTL、admin 来源；下游 CheckAuthorization | 仍待运维保证 admin token 最小权限/短生命周期 | Info |
| sealed model 文件可被向下游转发 | model child Exchange audience=model、scope 仅 MODEL；downstream 要求 owner 自己 mTLS 叶验证 | 无 | Info |

## 3. 发现与修复建议

### F-1（Medium）Provider 缓存 key 缺 installation，缓存命中跳过 sealed 文件安装绑定

- 证据：`core/src/runtime/platform/instance_delegation_provider.cpp` 中 task key 为
  `task|instance|permission`、model key 为 `model|instance|model|permission`；命中缓存在
  `options_.source->acquire*()` 之前返回，不再校验 `context.installation` 与文件内容。
- 影响：同一 instance 的恶意/错误 context（不同 installation）可复用已缓存的 child proof，
  绕过本请求的 sealed installation 复核。当前 PluginBridge 的 installation 由认证 profile 派生，
  正常路径不可自报；下游 Scheduler scope 也会带 installation，但 Model scope 通常只含 model，
  属于 defense-in-depth 缺口而非直接越权。
- 最小验证：代码检查 + 现有 provider 测试无 installation 变化用例；建议新增单测：同 instance、
  改 installation，断言不命中缓存且 source/Exchange 被重新调用/拒绝。
- 修复建议：cache key 加入 installation（task/model 均加），并考虑加入 sealed 文件 inode/mtime/
  source generation；模型快照变化时清空对应 key。

### F-2（Medium）Sealed 读取 TOCTOU：stat/open 校验后按路径重新 `ifstream` 打开

- 证据：`sealed_instance_delegation_source.cpp` 的 `regular_private_file(path)` 以
  O_RDONLY|O_NOFOLLOW 打开并关闭，随后 `std::ifstream input(path_)` 重新按路径打开；两个动作
  之间同 UID/目录写者可替换文件（或换成 symlink），读取阶段不再有 O_NOFOLLOW/0600/owner 保证。
- 影响：本地同 UID/目录权限攻击者可改变 Runtime 实际读取内容；由于 Exchange 在线校验且
  envelope/instance/scope 仍校验，直接跨实例提权受限，但可造成拒绝服务/竞态或读取到攻击者
  构造的父 token（仍需 Runtime 叶 + Identity 验证）。
- 建议：单次 `open(O_RDONLY|O_NOFOLLOW|O_CLOEXEC)` → `fstat` 校验 → 从该 fd 读取 →
  读取后再 `fstat` 比较 inode/size/mtime；拒绝非 regular/owner/mode 或大小变化。

### F-3（Medium）refresh 工具长期复用 admin token，无过期重认证/告警

- 证据：`scripts/deploy/instance-delegation-refresh` 的循环只在首轮读取 token；后续循环继续用
  同一 token，失败仅稳定打印错误，不重新读取 token 文件/不做退避终止；`token_stdin` 模式
  永不重读。
- 影响：管理员 session/credential 撤销后刷新持续失败（Availability）；泄露 token 在进程内存
  中长期存在并在 token 有效期内可重放（Confidentiality/Integrity 风险）。
- 建议：设置 token 最大年龄/重读 token 文件；UNAUTHENTICATED 时停止并告警（而不是无限循环）；
  明确要求短生命周期 admin token + 最小权限；评估使用专用 refresher principal。

### F-4（Low）Exchange 缺少 per-instance 并发/速率单飞

- 证据：provider 仅对 cache map 加锁；并发 cache miss 可同时发起多次 Exchange；无每实例
  in-flight/令牌桶，Identity 侧 delegation budget 只约束单次请求。
- 影响：恶意/故障插件实例可制造 Exchange 放大，消耗 Identity/网络资源（DoS）。
- 建议：per-instance/model single-flight + 有界并发/令牌桶 + 负结果短缓存；保持失败闭锁。

### F-5（Low）秘密驻留内存/core dump

- 证据：sealed token、child proof、admin token 均为普通 `std::string`/Python str，缓存到
  子委托到期；无 mlock/显式清零；进程异常可产生 core dump。
- 影响：同机 root/崩溃转储/调试可读取短期凭据；属主机威胁模型边界。
- 建议：部署禁用 core dump/限制 ptrace；对高价值容器启用 mlock；C++ 缓存过期时 best-effort
  zeroize（注意 allocator 限制），并在文档中明确主机信任边界。

### F-6（Info）reader 未在 Exchange 前校验 proof `sub` 与 envelope instance 一致

- 证据：`decode_proof` 仅要求 `sub` 非空，未比较 `sub == instance_id`；最终 Identity Exchange
  会因 subject/scope/assignment 不匹配 fail-closed。
- 建议：增加前置一致性校验以节省 Exchange 预算并缩小攻击面（非提权缺口）。

## 4. 已确认防护（正面结论）

- sealed reader 拒绝 symlink/非 0600/错 owner/超大/错 JSON、跨实例/跨模型/混合权限、TTL>56s、
  过期/时间戳不一致；C++/Python 测试通过。
- Exchange 强制 Runtime platform peer + 实际叶证书 SHA-256 + 父 token subject/assignment +
  child audience/hops=2/expiry；跨 Runtime 证书重放不可用。
- Provider 只缓存 child proof 到最早到期时间，不缓存父 token；task/model Exchange scope 和
  audience 分离。
- PluginBridge task/model ingress 的 instance/installation 来自认证 fence/profile，不接受客户端
  自报 owner；错误映射稳定且不含 token。
- 刷新器 token 仅 stdin/0600 文件、输出原子 0600、错误稳定；但需 F-3 生命周期改进。

## 5. 证据与最小场景

- 基线：main=`8455533`；`p05_sealed_delegation_source`、`p05_instance_delegation_provider`、
  `p05b_sealed_delegation_file` 在本地构建后通过（3/3，约 6s）。
- 最小拒绝场景（Python `sealed_delegation.validate_document`，临时数据）：
  - task 文件混入 `model.chat` → `task and model permissions must use separate sealed files`；
  - task 文件 scope 换成 `RESOURCE_MODEL` → `task sealed file requires INSTANCE scope`；
  - `ttl_seconds=57` → `TTL must be positive and <=56s`。
- 代码静态证据路径：`scripts/deploy/sealed_delegation.py`、`instance-delegation-refresh`、
  `sealed_instance_delegation_source.cpp`、`instance_delegation_provider.cpp`、
  `identity_exchange_application.cpp`、`runtime_bridge.cpp`、`runtime_model_client.cpp`。

## 6. 修复优先级

1. F-1（缓存 key/文件 generation）——Medium，先加单测。
2. F-2（单 fd O_NOFOLLOW 读取）——Medium。
3. F-3（refresh token 生命周期/告警）——Medium（部署工具）。
4. F-4/F-5/F-6 —— Low/Info 加固，可随部署硬化批次做。

本报告不宣称 B1 已通过第三方安全审计；仅覆盖上述代码/契约/最小场景。
