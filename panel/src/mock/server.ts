import type { Fetcher, Schemas } from '../api/client';
import type { SocketFactory, SocketLike } from '../stream/control-stream';

const data = {
  '/health': { state: 'HEALTHY', components: [{ component: 'Core', state: 'HEALTHY', reason: '服务正常' }, { component: 'Adapter', state: 'HEALTHY', reason: '已连接' }] } satisfies Schemas['Health'],
  '/bots': { items: [{ id: 'bot-01', name: 'Kiara', state: 'ONLINE' }], page: {} } satisfies Schemas['BotPage'],
  '/adapters': { items: [{ id: 'adapter-main', implementation: 'NoneBot2', state: 'HEALTHY', version: '1.0.0', capabilities: ['messages', 'groups'] }], page: {} } satisfies Schemas['AdapterPage'],
  '/plugins': { items: [{ id: 'welcome', name: 'Welcome', version: '1.2.0', api_version: 'v1', runtime: 'python-rpc', isolation: 'process', ordered: false, state: 'PLUGIN_STATE_RUNNING', permissions: ['messages.read'] }], page: {} } satisfies Schemas['PluginPage'],
  '/models': { items: [{ id: 'model-primary', name: 'Primary provider', state: 'READY' }], page: {} } satisfies Schemas['ModelPage'],
  '/groups': { items: [{ id: 'group-01', name: 'Operators', roles: ['operator'], permissions: ['logs.read', 'tasks.read'] }], page: {} } satisfies Schemas['GroupPage'],
  '/users': { items: [{ id: 'user-01', name: 'Administrator', groups: ['group-01'], roles: ['admin'], permissions: ['*'] }], page: {} } satisfies Schemas['UserPage'],
  '/tasks': { items: [{ id: 'task-01', name: 'Daily digest', state: 'SCHEDULED' }], page: {} } satisfies Schemas['TaskPage'],
  '/logs': { items: [{ id: 'log-01', name: 'Core started', occurred_at: '2026-10-30T02:00:00Z', level: 'INFO', component: 'Core', message: '服务已启动' }], page: {} } satisfies Schemas['LogPage'],
  '/audit': { items: [{ id: 'audit-01', occurred_at: '2026-10-30T02:00:00Z', actor: 'admin', action: 'login', resource: 'control-panel' }], page: {} } satisfies Schemas['AuditPage'],
};
const json = (body: unknown, status = 200) => new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });
export class MockServer {
  denied = new Set<string>();
  requests: string[] = [];
  sockets: MockSocket[] = [];
  fetch: Fetcher = async (input, init) => {
    const url = new URL(String(input), 'http://localhost');
    const path = url.pathname.replace(/^\/api\/v1/, '');
    this.requests.push(path);
    if (path === '/auth/login') {
      const body = JSON.parse(String(init?.body ?? '{}'));
      if (!body.username || !body.password) return json({ code: 'UNAUTHENTICATED', message: '用户名或密码错误', request_id: 'mock-login' }, 401);
      return json({ access_token: 'mock-access-token-0123456789', refresh_token: 'mock-refresh-token-0123456789', expires_in: 900 } satisfies Schemas['TokenPair']);
    }
    if (path === '/auth/refresh') return json({ access_token: 'mock-access-token-rotated-0123456789', refresh_token: 'mock-refresh-token-rotated-0123456789', expires_in: 900 } satisfies Schemas['TokenPair']);
    if (!new Headers(init?.headers).get('Authorization')) return json({ code: 'UNAUTHENTICATED', message: '请登录', request_id: 'mock-auth' }, 401);
    if (this.denied.has(path)) return json({ code: 'PERMISSION_DENIED', message: '权限不足', request_id: 'mock-denied' }, 403);
    const value = data[path as keyof typeof data];
    return value ? json(value) : json({ code: 'NOT_FOUND', message: '资源不存在', request_id: 'mock-missing' }, 404);
  };
  socketFactory: SocketFactory = () => { const socket = new MockSocket(); this.sockets.push(socket); queueMicrotask(() => socket.open()); return socket; };
}
export class MockSocket implements SocketLike {
  onopen: ((event: Event) => void) | null = null;
  onmessage: ((event: MessageEvent) => void) | null = null;
  onclose: ((event: CloseEvent) => void) | null = null;
  onerror: ((event: Event) => void) | null = null;
  sent: unknown[] = [];
  private closed = false;
  open() { if (!this.closed) this.onopen?.(new Event('open')); }
  send(data: string) {
    const frame = JSON.parse(data); this.sent.push(frame);
    queueMicrotask(() => this.emit({ type: 'SESSION_READY', session_id: frame.session_id ?? 'mock-session-capability-0123456789', last_sequence: frame.last_sequence }));
  }
  emit(value: unknown) { if (!this.closed) this.onmessage?.(new MessageEvent('message', { data: JSON.stringify(value) })); }
  close() { if (this.closed) return; this.closed = true; this.onclose?.(new CloseEvent('close')); }
}
