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
import { useCallback, useEffect, useMemo, useState } from 'react';
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
  TargetModelUpdateRequest,
  TargetOperationRef,
  TargetPrincipalPage,
  TargetResourceScope,
  TargetCatalogRef,
  TargetTaskPage,
  TargetCreateInstallationRequest,
  TargetCreateInstanceRequest,
  TargetInstanceConfigurationResponse,
  TargetInstanceFactHost,
  TargetInstanceFactPlugin,
  TargetInstanceFactRuntime,
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
const isUnavailable = (error: unknown) =>
  error instanceof ApiError && (error.status === 503 || error.code === 'UPSTREAM_UNAVAILABLE');
const terminalOperationStates = new Set(['OPERATION_SUCCEEDED', 'OPERATION_FAILED', 'OPERATION_CANCELLED', 'OPERATION_EXPIRED']);
const OPERATION_STATE_TEXT: Record<string, string> = {
  OPERATION_ACCEPTED: '已受理（尚未开始）', OPERATION_RUNNING: '执行中', OPERATION_BLOCKED: '受阻',
  OPERATION_SUCCEEDED: '状态成功', OPERATION_FAILED: '执行失败', OPERATION_CANCELLED: '已取消',
  OPERATION_EXPIRED: '已过期',
};
const OUTCOME_TEXT: Record<string, string> = {
  EXECUTION_NOT_STARTED: '未开始', EXECUTION_KNOWN_SUCCEEDED: '已知成功',
  EXECUTION_KNOWN_FAILED: '已知失败', EXECUTION_UNKNOWN: '结果未知（不得推断完成）',
};
const defaultScope = (instanceId = ''): TargetResourceScope => ({
  resources: [{ kind: 'RESOURCE_INSTANCE', id: instanceId }],
});
const scopeText = (scope: TargetResourceScope) => JSON.stringify(scope, null, 2);
function parseScopeText(value: string): TargetResourceScope | undefined {
  try {
    const parsed = JSON.parse(value) as TargetResourceScope;
    return parsed && Array.isArray(parsed.resources) && parsed.resources.length > 0 ? parsed : undefined;
  } catch { return undefined; }
}
function ScopeField({ value, onChange }: { value: string; onChange: (value: string) => void }) {
  return <label>scope（JSON）
    <textarea aria-label="scope" rows={3} value={value} spellCheck={false}
      onChange={(event) => onChange(event.target.value)} />
  </label>;
}
function CatalogField({ id, onChange }: { id: string; onChange: (value: string) => void }) {
  return <label>catalog ID（RESOURCE_CATALOG）
    <input aria-label="catalog ID" value={id} onChange={(event) => onChange(event.target.value)} />
  </label>;
}
/** Poll an owner operation. HTTP 200 for the snapshot never means the work completed. */
function OperationTracker({ api, operation, scope }: {
  api: ApiClient; operation?: TargetOperationRef; scope?: TargetResourceScope;
}) {
  const [snapshot, setSnapshot] = useState<TargetOperation>();
  const [failure, setFailure] = useState<Failure>();
  const owner = operation?.owner_service;
  const operationId = operation?.operation_id;
  useEffect(() => {
    if (!owner || !operationId) { setSnapshot(undefined); return; }
    let active = true;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const poll = async () => {
      try {
        const next = (await api.operation(owner, operationId, scope)).operation;
        if (!active) return;
        setSnapshot(next); setFailure(undefined);
        if (!terminalOperationStates.has(next.state)) timer = setTimeout(() => void poll(), 2000);
      } catch (error) {
        if (!active) return;
        setFailure(describeError(error));
      }
    };
    void poll();
    return () => { active = false; if (timer) clearTimeout(timer); };
  }, [api, owner, operationId, scope]);
  if (!operation) return null;
  return <section className="table-card"><div className="table-head"><div><p className="eyebrow">ASYNC · OWNER OPERATION</p>
    <h2>已受理，轮询 {operation.owner_service}/{operation.operation_id}</h2></div></div>
    <p className="muted">写操作 HTTP 200/202 只表示已受理；此处状态来自 owner 快照，200 不等于完成。</p>
    {failure && <FailureNotice failure={failure} />}
    {snapshot && <div className="table-wrap"><table><tbody>
      <tr><th>state</th><td>{OPERATION_STATE_TEXT[snapshot.state] ?? snapshot.state}</td></tr>
      <tr><th>execution_outcome</th><td>{OUTCOME_TEXT[snapshot.execution_outcome] ?? snapshot.execution_outcome}</td></tr>
      <tr><th>stalled</th><td>{snapshot.stalled ? '是（停滞）' : '否'}</td></tr>
      <tr><th>observed_at</th><td>{formatTime(snapshot.observed_at)}</td></tr>
      <tr><th>version</th><td>{snapshot.version}</td></tr>
      <tr><th>reason</th><td>{snapshot.reason?.code ?? '—'} {snapshot.reason?.message ?? ''}</td></tr>
      <tr><th>next_attempt_at</th><td>{formatTime(snapshot.next_attempt_at)}</td></tr>
      {snapshot.execution_outcome === 'EXECUTION_UNKNOWN' && <tr><th>说明</th><td>结果未知，不得据此断言完成或失败。</td></tr>}
    </tbody></table></div>}
    {snapshot && (snapshot.participants?.length ?? 0) > 0 && <TableCard title="参与者进度" eyebrow="PARTICIPANTS"
      columns={['参与者', '状态', 'reason', 'next_attempt_at']} rows={(snapshot.participants ?? []).map((item) => [
        item.participant, OPERATION_STATE_TEXT[item.state] ?? item.state, item.reason?.code ?? '—',
        formatTime(item.next_attempt_at)])} />}
  </section>;
}
function operationFrom(value: { operation?: TargetOperationRef }): TargetOperationRef | undefined {
  return value.operation;
}

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
  const [bindingExpectedVersion, setBindingExpectedVersion] = useState('1');
  const [catalogId, setCatalogId] = useState('');
  const [operation, setOperation] = useState<TargetOperationRef>();
  const trackerScope = useMemo(() => defaultScope(), []);
  const [notice, setNotice] = useState<Failure>();
  const [success, setSuccess] = useState('');
  const accounts = useQuery<TargetAccountPage>((active) => api.accountPage(
    catalogId ? { kind: 'RESOURCE_CATALOG', id: catalogId } : undefined,
  ).then((value) => { if (!active()) return value; return value; }), [api, catalogId], revision);
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
      const reply = await api.bindAdapter(bindingAccount, {
        binding: { account_id: bindingAccount, adapter_instance_id: adapterInstance, version: bindingExpectedVersion },
        expected_version: bindingExpectedVersion,
      });
      setOperation(operationFrom(reply));
      setSuccess('绑定命令已受理；HTTP 200 不代表完成，请轮询 operation 或账号绑定状态');
    } catch (error) { setNotice(describeError(error)); }
  };
  const unbind = async () => {
    setNotice(undefined); setSuccess('');
    try {
      const reply = await api.unbindAdapter(bindingAccount, {
        binding: { account_id: bindingAccount, adapter_instance_id: adapterInstance, version: bindingExpectedVersion },
        expected_version: bindingExpectedVersion,
      });
      setOperation(operationFrom(reply));
      setSuccess('解绑命令已受理；HTTP 200 不代表完成');
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
        <CatalogField id={catalogId} onChange={setCatalogId} />
        <label>expected_version<input aria-label="binding expected_version" value={bindingExpectedVersion} onChange={(event) => setBindingExpectedVersion(event.target.value)} /></label>
      </div>
      <p className="muted">列表端点要求显式 catalog（RESOURCE_CATALOG）；缺失时后端返回 400。绑定/解绑的 expected_version 必须取自当前绑定版本，冲突返回 409。</p>
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
    <OperationTracker api={api} operation={operation} scope={trackerScope} />
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
  const [catalogId, setCatalogId] = useState('');
  const [pageToken, setPageToken] = useState<string>();
  const installations = useQuery<TargetInstallationPage>((active) => api.installationPage(
    catalogId ? { kind: 'RESOURCE_CATALOG', id: catalogId } : undefined, pageToken,
  ).then((value) => { if (!active()) return value; return value; }), [api, catalogId, pageToken], revision);
  const [pluginTypeId, setPluginTypeId] = useState('');
  const [manifestText, setManifestText] = useState('{}');
  const [installationId, setInstallationId] = useState('');
  const [hostId, setHostId] = useState('');
  const [externallyManaged, setExternallyManaged] = useState(false);
  const [instanceId, setInstanceId] = useState('');
  const [expectedVersion, setExpectedVersion] = useState('1');
  const [configText, setConfigText] = useState('{}');
  const [configuration, setConfiguration] = useState<TargetInstanceConfigurationResponse>();
  const [facts, setFacts] = useState<{
    runtime?: TargetInstanceFactRuntime; host?: TargetInstanceFactHost; plugin?: TargetInstanceFactPlugin;
  }>({});
  const [operation, setOperation] = useState<TargetOperationRef>();
  const [failure, setFailure] = useState<Failure>();
  const [notice, setNotice] = useState('');
  const trackerScope = useMemo(() => defaultScope(instanceId), [instanceId]);
  const createInstallation = async () => {
    setFailure(undefined); setNotice('');
    let manifest: Record<string, unknown>;
    try { manifest = JSON.parse(manifestText) as Record<string, unknown>; }
    catch { setFailure({ kind: 'invalid', title: 'manifest 不是合法 JSON', detail: '请提交 PluginManifest JSON 对象。' }); return; }
    try {
      const reply = await api.createInstallation({ plugin_type_id: pluginTypeId, manifest } as TargetCreateInstallationRequest);
      setNotice(`安装创建返回 ${reply.installation.installation_id}（HTTP 200 记录已创建，不代表实例已运行）；plugin.manage 与目标后端就绪后生效`);
      setInstallationId(reply.installation.installation_id);
    } catch (error) { setFailure(describeError(error)); }
  };
  const createInstance = async () => {
    setFailure(undefined); setNotice('');
    if (!installationId) { setFailure({ kind: 'invalid', title: '缺少安装 ID', detail: '实例创建路径需要安装 ID。' }); return; }
    try {
      const reply = await api.createInstance(installationId, {
        installation_id: installationId, host_id: externallyManaged ? undefined : hostId,
        externally_managed: externallyManaged,
      } as TargetCreateInstanceRequest);
      const created = reply.instance as { instance_id: string };
      setNotice(`实例创建返回 ${created.instance_id}（记录已创建，进程事实以 state 为准）`);
      setInstanceId(created.instance_id);
    } catch (error) { setFailure(describeError(error)); }
  };
  const requireInstance = () => {
    if (!installationId || !instanceId) {
      setFailure({ kind: 'invalid', title: '缺少安装或实例 ID', detail: '实例配置/plugin-state 使用显式安装+实例对象路径。' });
      return false;
    }
    return true;
  };
  const loadConfig = async () => {
    setFailure(undefined); setNotice('');
    if (!requireInstance()) return;
    try {
      const reply = await api.instanceConfiguration(installationId, instanceId);
      setConfiguration(reply);
      setFacts({ host: reply.host, plugin: reply.plugin });
      setOperation(operationFrom(reply));
      setExpectedVersion(String(reply.runtime.desired_config_version));
      setConfigText(JSON.stringify(reply.runtime.configuration ?? {}, null, 2));
    } catch (error) { setFailure(describeError(error)); setConfiguration(undefined); }
  };
  const setDesired = async (desired: 'DESIRED_RUNNING' | 'DESIRED_STOPPED') => {
    setFailure(undefined); setNotice('');
    if (!requireInstance()) return;
    try {
      const reply = await api.setInstancePluginState(installationId, instanceId, {
        installation_id: installationId, instance_id: instanceId, desired,
        expected_version: expectedVersion,
      });
      setFacts({ runtime: reply.runtime, host: reply.host, plugin: reply.plugin });
      setOperation(operationFrom(reply));
      setNotice('期望状态命令已受理；runtime 期望、host 进程事实与 plugin 确认是三类独立事实，HTTP 200/202 不代表已就绪');
    } catch (error) { setFailure(describeError(error)); }
  };
  const updateConfig = async () => {
    setFailure(undefined); setNotice('');
    if (!requireInstance()) return;
    let values: Record<string, unknown>;
    try { values = JSON.parse(configText) as Record<string, unknown>; }
    catch { setFailure({ kind: 'invalid', title: '配置值不是合法 JSON', detail: 'configuration 必须是 JSON 对象。' }); return; }
    try {
      const reply = await api.updateInstanceConfiguration(installationId, instanceId, {
        configuration: values, expected_version: expectedVersion,
      });
      setConfiguration(reply);
      setFacts({ host: reply.host, plugin: reply.plugin });
      setOperation(operationFrom(reply));
      setExpectedVersion(String(reply.runtime.desired_config_version));
      setNotice('配置更新已受理；apply_state 不是 APPLIED 时只显示处理中/未确认，不显示完成');
    } catch (error) { setFailure(describeError(error)); }
  };
  const outcomeText: Record<string, string> = {
    EXECUTION_NOT_STARTED: '未开始', EXECUTION_KNOWN_SUCCEEDED: '已确认成功',
    EXECUTION_KNOWN_FAILED: '已确认失败', EXECUTION_UNKNOWN: '结果未知（不等于已停止）',
  };
  const applyText: Record<string, string> = {
    CONFIG_APPLY_UNSPECIFIED: '未确认', CONFIG_APPLY_PENDING: '处理中（未确认）',
    CONFIG_APPLY_APPLIED: '已确认应用', CONFIG_APPLY_REJECTED: '已拒绝（未应用）',
    CONFIG_APPLY_UNAVAILABLE: '不可用（未确认）',
  };
  const factTables = (facts.runtime || facts.host || facts.plugin) && <div className="stack">
    {facts.runtime && <TableCard title="Runtime 事实（期望）" eyebrow="FACT 1 · RUNTIME DESIRED"
      columns={['字段', '值']} rows={[
        ['desired', pretty(facts.runtime.desired)],
        ['desired_config_version', String(facts.runtime.desired_config_version ?? '—')],
        ['version', facts.runtime.version],
        ['observed_at', formatTime(facts.runtime.observed_at)],
        ['说明', 'Runtime 保存的期望值，不代表 Host/插件已执行'],
      ]} />}
    {facts.host && <TableCard title="Host 事实（观察）" eyebrow="FACT 2 · HOST OBSERVED"
      columns={['字段', '值']} rows={[
        ['host_id', text(facts.host.host_id)], ['registered', facts.host.registered ? '已注册' : '未注册'],
        ['process_outcome', outcomeText[facts.host.process_outcome] ?? facts.host.process_outcome],
        ['observed_at', formatTime(facts.host.observed_at)],
        ['说明', 'UNKNOWN 不等于已停止；未注册/未知时不得显示完成'],
      ]} />}
    {facts.plugin && <TableCard title="Plugin 事实（确认）" eyebrow="FACT 3 · PLUGIN ACK"
      columns={['字段', '值']} rows={[
        ['applied_config_version', String(facts.plugin.applied_config_version ?? '—')],
        ['apply_state', `${pretty(facts.plugin.apply_state)} · ${applyText[facts.plugin.apply_state] ?? '未确认'}`],
        ['observed_at', formatTime(facts.plugin.observed_at)],
        ['说明', facts.plugin.apply_state === 'CONFIG_APPLY_APPLIED'
          ? '仅 APPLIED 显示已确认完成' : '非 APPLIED：处理中/未确认，不显示完成'],
      ]} />}
  </div>;
  return <div className="stack">
    <BackendBanner pending={installations.pendingBackend} />
    {installations.failure && !installations.pendingBackend && <FailureNotice failure={installations.failure} />}
    <section className="table-card"><div className="table-head"><div><p className="eyebrow">LIVE · CATALOG LIST</p><h2>插件安装（目标 Runtime）</h2></div>
      <span className="count">{installations.value?.installations.length ?? 0} 条</span></div>
      <div className="row-actions"><CatalogField id={catalogId} onChange={(value) => { setCatalogId(value); setPageToken(undefined); }} /></div>
      <p className="muted">ListInstallations 要求显式 catalog（RESOURCE_CATALOG）；缺少 catalog 时后端返回 400。实例 plugin-state/configuration 使用 P12c-5 对象路径；旧 /plugins enable|config 不复用（未冻结），保持待 P12a。</p>
      <TableCard title="安装列表" eyebrow="LIVE · LIST INSTALLATIONS" columns={['安装 ID', '插件类型', '版本', '生命周期']}
        rows={(installations.value?.installations ?? []).map((item) => [item.installation_id, item.plugin_type_id, item.version, pretty(item.state)])} />
      <div className="pagination">{pageToken && <button className="secondary" onClick={() => setPageToken(undefined)}>返回首页</button>}
        {installations.value?.snapshot?.page?.next_page_token && <button className="secondary" onClick={() => setPageToken(installations.value?.snapshot?.page?.next_page_token)}>下一页 →</button>}</div>
    </section>

    <section className="table-card"><div className="table-head"><div><p className="eyebrow">CREATE · INSTALLATION / INSTANCE</p><h2>安装与实例创建</h2></div></div>
      <p className="muted">写操作返回 200/202 只是已受理/已记录；是否进入运行由 desired-state、进程事实与 operation 状态决定。成功响应中的 bootstrap_credential 由 Control 移除，Panel 不展示密钥。</p>
      <div className="row-actions">
        <label>plugin type ID<input aria-label="plugin type ID" value={pluginTypeId} onChange={(event) => setPluginTypeId(event.target.value)} /></label>
        <label>manifest（JSON）<textarea aria-label="plugin manifest" rows={3} value={manifestText} spellCheck={false} onChange={(event) => setManifestText(event.target.value)} /></label>
        <button className="secondary" onClick={() => void createInstallation()} disabled={!pluginTypeId}>创建安装</button>
      </div>
      <div className="row-actions">
        <label>安装 ID<input aria-label="创建实例的安装 ID" value={installationId} onChange={(event) => setInstallationId(event.target.value)} /></label>
        <label>host ID<input aria-label="host ID" value={hostId} onChange={(event) => setHostId(event.target.value)} disabled={externallyManaged} /></label>
        <label><input type="checkbox" aria-label="外部管理实例" checked={externallyManaged} onChange={(event) => setExternallyManaged(event.target.checked)} />外部管理实例</label>
        <button className="secondary" onClick={() => void createInstance()} disabled={!installationId || (!externallyManaged && !hostId)}>创建实例</button>
      </div>
      {notice && <p className="success">{notice}</p>}{failure && <FailureNotice failure={failure} />}
    </section>

    <section className="table-card"><div className="table-head"><div><p className="eyebrow">INSTANCE FACTS · P12c-5</p><h2>实例 plugin-state 与 configuration（三类事实）</h2></div></div>
      <p className="muted">对象路径为 /installations/&lt;安装&gt;/instances/&lt;实例&gt;；Runtime 期望（runtime）、Host 观察（host）、Plugin 确认（plugin）是三类独立事实，非 APPLIED 只显示处理中/未确认，不能显示完成。configuration 的值由服务端脱敏，秘密永不回显；版本为十进制字符串。写操作返回 200/202 只是受理，必须轮询 operation。</p>
      <div className="row-actions">
        <label>安装 ID<input aria-label="实例所属安装 ID" value={installationId} onChange={(event) => setInstallationId(event.target.value)} /></label>
        <label>实例 ID<input aria-label="实例 ID" value={instanceId} onChange={(event) => setInstanceId(event.target.value)} /></label>
        <label>expected_version<input aria-label="instance expected_version" value={expectedVersion} onChange={(event) => setExpectedVersion(event.target.value)} /></label>
        <button className="secondary" onClick={() => void loadConfig()} disabled={!installationId || !instanceId}>读取 configuration</button>
        <button className="secondary" onClick={() => void setDesired('DESIRED_RUNNING')} disabled={!installationId || !instanceId}>设为运行</button>
        <button className="secondary" onClick={() => void setDesired('DESIRED_STOPPED')} disabled={!installationId || !instanceId}>设为停止</button>
      </div>
      {notice && <p className="success">{notice}</p>}{failure && <FailureNotice failure={failure} />}
      {configuration && <TableCard title="Runtime configuration（只读脱敏）" eyebrow="GET CONFIGURATION · NO SECRETS"
        columns={['字段', '值']} rows={[
          ['desired_config_version', String(configuration.runtime.desired_config_version)],
          ['delivery_id', text(configuration.runtime.delivery_id)],
          ['apply_state', pretty(configuration.runtime.apply_state)],
          ['configuration（脱敏）', JSON.stringify(configuration.runtime.configuration ?? {}, null, 2)],
          ['observed_at', formatTime(configuration.runtime.observed_at)],
          ['说明', '密文/秘密不返回；版本变化不等于 Host/plugin 已应用'],
        ]} />}
      {factTables}
      <div className="row-actions"><label>configuration（JSON 对象）<textarea aria-label="实例配置值" rows={4} value={configText} spellCheck={false} onChange={(event) => setConfigText(event.target.value)} /></label>
        <button className="secondary" onClick={() => void updateConfig()} disabled={!installationId || !instanceId}>更新 configuration</button></div>
    </section>
    <OperationTracker api={api} operation={operation} scope={trackerScope} />
  </div>;
}

function OperationsPage({ api, revision }: { api: ApiClient; revision: number }) {
  const [owner, setOwner] = useState<'runtime' | 'scheduler' | 'model'>('runtime');
  const [operationId, setOperationId] = useState('');
  const [scope, setScope] = useState(() => scopeText(defaultScope()));
  const [operation, setOperation] = useState<TargetOperationRef>();
  const [failure, setFailure] = useState<Failure>();
  const [notFound, setNotFound] = useState(false);
  const [pending, setPending] = useState(false);
  const parsedScope = useMemo(() => parseScopeText(scope), [scope]);
  const load = useCallback(async () => {
    if (!parsedScope) { setFailure({ kind: 'invalid', title: 'scope 无效', detail: '为 owner 提供精确 scope JSON。' }); return; }
    setFailure(undefined); setNotFound(false); setPending(false);
    try {
      const snapshot = (await api.operation(owner, operationId, parsedScope)).operation;
      setOperation(snapshot.ref);
    } catch (error) {
      setOperation(undefined);
      if (error instanceof ApiError && error.status === 404) { setNotFound(true); return; }
      setPending(isBackendPending(error));
      setFailure(describeError(error));
    }
  }, [api, owner, operationId, parsedScope, revision]);
  return <div className="stack"><BackendBanner pending={pending} />
    <section className="table-card"><div className="table-head"><div><p className="eyebrow">LIVE · OPERATION PROGRESS · Q8</p><h2>按 owner service + operation 查询进度</h2></div></div>
      <p className="muted">GET 200 只表示快照读取成功，<strong>不是工作完成</strong>；完成必须由 state=SUCCEEDED 且 execution_outcome 不是 UNKNOWN。轮询不依据旧 WS 流推断，WS 断流后会重新拉取 REST 快照。</p>
      <div className="row-actions">
        <label>owner service<select aria-label="owner service" value={owner} onChange={(event) => setOwner(event.target.value as 'runtime' | 'scheduler' | 'model')}>
          <option value="runtime">runtime</option><option value="scheduler">scheduler</option><option value="model">model</option></select></label>
        <label>operation ID<input aria-label="operation ID" value={operationId} onChange={(event) => setOperationId(event.target.value)} /></label>
        <ScopeField value={scope} onChange={setScope} />
        <button className="secondary" onClick={() => void load()} disabled={!operationId || !parsedScope}>查询</button>
      </div>
      {notFound && <p className="empty">owner 返回 NOT_FOUND：操作不存在或当前 scope 不可见；不要据此推断执行失败。</p>}
      {failure && <FailureNotice failure={failure} />}
    </section>
    <OperationTracker api={api} operation={operation} scope={parsedScope} />
  </div>;
}

function TasksPage({ api, revision }: { api: ApiClient; revision: number }) {
  const [scope, setScope] = useState(() => scopeText(defaultScope()));
  const [listToken, setListToken] = useState<string>();
  const [list, setList] = useState<TargetTaskPage>();
  const [listFailure, setListFailure] = useState<Failure>();
  const [taskId, setTaskId] = useState('');
  const [task, setTask] = useState<TargetTaskRecord>();
  const [failure, setFailure] = useState<Failure>();
  const [notice, setNotice] = useState('');
  const [notFound, setNotFound] = useState(false);
  const parsedScope = useMemo(() => parseScopeText(scope), [scope]);
  const loadList = useCallback(async () => {
    if (!parsedScope) { setListFailure({ kind: 'invalid', title: 'scope 无效', detail: '任务分页查询要求显式 scope JSON。' }); return; }
    setListFailure(undefined);
    try { setList(await api.taskPage(parsedScope, listToken)); }
    catch (error) { setList(undefined); setListFailure(describeError(error)); }
  }, [api, parsedScope, listToken, revision]);
  useEffect(() => { void loadList(); }, [loadList]);
  const load = async (id = taskId) => {
    setFailure(undefined); setNotice(''); setNotFound(false);
    if (!parsedScope) { setFailure({ kind: 'invalid', title: 'scope 无效', detail: '任务查询要求显式 scope JSON。' }); return; }
    try { setTask((await api.targetTask(id, parsedScope)).task); }
    catch (error) {
      setTask(undefined);
      if (error instanceof ApiError && error.status === 404) { setNotFound(true); return; }
      setFailure(describeError(error));
    }
  };
  const cancel = async () => {
    if (!task || !parsedScope) return;
    setFailure(undefined); setNotice('');
    try {
      await api.cancelTask(task.task_id, {
        task_id: task.task_id,
        scope: parsedScope,
      } as never, parsedScope);
      setNotice('取消命令已受理；HTTP 200 不代表任务已停止，请重新查询 state 或 occurrence');
    } catch (error) { setFailure(describeError(error)); }
  };
  return <div className="stack">
    <section className="table-card"><div className="table-head"><div><p className="eyebrow">LIVE · TASK PAGE</p><h2>任务列表（items / page）</h2></div></div>
      <p className="muted">列表端点返回 TaskPage 的 items/page；分页令牌与 scope 原样转发，不把 HTTP 200 或列表 200 当成任务完成。</p>
      <div className="row-actions"><ScopeField value={scope} onChange={(value) => { setScope(value); setListToken(undefined); }} />
        <button className="secondary" onClick={() => void loadList()} disabled={!parsedScope}>刷新列表</button></div>
      {listFailure && <FailureNotice failure={listFailure} />}
      <div className="table-wrap"><table><thead><tr><th>task ID</th><th>名称</th><th>状态</th><th>created_at</th></tr></thead><tbody>
        {(list?.items ?? []).map((item) => <tr key={item.id} className="clickable" onClick={() => { setTaskId(item.id); void load(item.id); }}>
          <td>{item.id}</td><td>{text(item.name)}</td><td>{text(item.state)}</td><td>{formatTime(item.created_at)}</td></tr>)}
      </tbody></table></div>
      {(!list || list.items.length === 0) && <p className="empty">暂无任务；查询失败时不会显示为空数据。</p>}
      <div className="pagination">{listToken && <button className="secondary" onClick={() => setListToken(undefined)}>返回首页</button>}
        {list?.page?.next_page_token && <button className="secondary" onClick={() => setListToken(list?.page?.next_page_token)}>下一页 →</button>}</div>
    </section>
    <section className="table-card"><div className="table-head"><div><p className="eyebrow">LIVE · TARGET TASK · Q8</p><h2>任务查询与取消</h2></div></div>
      <div className="row-actions"><label>task ID<input aria-label="task ID" value={taskId} onChange={(event) => setTaskId(event.target.value)} /></label>
        <button className="secondary" onClick={() => void load()} disabled={!taskId || !parsedScope}>查询</button>
        <button className="secondary" onClick={() => void cancel()} disabled={!task || !parsedScope}>取消任务</button></div>
      {notFound && <p className="empty">未找到该任务（NOT_FOUND 只表示当前 scope 不可见，不推断任务执行失败）。</p>}
      {failure && <FailureNotice failure={failure} />}{notice && <p className="success">{notice}</p>}
      {task && <TableCard title="任务快照" eyebrow="TARGET TASK" columns={['字段', '值']} rows={[
        ['task_id', task.task_id], ['owner_instance', task.owner_instance_id], ['target', `${task.target.type}:${task.target.id}`],
        ['state', pretty(task.state)], ['run_at', text(task.run_at)], ['occurrence_id', text(task.occurrence_id)],
        ['version', task.version],
      ]} />}
    </section>
  </div>;
}

function ModelsPage({ api, permissions, revision }: { api: ApiClient; permissions: string[]; revision: number }) {
  const [scope, setScope] = useState(() => scopeText(defaultScope()));
  const parsedScope = useMemo(() => parseScopeText(scope), [scope]);
  const [modelId, setModelId] = useState('');
  const [metadata, setMetadata] = useState<TargetModelMetadataResponse>();
  const [configuration, setConfiguration] = useState<TargetModelConfigurationResponse>();
  const [failure, setFailure] = useState<Failure>();
  const [notice, setNotice] = useState('');
  const [operation, setOperation] = useState<TargetOperationRef>();
  const [form, setForm] = useState({ alias: '', provider_kind: '', endpoint: '', provider_model: '',
    timeout_ms: 0, version: '' });
  const canManage = permissions.includes('model.manage');
  const listed = useQuery((active) => api.models().then((value) => { if (!active()) return value; return value; }), [api], revision);
  const load = async () => {
    setFailure(undefined); setNotice(''); setConfiguration(undefined); setOperation(undefined);
    if (!parsedScope) { setFailure({ kind: 'invalid', title: 'scope 无效', detail: '模型读取需要精确 scope JSON。' }); return; }
    try {
      const model = await api.modelMetadata(modelId, parsedScope);
      setMetadata(model);
      if (canManage) {
        const config = await api.modelConfiguration(modelId, parsedScope);
        setConfiguration(config);
        setForm({ ...config.config });
      }
    } catch (error) { setMetadata(undefined); setConfiguration(undefined); setFailure(describeError(error)); }
  };
  const save = async () => {
    if (!configuration) return;
    setFailure(undefined); setNotice('');
    try {
      const reply = await api.updateModel(modelId, {
        config: { ...configuration.config, ...form },
        expected_version: configuration.config.version,
      } as TargetModelUpdateRequest);
      setOperation(operationFrom(reply));
      setNotice('模型更新已受理；HTTP 200/202 不代表新配置已生效，请轮询 operation；版本冲突返回 409');
      setMetadata({ model: reply.model });
    } catch (error) { setFailure(describeError(error)); }
  };
  return <div className="stack"><section className="table-card"><div className="table-head"><div><p className="eyebrow">LIVE · MODEL MANAGEMENT · Q7</p><h2>模型列表与只读配置</h2></div></div>
    <p className="muted">列表来自 /models 只读投影；元数据需 model.read，完整配置与更新需 model.manage。密钥永不回显，只显示已配置与版本。</p>
    <div className="row-actions"><ScopeField value={scope} onChange={setScope} />
      <label>model ID<input aria-label="model ID" value={modelId} onChange={(event) => setModelId(event.target.value)} /></label>
      <button className="secondary" onClick={() => void load()} disabled={!modelId || !parsedScope}>读取</button></div>
    {listed.failure && <FailureNotice failure={listed.failure} />}
    <div className="table-wrap"><table><thead><tr><th>model ID</th><th>名称</th><th>状态</th></tr></thead><tbody>
      {(listed.value?.items ?? []).map((item) => <tr key={item.id} className="clickable" onClick={() => setModelId(item.id)}>
        <td>{item.id}</td><td>{text(item.name)}</td><td>{text(item.state)}</td></tr>)}
    </tbody></table></div>
    {failure && <FailureNotice failure={failure} />}{notice && <p className="success">{notice}</p>}
    {metadata && <TableCard title="模型元数据" eyebrow="TARGET MODEL METADATA" columns={['字段', '值']} rows={[
      ['model_id', metadata.model.model_id], ['alias', metadata.model.alias], ['provider_kind', metadata.model.provider_kind],
      ['config_version', metadata.model.config_version], ['state', pretty(metadata.model.state)],
    ]} />}
    {!canManage && <PermissionGate permissions={permissions} required="model.manage" children={<p className="empty">当前角色只有只读元数据视图，完整配置/更新被 model.manage 门禁隐藏。</p>} />}
    {configuration && <section className="table-card"><div className="table-head"><div><p className="eyebrow">CONFIGURATION · NO CREDENTIALS</p><h2>模型配置与密钥状态</h2></div></div>
      <p className="muted">secret_configured={String(configuration.secret_configured)}；密钥内容与版本不进入 Panel，轮换不在此页提交。Disable/Enable 端点当前未在契约声明，保持待 P12a，不自造端点。</p>
      <div className="table-wrap"><table><tbody>
        <tr><th>model_id</th><td>{configuration.config.model_id}</td></tr>
        <tr><th>alias</th><td><input aria-label="模型 alias" value={form.alias} onChange={(event) => setForm({ ...form, alias: event.target.value })} /></td></tr>
        <tr><th>provider_kind</th><td><input aria-label="模型 provider_kind" value={form.provider_kind} onChange={(event) => setForm({ ...form, provider_kind: event.target.value })} /></td></tr>
        <tr><th>endpoint</th><td><input aria-label="模型 endpoint" value={form.endpoint} onChange={(event) => setForm({ ...form, endpoint: event.target.value })} /></td></tr>
        <tr><th>provider_model</th><td><input aria-label="模型 provider_model" value={form.provider_model} onChange={(event) => setForm({ ...form, provider_model: event.target.value })} /></td></tr>
        <tr><th>timeout_ms</th><td><input aria-label="模型 timeout_ms" type="number" value={form.timeout_ms} onChange={(event) => setForm({ ...form, timeout_ms: Number(event.target.value) })} /></td></tr>
        <tr><th>version</th><td>{configuration.config.version}</td></tr>
        <tr><th>observed_at</th><td>{formatTime(configuration.observed_at)}</td></tr>
      </tbody></table></div>
      <button className="primary" onClick={() => void save()} disabled={!canManage}>保存配置（expected_version={configuration.config.version}）</button>
    </section>}
    <OperationTracker api={api} operation={operation} scope={parsedScope} />
  </section></div>;
}

const expectedDependencies = ['identity', 'runtime', 'scheduler', 'model'] as const;

/** Aggregate health is 200+DEGRADED for single dependency loss and 503 when Identity itself is unhealthy. */
function HealthBoard({ api, revision }: { api: ApiClient; revision: number }) {
  const [aggregate, setAggregate] = useState<Failure>();
  const [health, setHealth] = useState<Awaited<ReturnType<ApiClient['health']>>>();
  const [identityUnavailable, setIdentityUnavailable] = useState(false);
  const [healthConsent, setHealthConsent] = useState<Failure>();
  const [unimplemented, setUnimplemented] = useState(false);
  const [probes, setProbes] = useState<{ live?: string; ready?: string; readyFailure?: Failure }>();
  useEffect(() => {
    let active = true;
    setHealth(undefined); setAggregate(undefined); setIdentityUnavailable(false);
    setHealthConsent(undefined); setUnimplemented(false);
    api.health().then((value) => { if (active) setHealth(value); }).catch((error: unknown) => {
      if (!active) return;
      const failure = describeError(error);
      if (failure.status === 401 || failure.status === 403) { setHealthConsent(failure); return; }
      if (error instanceof ApiError && error.status === 503 &&
          (error.code === 'CAPABILITY_NOT_SUPPORTED' || error.code === 'UNIMPLEMENTED')) {
        setUnimplemented(true); return;
      }
      if (isUnavailable(error)) { setIdentityUnavailable(true); return; }
      setAggregate(failure);
    });
    Promise.allSettled([api.healthLive(), api.healthReady()]).then(([live, ready]) => {
      if (!active) return;
      setProbes({
        live: live.status === 'fulfilled' ? live.value.state : undefined,
        ready: ready.status === 'fulfilled' ? ready.value.state : undefined,
        readyFailure: ready.status === 'rejected' ? describeError(ready.reason) : undefined,
      });
    });
    return () => { active = false; };
  }, [api, revision]);
  const components = new Map((health?.components ?? []).map((item) => [item.component.toLowerCase(), item]));
  return <div className="stack">
    {healthConsent && <div role="alert" className="failure forbidden">
      <strong>未授权：/api/v1/health 需要 health.read</strong>
      <p>{healthConsent.title}。这是权限/会话问题，不代表服务不健康；匿名 /health/live 与 /health/ready 探针仍可单独查看。</p>
    </div>}
    {unimplemented && <div role="alert" className="failure unavailable">
      <strong>健康聚合暂未实现（HTTP 503 CAPABILITY_NOT_SUPPORTED）</strong>
      <p>受权 /api/v1/health 仍未实现或该能力不可用；匿名 live/ready 探针继续显示。不能把它当作 Identity UNHEALTHY，也不能伪造四个依赖状态。</p>
    </div>}
    {identityUnavailable && <div role="alert" className="failure unavailable">
      <strong>Identity 未就绪：受权健康聚合返回 503</strong>
      <p>健康聚合的 503 表示 Identity/凭证链异常；不得显示为空依赖或正常状态。Runtime/Scheduler/Model 当前状态未知，需待 Identity 恢复后重新聚合。</p>
    </div>}
    {aggregate && <FailureNotice failure={aggregate} />}
    {health && <section className="hero"><div><p className="eyebrow">CONTROL AGGREGATE · {formatTime(health.components.map((item) => item.checked_at).filter(Boolean).sort().at(-1))}</p>
      <h2>{health.state === 'HEALTHY' ? '四依赖正常' : health.state === 'DEGRADED' ? '部分依赖降级（HTTP 200）' : 'Identity 异常（HTTP 503）'}</h2>
      <p>{health.state === 'DEGRADED'
        ? '单个 Runtime/Scheduler/Model 故障为 DEGRADED（HTTP 200）；相关读取会返回 UPSTREAM_UNAVAILABLE，不伪造空数据。'
        : health.state === 'HEALTHY' ? 'identity、runtime、scheduler、model 四个直接依赖均由 Control 观测。'
        : 'Identity/凭证链/撤销失败为 UNHEALTHY（HTTP 503）。'}</p></div></section>}
    <TableCard title="四个直接依赖" eyebrow="DEGRADED vs 503" columns={['依赖', '状态', '原因', 'observed_at']}
      rows={expectedDependencies.map((name) => {
        const item = components.get(name);
        return [name, item ? label('health', item.state) : '未上报（不能推断正常）', text(item?.reason),
          item?.checked_at ? formatTime(item.checked_at) : '—'];
      })} />
    <TableCard title="进程探针" eyebrow="LIVE · /health/live + /health/ready" columns={['探针', '状态', '说明']} rows={[
      ['live', probes?.live ?? (identityUnavailable ? '未知' : '加载中…'), '进程存活'],
      ['ready', probes?.ready ?? (probes?.readyFailure ? '未就绪/401/503' : '加载中…'),
        probes?.readyFailure ? `${probes.readyFailure.title}：${probes.readyFailure.detail}` : 'Identity 未就绪时 ready 端点返回 503'],
    ]} />
    {health?.state === 'DEGRADED' && <p className="empty">DEGRADED 下对应依赖的读取必须显示 UPSTREAM_UNAVAILABLE，不能因为 HTTP 200 聚合而展示为空数据。</p>}
  </div>;
}

function HealthPage({ api, revision }: { api: ApiClient; revision: number }) {
  return <HealthBoard api={api} revision={revision} />;
}

function OverviewPage({ api, permissions, revision }: { api: ApiClient; permissions: string[]; revision: number }) {
  return <div className="stack"><section className="hero"><div><p className="eyebrow">P12 LIVE · CONTROL PLANE</p><h2>目标管理面 live 接线</h2>
    <p>健康/账号/安装实例/operations/任务/模型/动作元数据按 P12a 契约发起真实请求；P12c 后端已提供 owner 投影。HTTP 200 只代表读取或受理，完成状态以 owner operation/记录为准。</p></div></section>
    <HealthBoard api={api} revision={revision} />
    <TableCard title="Q1–Q8 口径落地" eyebrow="DECISIONS" columns={['决策', '落地']} rows={[
      ['Q1 Action', 'QueryActionResult → 动作页；NOT_FOUND 显示未找到，不推断失败'],
      ['Q2 日志/审计', 'P12e 未合入前 503 显示「数据源暂不可用」；合入后按 occurred_at+ID 分页令牌消费'],
      ['Q3 groups', '不使用旧 /groups；群白名单走 ActionPolicy'],
      ['Q4 主体', 'ListPrincipals 人类主体目录已接线（权限/503 显式）'],
      ['Q5 健康', 'live/ready 探针 + 四依赖聚合；DEGRADED(200) 与 Identity 异常(503) 区分'],
      ['Q6 安装实例', '实例 plugin-state/configuration 三类事实；创建安装/实例；不复用 /plugins enable'],
      ['Q7 模型', 'model.read 元数据，model.manage 配置/更新；密钥只显示已配置与版本'],
      ['Q8 验收', '异步 operation 轮询；WS 断流后 REST 快照重拉，不当作最新'],
    ]} />
    <TableCard title="当前权限" eyebrow="PERMISSION GATE" columns={['权限']} rows={permissions.map((item) => [item])} />
  </div>;
}

export function PrincipalsDirectory({ api, revision }: { api: ApiClient; revision: number }) {
  const [page, setPage] = useState<TargetPrincipalPage>();
  const [failure, setFailure] = useState<Failure>();
  const [unavailable, setUnavailable] = useState(false);
  const [token, setToken] = useState<string>();
  useEffect(() => {
    let active = true; setFailure(undefined); setUnavailable(false);
    api.principalPage(token, 20).then((value) => { if (active) setPage(value); }).catch((error: unknown) => {
      if (!active) return;
      setPage(undefined);
      if (isUnavailable(error)) { setUnavailable(true); return; }
      setFailure(describeError(error));
    });
    return () => { active = false; };
  }, [api, revision, token]);
  const roles: Record<string, string> = { ROLE_ADMIN: '管理员', ROLE_OPERATOR: '操作员', ROLE_VIEWER: '只读' };
  return <section className="table-card">
    <div className="table-head"><div><p className="eyebrow">IDENTITY · LIST PRINCIPALS</p><h2>人类主体目录</h2></div>
      <span className="count">{page?.principals.length ?? 0} 条</span></div>
    <p className="muted">Identity 快照只列人类主体（服务/Host/插件实例排除）；需 identity.principal.read。401 清空会话，403 显示权限不足，503 显示数据源暂不可用而不是空目录。</p>
    {unavailable && <div role="alert" className="failure unavailable"><strong>主体目录数据源暂不可用（503）</strong>
      <p>Identity 未就绪或 principal 查询不可用；不能把该状态显示为空目录。</p></div>}
    {failure && <FailureNotice failure={failure} />}
    {page && (page.principals.length === 0 ? <p className="empty">当前 scope 下没有可见主体</p> : <div className="table-wrap"><table>
      <thead><tr><th>principal ID</th><th>角色</th><th>状态</th><th>版本</th></tr></thead><tbody>
      {page.principals.map((item) => <tr key={item.principal_id}><td>{item.principal_id}</td>
        <td>{item.roles?.map((role) => roles[role] ?? role).join('、') || '—'}</td>
        <td>{pretty(item.state)}</td><td>{item.version}</td></tr>)}
    </tbody></table></div>)}
    <div className="pagination"><span className="muted">checked_at {formatTime(page?.checked_at)}</span>
      {token && <button className="secondary" onClick={() => setToken(undefined)}>返回首页</button>}
      {page?.page?.next_page_token && <button className="secondary" onClick={() => setToken(page?.page?.next_page_token)}>下一页 →</button>}
    </div>
  </section>;
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
