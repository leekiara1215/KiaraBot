import type { Fetcher, Schemas } from '../api/client';
import type { SocketFactory, SocketLike } from '../stream/control-stream';

const health = (state: Schemas['Health']['state'], component: string): Schemas['Health'] => ({
  state, components: [{ component, state }],
});

function pluginItem(id: string, name: string, state: Schemas['Plugin']['state']): Schemas['Plugin'] {
  return { id, name, version: '1.2.0', api_version: 'kiarabot.plugin.v1', runtime: 'python-rpc', isolation: 'process', ordered: false, state, permissions: ['messages.read'] };
}

const base = {
  '/health': health('HEALTHY', 'Core'),
  '/bots': { items: [{ id: 'bot-01', name: 'Kiara', state: 'ONLINE' }], page: {} } satisfies Schemas['BotPage'],
  '/adapters': { items: [{ id: 'adapter-main', implementation: 'NoneBot2', state: 'HEALTHY', version: '1.0.0', capabilities: ['messages', 'groups'] }], page: {} } satisfies Schemas['AdapterPage'],
  '/models': { items: [{ id: 'model-primary', name: 'Primary provider', state: 'READY' }], page: {} } satisfies Schemas['ModelPage'],
  '/groups': { items: [{ id: 'group-01', name: 'Operators', roles: ['operator'], permissions: ['logs.read', 'tasks.read'] }], page: {} } satisfies Schemas['GroupPage'],
  '/users': { items: [{ id: 'user-01', name: 'Administrator', groups: ['group-01'], roles: ['admin'], permissions: ['*'] }], page: {} } satisfies Schemas['UserPage'],
  '/tasks': { items: [{ id: 'task-01', name: 'Daily digest', state: 'SCHEDULED' }], page: {} } satisfies Schemas['TaskPage'],
  '/audit': { items: [{ id: 'audit-01', occurred_at: '2026-10-30T02:00:00Z', actor: 'admin', action: 'login', resource: 'control-panel' }], page: {} } satisfies Schemas['AuditPage'],
};
const logItems = Array.from({ length: 25 }, (_, index) => ({
  id: `log-${String(index + 1).padStart(2, '0')}`,
  name: `Core event ${index + 1}`,
  occurred_at: new Date(Date.UTC(2026, 9, 30, 2, index)).toISOString(),
  level: index % 7 === 0 ? 'WARN' : 'INFO',
  component: 'Core',
  message: index === 0 ? '服务已启动' : `第 ${index + 1} 条运行日志`,
})) satisfies Schemas['LogEntry'][];
const json = (body: unknown, status = 200, headers: Record<string, string> = {}) =>
  new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json', ...headers } });
const error = (status: number, code: string, message: string, extra: Record<string, string> = {}) =>
  json({ code, message, request_id: `mock-${code.toLowerCase()}` }, status, extra);

/** Page `items` with an opaque next token when the client asks for more (only /logs and /sessions). */
function paginate<T>(items: T[], url: URL, token: string, page: 'LogPage' | 'SessionPage') {
  const size = Math.min(100, Math.max(1, Number(url.searchParams.get('page_size') ?? 20) || 20));
  const offset = url.searchParams.get('page_token') === token ? size : 0;
  const slice = items.slice(offset, offset + size);
  const next = offset + size < items.length ? token : undefined;
  return { items: slice, page: next ? { next_page_token: next, page_size: size } : { page_size: size } } as unknown as Schemas[typeof page];
}

export class MockServer {
  denied = new Set<string>();
  failures = new Map<string, { status: number; code?: string; retryAfter?: number }>();
  requests: string[] = [];
  queries: string[] = [];
  sockets: MockSocket[] = [];
  private pluginStates = new Map<string, Schemas['Plugin']['state']>([['welcome', 'PLUGIN_STATE_RUNNING']]);
  private configs = new Map<string, Schemas['PluginConfig']>([
    ['welcome', { version: '1', values: { greeting: '你好' }, delivery: { state: 'APPLIED', applied_version: '1' } }],
  ]);
  private sessions: Schemas['Session'][] = [
    { session_id: 'session-current-0123456789', user_id: 'mock-user', created_at: '2026-10-30T02:00:00Z', expires_at: '2026-11-29T02:00:00Z', state: 'ACTIVE' },
    { session_id: 'session-other-0123456789', user_id: 'operator', created_at: '2026-10-29T02:00:00Z', expires_at: '2026-11-28T02:00:00Z', state: 'ACTIVE' },
  ];

  /** Test helper: simulate a concurrent config write so expected_version becomes stale. */
  bumpConfig(pluginId: string) {
    const current = this.configs.get(pluginId);
    if (current) this.configs.set(pluginId, { ...current, version: String(Number(current.version) + 1) });
  }

  /** Test helper: push a resource event to every ready socket. */
  pushEvent(eventType: string, payload: unknown = {}) {
    for (const socket of this.sockets) socket.push(eventType, payload);
  }

  fetch: Fetcher = async (input, init) => {
    const url = new URL(String(input), 'http://localhost');
    const path = url.pathname.replace(/^\/api\/v1/, '');
    this.requests.push(path);
    this.queries.push(url.search);
    if (path === '/auth/login') {
      const body = JSON.parse(String(init?.body ?? '{}'));
      if (!body.username || !body.password) return error(401, 'UNAUTHENTICATED', '用户名或密码错误');
      return json({ access_token: 'mock-access-token-0123456789', refresh_token: 'mock-refresh-token-0123456789', expires_in: 900 } satisfies Schemas['TokenPair']);
    }
    if (path === '/auth/refresh') return json({ access_token: 'mock-access-token-rotated-0123456789', refresh_token: 'mock-refresh-token-rotated-0123456789', expires_in: 900 } satisfies Schemas['TokenPair']);
    if (!new Headers(init?.headers).get('Authorization')) return error(401, 'UNAUTHENTICATED', '请登录');
    const failure = this.failures.get(path);
    if (failure) {
      const retry: Record<string, string> = {};
      if (failure.retryAfter) retry['Retry-After'] = String(failure.retryAfter);
      return error(failure.status, failure.code ?? 'INTERNAL', `模拟故障 HTTP ${failure.status}`, retry);
    }
    if (path === '/auth/me') {
      return json({
        user_id: 'mock-user', session_id: 'session-current-0123456789', roles: ['admin'],
        permissions: ['health.read', 'bot.read', 'adapter.read', 'plugin.read', 'plugin.manage',
          'plugin.config.read', 'plugin.config.write', 'model.read', 'group.read', 'user.read',
          'task.read', 'log.read', 'audit.read', 'action.read', 'session.read', 'session.revoke'],
      } satisfies Schemas['Principal']);
    }
    if (path === '/auth/sessions') return json(paginate(this.sessions, url, 'sessions-page-2', 'SessionPage'));
    if (path === '/plugins/validate') {
      const manifest = JSON.parse(String(init?.body ?? '{}')) as Partial<Schemas['PluginManifest']>;
      const errors: string[] = [];
      for (const field of ['id', 'name', 'version', 'api_version', 'runtime'] as const) if (!manifest[field]) errors.push(`${field} is required`);
      if (manifest.runtime && manifest.runtime !== 'python-rpc') errors.push('only python-rpc is supported in v1');
      if (manifest.isolation && manifest.isolation !== 'process') errors.push('only process isolation is supported in v1');
      if (manifest.dependencies?.length) errors.push('dependencies are not supported in v1');
      return json({ valid: errors.length === 0, errors } satisfies Schemas['ValidationResult']);
    }
    if (this.denied.has(path)) return error(403, 'PERMISSION_DENIED', '权限不足');
    const action = /^\/plugins\/([^/]+)\/(enable|disable|config|health)$/.exec(path);
    if (action) {
      const [, id, kind] = action;
      const method = String(init?.method ?? 'GET').toUpperCase();
      if (kind === 'enable' || kind === 'disable') {
        if (method !== 'POST') return error(405, 'INVALID_ARGUMENT', 'method not allowed');
        this.pluginStates.set(id, kind === 'enable' ? 'PLUGIN_STATE_RUNNING' : 'PLUGIN_STATE_DISABLED');
        return new Response(null, { status: 202 });
      }
      if (kind === 'health') return json(health('HEALTHY', id));
      if (method === 'PUT') {
        const update = JSON.parse(String(init?.body ?? '{}')) as Partial<Schemas['PluginConfigUpdate']>;
        const current = this.configs.get(id);
        if (!current) return error(404, 'NOT_FOUND', '插件配置不存在');
        if (update.expected_version !== current.version) return error(409, 'CONFLICT', 'expected_version 已过期');
        const next: Schemas['PluginConfig'] = {
          version: String(Number(current.version) + 1),
          values: update.values ?? {},
          delivery: { state: 'APPLIED', applied_version: String(Number(current.version) + 1) },
        };
        this.configs.set(id, next);
        return json(next);
      }
      const config = this.configs.get(id);
      return config ? json(config) : error(404, 'NOT_FOUND', '插件配置不存在');
    }
    if (path === '/actions/by-idempotency-key' || /^\/actions\/[^/]+$/.test(path)) {
      // api/openapi.yaml: x-contract-status not_implemented -> every caller gets NOT_FOUND in v1.
      return error(404, 'NOT_FOUND', 'not_implemented: v1 REST Action 查询无生产者');
    }
    if (path.endsWith('/revoke')) {
      const session = path.split('/')[3];
      this.sessions = this.sessions.filter((item) => item.session_id !== session);
      return new Response(null, { status: 204 });
    }
    if (path === '/plugins') {
      const items = [...this.pluginStates].map(([id, state]) => pluginItem(id, id === 'welcome' ? 'Welcome' : id, state));
      return json({ items, page: {} } satisfies Schemas['PluginPage']);
    }
    if (path === '/logs') return json(paginate(logItems, url, 'logs-page-2', 'LogPage'));
    const value = base[path as keyof typeof base];
    return value ? json(value) : error(404, 'NOT_FOUND', '资源不存在');
  };

  socketFactory: SocketFactory = () => { const socket = new MockSocket(); this.sockets.push(socket); queueMicrotask(() => socket.open()); return socket; };
}

export class MockSocket implements SocketLike {
  onopen: ((event: Event) => void) | null = null;
  onmessage: ((event: MessageEvent) => void) | null = null;
  onclose: ((event: CloseEvent) => void) | null = null;
  onerror: ((event: Event) => void) | null = null;
  sent: unknown[] = [];
  ready = false;
  sequence = 0;
  private closed = false;
  open() { if (!this.closed) this.onopen?.(new Event('open')); }
  send(data: string) {
    if (this.closed) return;
    const frame = JSON.parse(data);
    this.sent.push(frame);
    queueMicrotask(() => {
      if (this.closed) return;
      this.ready = true;
      this.sequence = Number(frame.last_sequence ?? 0);
      this.emit({ type: 'SESSION_READY', session_id: frame.session_id ?? 'mock-session-capability-0123456789', last_sequence: this.sequence });
    });
  }
  /** Test helper: emit a sequenced stream event (only after SESSION_READY). */
  push(eventType: string, payload: unknown) {
    if (!this.ready || this.closed) return;
    this.sequence += 1;
    this.emit({ schema_version: 'kiarabot.stream.v1', event_type: eventType, sequence: this.sequence, occurred_at: new Date().toISOString(), payload });
  }
  emit(value: unknown) { if (!this.closed) this.onmessage?.(new MessageEvent('message', { data: JSON.stringify(value) })); }
  /** Test helper: close with a protocol close code (4401/4403/4400/1013/1000). */
  closeWith(code: number) {
    if (this.closed) return;
    this.closed = true;
    this.onclose?.(new CloseEvent('close', { code }));
  }
  close() { this.closeWith(1000); }
}
