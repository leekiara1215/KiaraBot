import { useCallback, useEffect, useState } from 'react';
import { ApiClient, ApiError, type Health, type Page as ApiPage, type TokenPair, type Schemas } from './api/client';
import { ControlStream, type SocketFactory, type StreamStatus } from './stream/control-stream';
import './styles.css';

type Page = 'overview' | 'bots' | 'adapters' | 'plugins' | 'models' | 'permissions' | 'tasks' | 'logs' | 'audit';
type Row = { id: string; cells: string[] };
type ViewData = { columns: string[]; rows: Row[]; next?: string };
const nav: { key: Page; label: string; icon: string }[] = [
  { key: 'overview', label: '总览', icon: '◫' }, { key: 'bots', label: '机器人', icon: '◉' }, { key: 'adapters', label: '适配器', icon: '⌁' },
  { key: 'plugins', label: '插件', icon: '◇' }, { key: 'models', label: '模型', icon: '✦' }, { key: 'permissions', label: '权限', icon: '♙' },
  { key: 'tasks', label: '任务', icon: '◷' }, { key: 'logs', label: '日志', icon: '≡' }, { key: 'audit', label: '审计', icon: '▤' },
];
const statusText: Record<StreamStatus, string> = { connecting: '正在连接', connected: '实时连接正常', disconnected: '连接已断开，正在重试', resync: '正在重新同步' };
const text = (value: string | undefined) => value || '—';
const joined = (items: string[]) => items.length ? items.join('、') : '—';
function pageRows<T>(page: ApiPage<T>, columns: string[], row: (item: T) => Row): ViewData { return { columns, rows: page.items.map(row), next: page.page.next_page_token }; }
async function allPages<T>(load: (token?: string) => Promise<ApiPage<T>>): Promise<T[]> {
  const items: T[] = []; const seen = new Set<string>(); let token: string | undefined;
  do {
    const page = await load(token); items.push(...page.items); token = page.page.next_page_token;
    if (token && seen.has(token)) throw new Error('分页令牌重复，无法继续读取');
    if (token) seen.add(token);
  } while (token);
  return items;
}

async function loadView(api: ApiClient, page: Exclude<Page, 'overview'>, token?: string): Promise<ViewData> {
  switch (page) {
    case 'bots': return pageRows(await api.bots(token), ['名称', '标识', '状态'], (x) => ({ id: x.id, cells: [x.name, x.id, text(x.state)] }));
    case 'adapters': return pageRows(await api.adapters(token), ['标识', '实现', '状态', '能力'], (x) => ({ id: x.id, cells: [x.id, x.implementation, x.state, joined(x.capabilities ?? [])] }));
    case 'plugins': return pageRows(await api.plugins(token), ['名称', '版本', '运行时', '状态'], (x) => ({ id: x.id, cells: [x.name, x.version, x.runtime, x.state] }));
    case 'models': return pageRows(await api.models(token), ['名称', '标识', '状态'], (x) => ({ id: x.id, cells: [x.name, x.id, text(x.state)] }));
    case 'tasks': return pageRows(await api.tasks(token), ['名称', '标识', '状态'], (x) => ({ id: x.id, cells: [x.name, x.id, text(x.state)] }));
    case 'logs': return pageRows(await api.logs(token), ['时间', '级别', '组件', '消息'], (x) => ({ id: x.id, cells: [formatTime(x.occurred_at), x.level, x.component, x.message] }));
    case 'audit': return pageRows(await api.audit(token), ['时间', '操作者', '操作', '资源'], (x) => ({ id: x.id, cells: [formatTime(x.occurred_at), x.actor, x.action, text(x.resource)] }));
    case 'permissions': {
      const [users, groups] = await Promise.all([allPages((next) => api.users(next)), allPages((next) => api.groups(next))]);
      return { columns: ['类型', '名称', '角色', '有效权限', '所属群组'], rows: [...users.map((x) => ({ id: `u-${x.id}`, cells: ['用户', x.name, joined(x.roles), joined(x.permissions), joined(x.groups)] })), ...groups.map((x) => ({ id: `g-${x.id}`, cells: ['群组', x.name, joined(x.roles), joined(x.permissions), '—'] }))] };
    }
  }
}
function formatTime(value: string) { const date = new Date(value); return Number.isNaN(date.valueOf()) ? value : date.toLocaleString('zh-CN'); }
function errorText(error: unknown) { return error instanceof ApiError && error.status === 403 ? `权限不足，无法访问此资源（${error.code}）` : error instanceof Error ? error.message : '请求失败，请重试'; }

export function App({ api, socketFactory, streamUrl = '/ws/control', mock = false }: { api: ApiClient; socketFactory: SocketFactory; streamUrl?: string; mock?: boolean }) {
  const [tokens, setTokens] = useState<TokenPair>(); const [page, setPage] = useState<Page>('overview');
  const [authError, setAuthError] = useState('');
  const [principal, setPrincipal] = useState<Schemas['Principal']>();
  const [streamReady, setStreamReady] = useState(false);
  const expire = useCallback(() => { api.setToken(); setTokens(undefined); setPrincipal(undefined); setStreamReady(false); setAuthError('会话已失效，请重新登录'); }, [api]);
  useEffect(() => { api.setUnauthorizedHandler(expire); return () => api.setUnauthorizedHandler(); }, [api, expire]);
  useEffect(() => {
    if (!tokens) return;
    let active = true;
    api.principal().then((value) => { if (active) setPrincipal(value); }).catch(() => { if (active) expire(); });
    return () => { active = false; };
  }, [api, tokens, expire]);
  const [status, setStatus] = useState<StreamStatus>('disconnected'); const [revision, setRevision] = useState(0);
  useEffect(() => {
    if (!tokens) return;
    const stream = new ControlStream({ url: streamUrl, accessToken: tokens.access_token, factory: socketFactory, onStatus: (value) => { setStatus(value); if (value === 'connected') setStreamReady(true); }, onUnauthorized: expire, onRefresh: () => setRevision((n) => n + 1) });
    stream.start(); return () => stream.stop();
  }, [tokens, socketFactory, streamUrl, expire]);
  useEffect(() => {
    if (!tokens) return;
    let active = true;
    const timer = setTimeout(() => { api.refresh(tokens.refresh_token).then((next) => { if (active) { api.setToken(next.access_token); setTokens(next); } }).catch(() => { if (active) { api.setToken(); setTokens(undefined); setAuthError('会话已失效，请重新登录'); } }); }, Math.max(1000, (tokens.expires_in - 60) * 1000));
    return () => { active = false; clearTimeout(timer); };
  }, [api, tokens]);
  if (!tokens) return <Login api={api} mock={mock} initialError={authError} onLogin={(next) => { setAuthError(''); setTokens(next); }} />;
  const pagePermission: Record<Page, string> = { overview: 'health.read', bots: 'bot.read', adapters: 'adapter.read', plugins: 'plugin.read', models: 'model.read', permissions: 'user.read', tasks: 'task.read', logs: 'log.read', audit: 'audit.read' };
  const visibleNav = nav.filter((item) => principal?.permissions.includes(pagePermission[item.key]));
  return <div className="shell"><aside className="sidebar"><div className="brand"><span className="brand-icon">K</span><div><strong>KiaraBot</strong><small>CONTROL PLANE</small></div></div><div className="nav-label">工作空间</div><nav aria-label="主导航">{visibleNav.map((item) => <button key={item.key} className={page === item.key ? 'nav-item selected' : 'nav-item'} onClick={() => setPage(item.key)}><span aria-hidden="true">{item.icon}</span>{item.label}</button>)}</nav><div className="sidebar-bottom"><span className={`dot ${status}`} /> <span>{statusText[status]}</span></div></aside><main><header className="topbar"><div><span className="crumb">工作空间 / </span><strong>{nav.find((item) => item.key === page)?.label}</strong></div><div className="top-actions"><span>{principal?.roles.join("、")}</span>{mock && <span className="mock-badge">MOCK 环境</span>}<button className="text-button" onClick={async () => { try { if (principal) await api.revokeSession(principal.session_id); } finally { api.setToken(); setTokens(undefined); setPrincipal(undefined); setStreamReady(false); setPage('overview'); } }}>退出登录</button></div></header><div className="content"><div className="page-title"><div><p className="eyebrow">KIARABOT CONTROL</p><h1>{nav.find((item) => item.key === page)?.label}</h1><p className="muted">{page === 'overview' ? '系统运行状态与服务概览' : '查看来自控制 API 的最新数据'}</p></div><button className="secondary" onClick={() => setRevision((n) => n + 1)}>↻ 刷新</button></div><div role="status" className={`connection ${status}`}><span className={`dot ${status}`} />{statusText[status]}</div>{!streamReady || !principal ? <p>正在建立会话…</p> : page === 'overview' ? <Overview api={api} revision={revision} /> : <ResourceView key={page} api={api} page={page} revision={revision} />}</div></main></div>;
}
function Login({ api, onLogin, mock, initialError }: { api: ApiClient; onLogin: (tokens: TokenPair) => void; mock: boolean; initialError: string }) {
  const [username, setUsername] = useState(''); const [password, setPassword] = useState(''); const [error, setError] = useState(initialError); const [busy, setBusy] = useState(false);
  return <div className="login-page"><div className="login-aside"><div className="brand"><span className="brand-icon">K</span><strong>KiaraBot</strong></div><div><p className="eyebrow">YOUR BOT, IN VIEW</p><h1>一处掌握，<br />从容管理。</h1><p>连接状态、插件与运行记录，<br />都在一个清晰的工作空间。</p></div><small>KIARABOT CONTROL PLANE · V1</small></div><div className="login-main"><form onSubmit={async (event) => { event.preventDefault(); setBusy(true); setError(''); try { const result = await api.login({ username, password }); setPassword(''); onLogin(result); } catch (e) { setError(errorText(e)); } finally { setBusy(false); } }}><p className="eyebrow">WELCOME BACK</p><h2>登录控制台</h2><p className="muted">输入账号凭据以继续</p>{mock && <p className="mock-note">Mock 模式：输入任意非空用户名和密码。</p>}<label>用户名<input autoComplete="username" required value={username} onChange={(e) => setUsername(e.target.value)} /></label><label>密码<input type="password" autoComplete="current-password" required value={password} onChange={(e) => setPassword(e.target.value)} /></label>{error && <div role="alert" className="error">{error}</div>}<button className="primary" disabled={busy}>{busy ? '登录中…' : '登录'} <span>→</span></button><p className="login-foot">凭据仅用于当前会话，不会保存在浏览器持久化存储中。</p></form></div></div>;
}
function Overview({ api, revision }: { api: ApiClient; revision: number }) {
  const [health, setHealth] = useState<Health>(); const [error, setError] = useState('');
  useEffect(() => { let active = true; setError(''); api.health().then((value) => { if (active) setHealth(value); }).catch((e) => { if (active) setError(errorText(e)); }); return () => { active = false; }; }, [api, revision]);
  return <>{error && <div role="alert" className="error">{error}</div>}<section className="hero"><div><p className="eyebrow">SYSTEM STATUS</p><h2>{health ? ({ HEALTH_STATE_UNSPECIFIED: '状态未知', HEALTHY: '一切运行正常', DEGRADED: '部分服务受影响', UNHEALTHY: '服务不可用' }[health.state]) : '正在获取状态…'}</h2><p>来自 Control API 的健康快照</p></div><div className="hero-orbit"><span>✦</span></div></section><div className="section-heading"><div><p className="eyebrow">COMPONENTS</p><h2>服务组件</h2></div><span className="muted">{health?.components.length ?? 0} 个组件</span></div><div className="cards">{health?.components.map((component) => <article className="card" key={component.component}><span className="card-icon">◈</span><div><h3>{component.component}</h3><p className="muted">{component.reason || '暂无详情'}</p></div><span className="tag">{component.state}</span></article>)}</div></>;
}
function ResourceView({ api, page, revision }: { api: ApiClient; page: Exclude<Page, 'overview'>; revision: number }) {
  const [data, setData] = useState<ViewData>(); const [error, setError] = useState(''); const [loading, setLoading] = useState(true); const [token, setToken] = useState<string>();
  useEffect(() => { setToken(undefined); }, [revision]);
  useEffect(() => { let active = true; setLoading(true); setError(''); loadView(api, page, token).then((value) => { if (active) setData(value); }).catch((e) => { if (active) { setData(undefined); setError(errorText(e)); } }).finally(() => { if (active) setLoading(false); }); return () => { active = false; }; }, [api, page, revision, token]);
  return <section className="table-card"><div className="table-head"><div><p className="eyebrow">LIVE DATA</p><h2>{nav.find((item) => item.key === page)?.label}列表</h2></div><span className="count">{data?.rows.length ?? 0} 条</span></div>{loading && <p className="empty">正在加载…</p>}{error && <div role="alert" className="error">{error}</div>}{!loading && data && (data.rows.length ? <div className="table-wrap"><table><thead><tr>{data.columns.map((column) => <th key={column}>{column}</th>)}</tr></thead><tbody>{data.rows.map((row) => <tr key={row.id}>{row.cells.map((cell, index) => <td key={index}>{cell}</td>)}</tr>)}</tbody></table></div> : <p className="empty">暂无数据</p>)}<div className="pagination">{token && <button className="secondary" onClick={() => setToken(undefined)}>返回首页</button>}{data?.next && <button className="secondary" onClick={() => setToken(data.next)}>下一页 →</button>}</div></section>;
}
