# P05b-8：Runtime sealed 父委托 reader 泛化（task+model）

日期：2026-11-03；基线 main=`241f983`（含 P05b-7）。

## 范围

把 main 的 `SealedInstanceDelegationSource` 从仅 task 扩展为 task+model 两类 envelope，供
P05b-5 Scheduler 路径与 P05b-4 Model 路径共用，避免 P05b-4 复制 reader。

## 接口

- `SealedInstanceDelegationSourceOptions{path, model_path}`；model_path 为空时从 task 路径
  推导 `instance-delegation-<instance>-model.json`。
- `SealedInstanceDelegationSource::acquire`（task）与 `acquire_model(context, model_id,
  permission)`（model）。
- `RuntimeInstanceDelegationProvider::scheduler_task_delegation`（保持原语义）与
  `model_chat_delegation(context, model_id, permission)`。
- Runtime-only 配置 `model_instance_delegation_sealed_file`；未配置时推导兄弟文件。

## 校验与交换

- task envelope：`task.schedule/task.read`，scope 含同 instance（可选 installation），无
  MODEL；Exchange audience=Scheduler，scope INSTANCE+INSTALLATION。
- model envelope：`model.chat/model.call.inspect`，scope 恰好一个 MODEL 且 id=请求 model；
  Exchange audience=Model，scope 仅 MODEL。task/model 权限混合、跨实例、跨模型、错 scope/
  audience/proof 时间戳/TTL>56s/非 0600/符号链接/篡改均 fail-closed（UNAUTHENTICATED）。
- Exchange 结果校验 audience/hops=2/expiry 由现有 `RuntimeIdentityClient` 执行；provider 缓存
  仅到子委托/父委托/56s 上限的较早者，不缓存父 token。

## 测试

- `p05_sealed_delegation_source`：原 task 矩阵回归 + model 成功、跨模型、task/model 混合、
  task envelope 被 model reader 拒绝。
- `p05_instance_delegation_provider`：原 Scheduler 路径回归 + model 成功/缓存/跨模型拒绝。
- 全量构建 + `ctest -R 'p05|p10|p11|p03_postgres'` 全绿；contract 门禁通过；format16 clean。

## 未完成

- 真实 Model client/组合根接线（P05b-4）；本任务只提供可复用 reader/provider API。
- model 文件的真实部署刷新/在线 CheckAuthorization 联合验收由 P05b-4/P05b-7 负责。
- account 归属与 P13h-3 无关，未在本任务修改。
