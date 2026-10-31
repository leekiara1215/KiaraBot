import { useCallback, useEffect, useState } from 'react';
import { type ApiClient, type Health, type Page, type Plugin, type PluginConfig, type ValidationResult } from '../api/client';
import { ActionFeedback, describeError, FailureNotice, JsonField, JsonPreview, label, StateTag, useAction, type ActionState, type Failure } from '../ui';

const manifestTemplate = JSON.stringify({
  id: 'sample.plugin',
  name: 'Sample',
  version: '1.0.0',
  api_version: 'kiarabot.plugin.v1',
  runtime: 'python-rpc',
  entrypoint: 'sample:create_plugin',
  capabilities: ['event.message.received'],
  permissions: ['messages.read'],
}, null, 2);

/** 插件管理：列表 + 启用/禁用 + 配置读写 + 健康查询 + 清单校验（只调用已就绪的 REST 端点）。 */
export function PluginsView({ api, permissions, revision }: { api: ApiClient; permissions: string[]; revision: number }) {
  const [page, setPage] = useState<Page<Plugin>>();
  const [failure, setFailure] = useState<Failure>();
  const [loading, setLoading] = useState(true);
  const [busy, setBusy] = useState('');
  const [feedback, setFeedback] = useState<Record<string, ActionState>>({});
  const [selected, setSelected] = useState<{ id: string; panel: 'config' | 'health' }>();
  const canManage = permissions.includes('plugin.manage');
  const canReadConfig = permissions.includes('plugin.config.read');
  const canWriteConfig = permissions.includes('plugin.config.write');

  useEffect(() => {
    let active = true;
    setLoading(true); setFailure(undefined);
    api.plugins().then((value) => { if (active) setPage(value); }).catch((error) => { if (active) setFailure(describeError(error)); }).finally(() => { if (active) setLoading(false); });
    return () => { active = false; };
  }, [api, revision]);

  const act = useCallback(async (key: string, op: () => Promise<unknown>, done: string) => {
    setBusy(key);
    setFeedback((current) => ({ ...current, [key]: { phase: 'running', message: '提交中…' } }));
    try {
      await op();
      setFeedback((current) => ({ ...current, [key]: { phase: 'ok', message: done } }));
    } catch (error) {
      setFeedback((current) => ({ ...current, [key]: { phase: 'error', failure: describeError(error) } }));
    } finally {
      setBusy('');
      try { setPage(await api.plugins()); } catch { /* 列表刷新失败保留上一次快照 */ }
    }
  }, [api]);

  return (
    <div className="stack">
      <section className="table-card">
        <div className="table-head">
          <div><p className="eyebrow">PLUGIN MANAGEMENT</p><h2>插件列表</h2></div>
          <span className="count">{page?.items.length ?? 0} 个插件</span>
        </div>
        {loading && <p className="empty">正在加载…</p>}
        {failure && <FailureNotice failure={failure} />}
        {!loading && page && !page.items.length && <p className="empty">暂无插件</p>}
        {page && page.items.length > 0 && (
          <div className="table-wrap">
            <table>
              <thead><tr><th>名称</th><th>标识</th><th>版本</th><th>运行时</th><th>状态</th><th>管理动作</th></tr></thead>
              <tbody>
                {page.items.map((plugin) => (
                  <tr key={plugin.id}>
                    <td>{plugin.name}</td>
                    <td>{plugin.id}</td>
                    <td>{plugin.version}</td>
                    <td>{plugin.runtime}</td>
                    <td>{label('plugin', plugin.state)}</td>
                    <td className="row-actions">
                      <button className="secondary small" aria-label={`启用 ${plugin.name}`} disabled={!canManage || busy === plugin.id} onClick={() => act(plugin.id, () => api.enablePlugin(plugin.id), '启用请求已接受（202，不代表进程已启动）')}>启用</button>
                      <button className="secondary small" aria-label={`禁用 ${plugin.name}`} disabled={!canManage || busy === plugin.id} onClick={() => act(plugin.id, () => api.disablePlugin(plugin.id), '禁用请求已接受（202，已发生的平台副作用不撤回）')}>禁用</button>
                      <button className="secondary small" aria-label={`配置 ${plugin.name}`} disabled={!canReadConfig} onClick={() => setSelected({ id: plugin.id, panel: 'config' })}>配置</button>
                      <button className="secondary small" aria-label={`健康 ${plugin.name}`} disabled={!canReadConfig} onClick={() => setSelected({ id: plugin.id, panel: 'health' })}>健康</button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
        <div className="row-feedback">
          {Object.entries(feedback).map(([key, state]) => <div key={key} className="inline-feedback"><strong>{key}</strong><ActionFeedback state={state} /></div>)}
        </div>
        {!canManage && page && <p className="hint">当前角色没有 plugin.manage，启用/禁用/校验按钮不可用；服务端仍会独立鉴权。</p>}
      </section>

      {selected?.panel === 'config' && <ConfigPanel api={api} pluginId={selected.id} canWrite={canWriteConfig} onClose={() => setSelected(undefined)} />}
      {selected?.panel === 'health' && <HealthPanel api={api} pluginId={selected.id} onClose={() => setSelected(undefined)} />}
      <ValidatePanel api={api} canManage={canManage} />
    </div>
  );
}

function ConfigPanel({ api, pluginId, canWrite, onClose }: { api: ApiClient; pluginId: string; canWrite: boolean; onClose: () => void }) {
  const [config, setConfig] = useState<PluginConfig>();
  const [draft, setDraft] = useState('');
  const [failure, setFailure] = useState<Failure>();
  const [saved, setSaved] = useState('');
  const [loading, setLoading] = useState(true);
  const load = useCallback(async () => {
    setLoading(true); setFailure(undefined); setSaved('');
    try {
      const value = await api.pluginConfig(pluginId);
      setConfig(value); setDraft(JSON.stringify(value.values, null, 2));
    } catch (error) { setFailure(describeError(error)); } finally { setLoading(false); }
  }, [api, pluginId]);
  useEffect(() => { void load(); }, [load]);

  const save = async () => {
    if (!config) return;
    setFailure(undefined); setSaved('');
    let values: Record<string, unknown>;
    try { values = JSON.parse(draft) as Record<string, unknown>; } catch { setFailure({ kind: 'invalid', title: 'JSON 无效', detail: '配置值必须是合法 JSON 对象（仅前端体验性校验）。' }); return; }
    if (!values || typeof values !== 'object' || Array.isArray(values)) { setFailure({ kind: 'invalid', title: 'JSON 无效', detail: '配置值必须是 JSON 对象。' }); return; }
    try {
      const next = await api.updatePluginConfig(pluginId, { expected_version: config.version, values });
      setConfig(next); setDraft(JSON.stringify(next.values, null, 2));
      setSaved(`已保存持久化版本 ${next.version}；PUT 只确认持久化，运行实例应用情况见下方 delivery。`);
    } catch (error) {
      const parsed = describeError(error);
      if (parsed.kind === 'conflict') setFailure({ ...parsed, title: '版本冲突', detail: 'expected_version 已过期（配置被其他请求修改）。请重新读取后再提交。' });
      else setFailure(parsed);
    }
  };

  return (
    <section className="table-card panel-card">
      <div className="table-head">
        <div><p className="eyebrow">PLUGIN CONFIG</p><h2>{pluginId} 配置</h2></div>
        <div className="row-actions"><button className="secondary small" onClick={() => void load()}>重新读取</button><button className="text-button" onClick={onClose}>关闭</button></div>
      </div>
      {loading && <p className="empty">正在读取配置…</p>}
      {failure && <FailureNotice failure={failure} />}
      {saved && <p role="status" className="feedback ok">{saved}</p>}
      {config && (
        <div className="panel-body">
          <p className="hint">持久化版本 <code>{config.version}</code>；delivery = <StateTag text={label('delivery', config.delivery?.state)} tone={config.delivery?.state === 'APPLIED' ? 'ok' : config.delivery?.state === 'REJECTED' ? 'bad' : 'warn'} />
            {config.delivery?.applied_version && <span>（applied_version={config.delivery.applied_version}）</span>}
            {config.delivery?.error && <span> error={config.delivery.error.code}</span>}
          </p>
          <JsonField id="plugin-config" label="配置值（JSON 对象）" value={draft} onChange={setDraft} disabled={!canWrite} rows={10} />
          <div className="row-actions">
            <button className="primary small" disabled={!canWrite} onClick={() => void save()}>保存（expected_version={config.version}）</button>
          </div>
          {!canWrite && <p className="hint">当前角色没有 plugin.config.write，保存按钮不可用。</p>}
        </div>
      )}
    </section>
  );
}

function HealthPanel({ api, pluginId, onClose }: { api: ApiClient; pluginId: string; onClose: () => void }) {
  const [health, setHealth] = useState<Health>();
  const [failure, setFailure] = useState<Failure>();
  const [loading, setLoading] = useState(true);
  const load = useCallback(async () => {
    setLoading(true); setFailure(undefined);
    try { setHealth(await api.pluginHealth(pluginId)); } catch (error) { setFailure(describeError(error)); } finally { setLoading(false); }
  }, [api, pluginId]);
  useEffect(() => { void load(); }, [load]);
  return (
    <section className="table-card panel-card">
      <div className="table-head">
        <div><p className="eyebrow">PLUGIN HEALTH</p><h2>{pluginId} 健康</h2></div>
        <div className="row-actions"><button className="secondary small" onClick={() => void load()}>重新探测</button><button className="text-button" onClick={onClose}>关闭</button></div>
      </div>
      {loading && <p className="empty">正在探测…</p>}
      {failure && <FailureNotice failure={failure} />}
      {health && (
        <div className="panel-body">
          <p>聚合状态：<StateTag text={label('health', health.state)} tone={health.state === 'HEALTHY' ? 'ok' : health.state === 'DEGRADED' ? 'warn' : 'bad'} /></p>
          <div className="cards">
            {health.components.map((component) => (
              <article className="card" key={component.component}>
                <span className="card-icon">◈</span>
                <div><h3>{component.component}</h3><p className="muted">{component.reason || '暂无详情'}</p></div>
                <span className="tag">{label('health', component.state)}</span>
              </article>
            ))}
          </div>
        </div>
      )}
    </section>
  );
}

function ValidatePanel({ api, canManage }: { api: ApiClient; canManage: boolean }) {
  const [draft, setDraft] = useState(manifestTemplate);
  const [result, setResult] = useState<ValidationResult>();
  const [failure, setFailure] = useState<Failure>();
  const { state, run } = useAction();
  const validate = async () => {
    setFailure(undefined); setResult(undefined);
    let manifest: unknown;
    try { manifest = JSON.parse(draft); } catch { setFailure({ kind: 'invalid', title: 'JSON 无效', detail: '清单必须是合法 JSON 对象。' }); return; }
    await run('清单校验', async () => {
      try { setResult(await api.validatePlugin(manifest as Parameters<ApiClient['validatePlugin']>[0])); return '校验完成'; }
      catch (error) { setFailure(describeError(error)); throw error; }
    });
  };
  return (
    <section className="table-card panel-card">
      <div className="table-head">
        <div><p className="eyebrow">MANIFEST VALIDATION</p><h2>校验插件清单</h2></div>
        <span className="hint">POST /plugins/validate，不启用插件（plugin.manage）</span>
      </div>
      <div className="panel-body">
        <JsonField id="plugin-manifest" label="插件清单（JSON）" value={draft} onChange={setDraft} rows={12} disabled={!canManage} />
        <div className="row-actions"><button className="primary small" disabled={!canManage} onClick={() => void validate()}>校验清单</button></div>
        <ActionFeedback state={state} />
        {failure && <FailureNotice failure={failure} />}
        {result && (
          <div className={result.valid ? 'feedback ok' : 'feedback error'} role="status">
            {result.valid ? '清单有效' : `清单无效（${result.errors.length} 个错误）`}
            {result.errors.length > 0 && <JsonPreview value={result.errors} />}
          </div>
        )}
      </div>
    </section>
  );
}
