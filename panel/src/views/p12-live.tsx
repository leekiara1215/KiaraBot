/**
 * P12 live management pages.
 *
 * These pages consume the P12a contract through ApiClient. Until the P12c
 * Control backend lands the target endpoints may answer 404/501; the pages keep
 * that state explicit (P12c 后端未实现) and never fabricate success. Locked
 * semantics: a NOT_FOUND action result is displayed as "未找到", not as an
 * execution failure; secrets are never displayed (only configured/version
 * markers); logs/audit queries are a declared P12 phase-2 deferral.
 */
import { useCallback, useEffect, useState } from 'react';
import type {
  ApiClient,
  ProcessProbe,
  Schemas,
  TargetAccountPage,
  TargetActionMetadataResponse,
  TargetActionMetadata,
  TargetActionPolicy,
  TargetInstallationPage,
  TargetInstanceConfigResponse,
  TargetModelConfigurationResponse,
  TargetModelMetadataResponse,
  TargetOperation,
  TargetTaskRecord,
} from '../api/client';
import { ApiError } from '../api/client';
import { describeError, FailureNotice, formatTime, label, type Failure } from '../ui';
import type { P12PageKey } from '../p12/catalog';

type Account = Schemas['TargetAccountRecord'];
type Installation = Schemas['TargetInstallationRecord'];
type OperationProgress = NonNullable<TargetOperation['participants']>[number];

const text = (value: string | undefined | null) => (value ? value : '—');
const pretty = (value?: string) => (value ? value.replace(/_/g, ' ') : '—');
const isBackendPending = (error: unknown) => {
  const failure = describeError(error);
  return error instanceof ApiError && (error.status === 404 || error.status === 501);
};

function useQuery<T>(load: (active: () => boolean) => Promise<T>, deps: unknown[], revision: number) {
  const [value, setValue] = useState<T>();
  const [failure, setFailure] = useState<Failure>();
  const [loading, setLoading] = useState(true);
  const [pendingBackend, setPendingBackend] = useState(false);
  const run = useCallback(() => {
    let active = true;
    setLoading(true);
    setFailure(undefined);
    setPendingBackend(false);
    load(() => active)
      .then((next) => { if (active) setValue(next); })
      .catch((error) => {
        if (!active) return;
        setValue(undefined);
        setFailure(describeError(error));
        setPendingBackend(isBackendPending(error));
      })
      .finally(() => { if (active) setLoading(false); });
    return () => { active = false; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, deps);
  useEffect(() => run(), [run, revision]);
  return { value, failure, loading, pendingBackend, reload: run };
}

function PermissionGate({ permissions, required, children }: {
  permissions: string[]; required: string; children: React.ReactNode;
}) {
  if (!permissions.includes(required)) {
    return <section className="table-card"><p className="empty">当前角色缺少 <strong>{required}</strong> 权限，后端会拒绝该操作。</p></section>;
  }
  return <>{children}</>;
}

function BackendBanner({ pending }: { pending: boolean }) {
  if (!pending) return null;
  return <div className="failure"><strong>P12c 后端未实现 / 契约已声明但 handler 未接线</strong>
    <p>本页已按 P12a 契约发起请求；Control 返回 404/501 时保持显式状态，不伪造成功。后端接入后无需改 Panel。</p></div>;
}

function TableCard({ title, eyebrow, count, columns, rows }: {
  title: string; eyebrow: string; count?: number; columns: string[]; rows: string[][];
}) {
  return <section className="table-card">
    <div className="table-head"><div><p className="eyebrow">{eyebrow}</p><h2>{title}</h2></div>
      <span className="count">{count ?? rows.length} 条</span></div>
    {rows.length === 0 ? <p className="empty">暂无数据</p> : <div className="table-wrap"><table>
      <thead><tr>{columns.map((column) => <th key={column}>{column}</th>)}</tr></thead>
      <tbody>{rows.map((row, index) => <tr key={index}>{row.map((cell, cellIndex) => <td key={cellIndex}>{cell}</td>)}</tr>)}</tbody>
    </table></div>}
  </section>;
}

function AccountsPage({ api, permissions, revision }: { api: ApiClient; permissions: string[]; revision: number }) {
  const [selected, setSelected] = useState<Account>();
  const [policy, setPolicy] = useState<TargetActionPolicy>();
  const [bindingAccount, setBindingAccount] = useState('');
  const [adapterInstance, setAdapterInstance] = useState('');
  const [notice, setNotice] = useState<Failure>();
  const [success, setSuccess] = useState('');
  const accounts = useQuery<TargetAccountPage>((active) => api.accountPage().then((value) => { if (!active()) return value; return value; }), [api], revision);
  useEffect(() => {
    if (!selected) { setPolicy(undefined); return; }
    let active = true;
    api.actionPolicy(selected.account_id)
      .then((value) => { if (active) setPolicy(value.policy); })
      .catch((error) => { if (active) setPolicy(undefined); setNotice(describeError(error)); });
    return () => { active = false; };
  }, [api, selected, revision]);
  const bind = async () => {
    setNotice(undefined); setSuccess('');
    try {
      await api.bindAdapter(bindingAccount, {
        binding: { account_id: bindingAccount, adapter_instance_id: adapterInstance },
        expected_version: '0',
      } as never);
      setSuccess('绑定命令已受理；按 operation 查询进度');
    } catch (error) { setNotice(describeError(error)); }
  };
  const unbind = async () => {
    setNotice(undefined); setSuccess('');
    try {
      await api.unbindAdapter(bindingAccount, {
        binding: { account_id: bindingAccount, adapter_instance_id: adapterInstance },
        expected_version: '0',
      } as never);
      setSuccess('解绑命令已受理');
    } catch (error) { setNotice(describeError(error)); }
  };
  const savePolicy = async () => {
    if (!policy) return;
    setNotice(undefined); setSuccess('');
    try {
      await api.updateActionPolicy(policy.account_id, {
        policy, expected_version: policy.version,
      } as never);
      setSuccess('Action 策略已提交；版本冲突会返回 CONFLICT');
    } catch (error) { setNotice(describeError(error)); }
  };
  const updatePolicy = <K extends keyof TargetActionPolicy>(key: K, value: TargetActionPolicy[K]) =>
    setPolicy((current) => (current ? { ...current, [key]: value } : current));
  return <div className="stack">
    <BackendBanner pending={accounts.pendingBackend} />
    {accounts.failure && !accounts.pendingBackend && <FailureNotice failure={accounts.failure} />}
    <TableCard title="账号（目标 Runtime）" eyebrow="LIVE · LIST ACCOUNTS" count={accounts.value?.accounts.length}
      columns={['账号 ID', '平台账号', '显示名', '版本', '生命周期']}
      rows={(accounts.value?.accounts ?? []).map((account) => [account.account_id, `${account.platform}:${account.platform_account_id}`,
        text(account.display_name), account.version, pretty(account.state)])} />
    <section className="table-card"><div className="table-head"><div><p className="eyebrow">ACCOUNT POLICY · Q3</p><h2>群白名单与危险动作策略</h2></div></div>
      <p className="muted">账号与适配器绑定、禁言/踢出开关、允许群 ID 与每日预算保存在 Runtime ActionPolicy（带版本）。</p>
      <div className="row-actions">
        <label>账号 ID<input aria-label="策略账号 ID" value={bindingAccount} onChange={(event) => setBindingAccount(event.target.value)} /></label>
        <label>adapter instance<input aria-label="adapter instance" value={adapterInstance} onChange={(event) => setAdapterInstance(event.target.value)} /></label>
        <button className="secondary" onClick={() => void bind()} disabled={!bindingAccount || !adapterInstance}>绑定适配器</button>
        <button className="secondary" onClick={() => void unbind()} disabled={!bindingAccount}>解绑</button>
      </div>
      <div className="row-actions">
        <label>查看策略<input aria-label="策略查询账号" value={selected?.account_id ?? ''} onChange={(event) => {
          const value = accounts.value?.accounts.find((account) => account.account_id === event.target.value);
          setSelected(value);
        }} /></label>
      </div>
      {notice && <FailureNotice failure={notice} />}{success && <p className="success">{success}</p>}
      {policy && <div className="table-wrap"><table><tbody>
        <tr><th>允许群 IDs</th><td><input aria-label="允许群 IDs" value={policy.allowed_group_ids.join(',')} onChange={(event) =>
          updatePolicy('allowed_group_ids', event.target.value.split(',').map((item) => item.trim()).filter(Boolean))} /></td></tr>
        <tr><th>禁言开关</th><td><input type="checkbox" checked={policy.mute_enabled} onChange={(event) => updatePolicy('mute_enabled', event.target.checked)} /></td></tr>
        <tr><th>踢出开关</th><td><input type="checkbox" checked={policy.kick_enabled} onChange={(event) => updatePolicy('kick_enabled', event.target.checked)} /></td></tr>
        <tr><th>禁言每日预算</th><td><input aria-label="禁言预算" type="number" value={policy.mute_daily_budget} onChange={(event) => updatePolicy('mute_daily_budget', Number(event.target.value))} /></td></tr>
        <tr><th>踢出每日预算</th><td><input aria-label="踢出预算" type="number" value={policy.kick_daily_budget} onChange={(event) => updatePolicy('kick_daily_budget', Number(event.target.value))} /></td></tr>
        <tr><th>版本</th><td>{policy.version}</td></tr>
      </tbody></table></div>}
      {policy && <button className="primary" onClick={() => void savePolicy()}>保存策略</button>}
    </section>
    <ActionMetadataSection api={api} permissions={permissions} revision={revision} />
  </div>;
}

function ActionMetadataSection({ api, permissions, revision }: { api: ApiClient; permissions: string[]; revision: number }) {
  const [accountId, setAccountId] = useState('');
  const [actions, setActions] = useState<TargetActionMetadata[]>([]);
  const [failure, setFailure] = useState<Failure>();
  const [notFound, setNotFound] = useState(false);
  const canInspect = permissions.includes('action.inspect');
  const query = async () => {
    setFailure(undefined); setNotFound(false); setActions([]);
    try {
      const page: TargetActionMetadataResponse = await api.actionMetadata({ accountId });
      setActions(page.actions);
    } catch (error) {
      if (error instanceof ApiError && error.status === 404) { setNotFound(true); return; }
      setFailure(describeError(error));
    }
  };
  if (!canInspect) {
    return <section className="table-card"><p className="empty">动作元数据需要 <strong>action.inspect</strong> 权限；管理员角色不会隐式获得该权限。</p></section>;
  }
  return <section className="table-card"><div className="table-head"><div><p className="eyebrow">ACTION METADATA · Q1</p><h2>动作元数据查询</h2></div></div>
    <p className="muted">需显式账号 scope + action.inspect；按幂等键查询仍需 owner instance（本表只按账号列出）。NOT_FOUND 只表示不可见，不推断动作失败。</p>
    <div className="row-actions"><label>账号 ID<input aria-label="动作账号 ID" value={accountId} onChange={(event) => setAccountId(event.target.value)} /></label>
      <button className="secondary" onClick={() => void query()} disabled={!accountId}>查询动作</button></div>
    {notFound && <p className="empty">未找到匹配的动作（NOT_FOUND 不代表动作执行失败）。</p>}
    {failure && <FailureNotice failure={failure} />}
    <TableCard title="动作元数据" eyebrow={`LIVE · /actions/metadata · revision ${revision}`} columns={['action_id', 'kind', 'status', 'outcome', 'updated_at']}
      rows={actions.map((item) => [item.action_id, pretty(item.kind), pretty(item.status),
        pretty(item.execution_outcome), formatTime(item.updated_at)])} />
  </section>;
}

function InstallationsPage({ api, revision }: { api: ApiClient; revision: number }) {
  const installations = useQuery<TargetInstallationPage>((active) => api.installationPage().then((value) => { if (!active()) return value; return value; }), [api], revision);
  const [instanceId, setInstanceId] = useState('');
  const [config, setConfig] = useState<TargetInstanceConfigResponse>();
  const [failure, setFailure] = useState<Failure>();
  const [notice, setNotice] = useState('');
  const loadConfig = async () => {
    setFailure(undefined); setNotice('');
    try { setConfig(await api.instanceConfig(instanceId)); } catch (error) { setFailure(describeError(error)); }
  };
  const setDesired = async (desired: 'DESIRED_STATE_RUNNING' | 'DESIRED_STATE_STOPPED') => {
    setFailure(undefined); setNotice('');
    try {
      await api.setInstanceDesiredState(instanceId, {
        instance_id: instanceId, desired_state: desired, expected_version: '0',
      } as never);
      setNotice('命令已受理；请用 operation 查看进度');
    } catch (error) { setFailure(describeError(error)); }
  };
  return <div className="stack">
    <BackendBanner pending={installations.pendingBackend} />
    {installations.failure && !installations.pendingBackend && <FailureNotice failure={installations.failure} />}
    <TableCard title="插件安装（目标 Runtime）" eyebrow="LIVE · LIST INSTALLATIONS" count={installations.value?.installations.length}
      columns={['安装 ID', '插件类型', '版本', '生命周期']}
      rows={(installations.value?.installations ?? []).map((item) => [item.installation_id, item.plugin_type_id, item.version, pretty(item.state)])} />
    <section className="table-card"><div className="table-head"><div><p className="eyebrow">INSTANCE CONFIG · Q6</p><h2>实例配置与期望状态</h2></div></div>
      <p className="muted">分开显示 Runtime 期望状态/配置版本、Host 进程事实与插件确认的应用版本；启用/停用对象必须是实例，不是安装。</p>
      <div className="row-actions"><label>实例 ID<input aria-label="实例 ID" value={instanceId} onChange={(event) => setInstanceId(event.target.value)} /></label>
        <button className="secondary" onClick={() => void loadConfig()} disabled={!instanceId}>读取配置</button>
        <button className="secondary" onClick={() => void setDesired('DESIRED_STATE_RUNNING')} disabled={!instanceId}>设为运行</button>
        <button className="secondary" onClick={() => void setDesired('DESIRED_STATE_STOPPED')} disabled={!instanceId}>设为停止</button></div>
      {failure && <FailureNotice failure={failure} />}{notice && <p className="success">{notice}</p>}
      {config && <TableCard title="实例配置快照" eyebrow="TARGET INSTANCE CONFIG" columns={['字段', '值']} rows={[
        ['instance', config.config.instance.id], ['desired_version', config.config.desired_version],
        ['applied_version', config.config.applied_version], ['apply_state', pretty(config.config.apply_state)],
        ['delivery_id', text(config.config.delivery_id)], ['values', JSON.stringify(config.config.values)],
      ]} />}
    </section>
  </div>;
}

function OperationsPage({ api, revision }: { api: ApiClient; revision: number }) {
  const [owner, setOwner] = useState('runtime');
  const [operationId, setOperationId] = useState('');
  const [operation, setOperation] = useState<TargetOperation>();
  const [failure, setFailure] = useState<Failure>();
  const [pending, setPending] = useState(false);
  const load = useCallback(async () => {
    if (!operationId) return;
    setFailure(undefined); setPending(false);
    try { setOperation((await api.operation(owner, operationId)).operation); }
    catch (error) { setOperation(undefined); setFailure(describeError(error)); setPending(isBackendPending(error)); }
  }, [api, owner, operationId]);
  useEffect(() => { void load(); }, [load, revision]);
  useEffect(() => {
    if (!operation || ['OPERATION_SUCCEEDED', 'OPERATION_FAILED', 'OPERATION_CANCELLED', 'OPERATION_EXPIRED'].includes(operation.state)) return;
    const timer = setTimeout(() => void load(), 2000);
    return () => clearTimeout(timer);
  }, [operation, load]);
  const participants: OperationProgress[] = operation?.participants ?? [];
  return <div className="stack"><BackendBanner pending={pending} />
    <section className="table-card"><div className="table-head"><div><p className="eyebrow">LIVE · OPERATION PROGRESS · Q8</p><h2>按 owner service + operation 查询进度</h2></div></div>
      <p className="muted">异步状态区分 受理/执行/受阻/完成/失败/取消/过期；WS 断流重连后本页从 REST 快照重新加载。阻断状态带 reason/next_attempt_at。</p>
      <div className="row-actions"><label>owner service<input aria-label="owner service" value={owner} onChange={(event) => setOwner(event.target.value)} /></label>
        <label>operation ID<input aria-label="operation ID" value={operationId} onChange={(event) => setOperationId(event.target.value)} /></label>
        <button className="secondary" onClick={() => void load()} disabled={!operationId}>查询</button></div>
      {failure && <FailureNotice failure={failure} />}
      {operation && <TableCard title="操作快照" eyebrow={`${text(operation.ref.owner_service)} · ${operation.ref.operation_id}`} columns={['字段', '值']} rows={[
        ['state', pretty(operation.state)], ['execution_outcome', pretty(operation.execution_outcome)],
        ['stalled', operation.stalled ? 'yes' : 'no'], ['version', operation.version],
        ['observed_at', formatTime(operation.observed_at)], ['reason', text(operation.reason?.code)],
        ['next_attempt_at', text(operation.next_attempt_at)],
      ]} />}
      {operation && <TableCard title="参与者进度" eyebrow="PARTICIPANTS" columns={['参与者', '状态', 'reason', 'next_attempt_at']}
        rows={participants.map((item) => [item.participant, pretty(item.state), text(item.reason?.code), text(item.next_attempt_at)])} />}
    </section></div>;
}

function TasksPage({ api, revision }: { api: ApiClient; revision: number }) {
  const [taskId, setTaskId] = useState('');
  const [task, setTask] = useState<TargetTaskRecord>();
  const [failure, setFailure] = useState<Failure>();
  const [notice, setNotice] = useState('');
  const [notFound, setNotFound] = useState(false);
  const load = async () => {
    setFailure(undefined); setNotice(''); setNotFound(false);
    try { setTask((await api.targetTask(taskId)).task); }
    catch (error) {
      setTask(undefined);
      if (error instanceof ApiError && error.status === 404) { setNotFound(true); return; }
      setFailure(describeError(error));
    }
  };
  const cancel = async () => {
    if (!task) return;
    setFailure(undefined); setNotice('');
    try {
      await api.cancelTask(task.task_id, {
        task_id: task.task_id,
        scope: { resources: [{ kind: 'RESOURCE_INSTANCE', id: task.owner_instance_id }] },
      } as never);
      setNotice('取消命令已受理；任务状态以查询为准');
    } catch (error) { setFailure(describeError(error)); }
  };
  return <div className="stack"><section className="table-card"><div className="table-head"><div><p className="eyebrow">LIVE · TARGET TASK · Q8</p><h2>目标 Scheduler 任务查询与取消</h2></div></div>
    <div className="row-actions"><label>task ID<input aria-label="task ID" value={taskId} onChange={(event) => setTaskId(event.target.value)} /></label>
      <button className="secondary" onClick={() => void load()} disabled={!taskId}>查询</button>
      <button className="secondary" onClick={() => void cancel()} disabled={!task}>取消任务</button></div>
    {notFound && <p className="empty">未找到该任务（NOT_FOUND 只表示查询范围不可见，不推断任务执行失败）。</p>}
    {failure && <FailureNotice failure={failure} />}{notice && <p className="success">{notice}</p>}
    {task && <TableCard title="任务快照" eyebrow="TARGET TASK" columns={['字段', '值']} rows={[
      ['task_id', task.task_id], ['owner_instance', task.owner_instance_id], ['target', `${task.target.type}:${task.target.id}`],
      ['state', pretty(task.state)], ['run_at', text(task.run_at)], ['occurrence_id', text(task.occurrence_id)],
    ]} />}
  </section></div>;
}

function ModelsPage({ api, permissions, revision }: { api: ApiClient; permissions: string[]; revision: number }) {
  const [modelId, setModelId] = useState('');
  const [metadata, setMetadata] = useState<TargetModelMetadataResponse>();
  const [configuration, setConfiguration] = useState<TargetModelConfigurationResponse>();
  const [failure, setFailure] = useState<Failure>();
  const canManage = permissions.includes('model.manage');
  const load = async () => {
    setFailure(undefined); setConfiguration(undefined);
    try {
      setMetadata(await api.modelMetadata(modelId));
      if (canManage) setConfiguration(await api.modelConfiguration(modelId));
    } catch (error) { setMetadata(undefined); setFailure(describeError(error)); }
  };
  return <div className="stack"><section className="table-card"><div className="table-head"><div><p className="eyebrow">LIVE · MODEL READ-ONLY · Q7</p><h2>模型元数据与配置（只读）</h2></div></div>
    <p className="muted">普通读取需要 model.read；完整配置要求 model.manage。密钥永远不回显，只提示“已配置/版本”（密钥写入不在本页）。</p>
    <div className="row-actions"><label>model ID<input aria-label="model ID" value={modelId} onChange={(event) => setModelId(event.target.value)} /></label>
      <button className="secondary" onClick={() => void load()} disabled={!modelId}>读取</button></div>
    {failure && <FailureNotice failure={failure} />}
    {metadata && <TableCard title="模型元数据" eyebrow="TARGET MODEL METADATA" columns={['字段', '值']} rows={[
      ['model_id', metadata.model.model_id], ['alias', metadata.model.alias], ['provider_kind', metadata.model.provider_kind],
      ['config_version', metadata.model.config_version], ['state', pretty(metadata.model.state)],
      ['secret', '已配置（版本不回显）'],
    ]} />}
    {!canManage && <PermissionGate permissions={permissions} required="model.manage" children={<p className="empty">当前角色只有只读元数据视图，完整配置被 model.manage 门禁隐藏。</p>} />}
    {configuration && <TableCard title="模型配置（完整只读）" eyebrow="TARGET MODEL CONFIGURATION" columns={['字段', '值']} rows={[
      ['model_id', configuration.config.model_id], ['alias', configuration.config.alias],
      ['provider_kind', configuration.config.provider_kind], ['endpoint', configuration.config.endpoint],
      ['provider_model', configuration.config.provider_model], ['timeout_ms', String(configuration.config.timeout_ms)],
      ['version', configuration.config.version],
    ]} />}
  </section></div>;
}

function HealthPage({ api, revision }: { api: ApiClient; revision: number }) {
  const probes = useQuery((active) => Promise.all([api.healthLive(), api.healthReady()]).then(([live, ready]) => {
    if (!active()) return { live, ready };
    return { live, ready };
  }), [api], revision);
  const aggregate = useQuery((active) => api.health().then((value) => { if (!active()) return value; return value; }), [api], revision);
  return <div className="stack"><BackendBanner pending={aggregate.pendingBackend} />
    <TableCard title="进程探针" eyebrow="LIVE · /health/live + /health/ready" columns={['探针', '状态']} rows={probes.value
      ? [['live', probes.value.live.state], ['ready', probes.value.ready.state]] : []} />
    {aggregate.value && <TableCard title="组件健康" eyebrow="AUTHENTICATED · /api/v1/health" columns={['组件', '状态', '原因']}
      rows={aggregate.value.components.map((component) => [component.component, label('health', component.state), text(component.reason)])} />}
  </div>;
}

function OverviewPage({ api, permissions, revision }: { api: ApiClient; permissions: string[]; revision: number }) {
  const probes = useQuery((active) => Promise.all([api.healthLive(), api.healthReady()]).then(([live, ready]) => {
    if (!active()) return { live, ready };
    return { live, ready };
  }), [api], revision);
  return <div className="stack"><section className="hero"><div><p className="eyebrow">P12 LIVE · CONTROL PLANE</p><h2>目标管理面 live 接线</h2>
    <p>健康/账号/安装实例/operations/任务/模型/动作元数据按 P12a 契约发起真实请求；P12c 后端并行接入。日志与审计查询按 Q2 阶段性延期，不在本版调用。</p></div></section>
    <TableCard title="就绪探针" eyebrow="PROBES" columns={['探针', '状态']} rows={probes.value
      ? [['live', probes.value.live.state], ['ready', probes.value.ready.state]] : []} />
    <TableCard title="Q1–Q8 口径落地" eyebrow="DECISIONS" columns={['决策', '落地']} rows={[
      ['Q1 Action', 'QueryActionResult → 动作页；NOT_FOUND 显示未找到，不推断失败'],
      ['Q2 日志/审计', '阶段性延期：本版不调用 /logs /audit'],
      ['Q3 groups', '不使用旧 /groups；群白名单走 ActionPolicy'],
      ['Q4 主体', '用户页暂保留 reference；ListPrincipals 待 P12a'],
      ['Q5 健康', 'live/ready 公开探针 + 受权 /health 聚合'],
      ['Q6 安装实例', '实例 desired-state/config；不复用 /plugins enable'],
      ['Q7 模型', 'model.read 元数据，model.manage 完整配置；密钥不回显'],
      ['Q8 验收', '异步 operation 进度、WS 断流后 REST 快照恢复'],
    ]} />
    <TableCard title="当前权限" eyebrow="PERMISSION GATE" columns={['权限']} rows={permissions.map((item) => [item])} />
  </div>;
}

export function P12LiveView({ page, api, permissions, revision }: {
  page: P12PageKey; api: ApiClient; permissions: string[]; revision: number;
}) {
  switch (page) {
    case 'p12-overview': return <OverviewPage api={api} permissions={permissions} revision={revision} />;
    case 'p12-accounts': return <AccountsPage api={api} permissions={permissions} revision={revision} />;
    case 'p12-installations': return <InstallationsPage api={api} revision={revision} />;
    case 'p12-operations': return <OperationsPage api={api} revision={revision} />;
    case 'p12-tasks': return <TasksPage api={api} revision={revision} />;
    case 'p12-model-config': return <ModelsPage api={api} permissions={permissions} revision={revision} />;
    case 'p12-health': return <HealthPage api={api} revision={revision} />;
  }
}
