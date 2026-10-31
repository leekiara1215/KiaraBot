import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { App } from '../src/App';
import { ApiClient } from '../src/api/client';
import { MockServer } from '../src/mock/server';
import { ControlStream } from '../src/stream/control-stream';
import { ErrorBoundary } from '../src/ui';

async function login(server: MockServer) {
  const api = new ApiClient(server.fetch);
  render(<App api={api} socketFactory={server.socketFactory} mock />);
  fireEvent.change(screen.getByLabelText('用户名'), { target: { value: 'admin' } });
  fireEvent.change(screen.getByLabelText('密码'), { target: { value: 'secret' } });
  fireEvent.click(screen.getByRole('button', { name: /登录/ }));
  await screen.findByText('一切运行正常');
  await waitFor(() => expect(server.sockets[0]?.ready).toBe(true));
  return api;
}

function navigate(name: string) {
  fireEvent.click(within(screen.getByRole('navigation')).getByRole('button', { name }));
}

describe('mock API integration and UI', () => {
  it('rotates tokens through the API and sends an idempotency key', async () => {
    const server = new MockServer(); const requests: RequestInit[] = [];
    const api = new ApiClient(async (input, init) => { requests.push(init ?? {}); return server.fetch(input, init); });
    const tokens = await api.login({ username: 'admin', password: 'secret' });
    const rotated = await api.refresh(tokens.refresh_token);
    expect(rotated.access_token).toContain('rotated');
    expect(server.requests).toContain('/auth/refresh');
    expect(new Headers(requests[1].headers).get('Idempotency-Key')?.length).toBeGreaterThanOrEqual(16);
    expect(JSON.parse(String(requests[1].body))).toEqual({ refresh_token: tokens.refresh_token });
  });
  it('logs in and loads each requested page from the API without persisting tokens', async () => {
    const server = new MockServer();
    const setItem = vi.spyOn(Storage.prototype, 'setItem');
    await login(server);
    const expected = [
      ['机器人', 'Kiara', '/bots'], ['适配器', 'NoneBot2', '/adapters'], ['插件', 'Welcome', '/plugins'],
      ['模型', 'Primary provider', '/models'], ['权限', 'Administrator', '/users'], ['任务', 'Daily digest', '/tasks'],
      ['日志', '服务已启动', '/logs'], ['审计', 'control-panel', '/audit'],
    ];
    for (const [page, content, path] of expected) {
      navigate(page);
      expect(await screen.findByText(content)).toBeInTheDocument();
      expect(server.requests).toContain(path);
    }
    expect(server.requests).toContain('/groups');
    expect(setItem).not.toHaveBeenCalled();
  });
  it('shows a permission error and a disconnected status', async () => {
    const server = new MockServer(); server.denied.add('/logs');
    await login(server);
    await waitFor(() => expect(screen.getAllByText('实时连接正常').length).toBeGreaterThan(0));
    navigate('日志');
    expect(await screen.findByRole('alert')).toHaveTextContent('权限不足');
    act(() => server.sockets[0].close());
    expect(screen.getAllByText('连接已断开，正在重试').length).toBeGreaterThan(0);
  });
});

describe('plugin management endpoints', () => {
  it('enables, disables and reads plugin health', async () => {
    const server = new MockServer();
    await login(server);
    navigate('插件');
    await screen.findByText('Welcome');

    fireEvent.click(screen.getByRole('button', { name: '禁用 Welcome' }));
    expect(await screen.findByText(/禁用请求已接受/)).toBeInTheDocument();
    expect(server.requests).toContain('/plugins/welcome/disable');
    expect(await screen.findByText('已禁用')).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: '启用 Welcome' }));
    expect(await screen.findByText(/启用请求已接受/)).toBeInTheDocument();
    expect(server.requests).toContain('/plugins/welcome/enable');

    fireEvent.click(screen.getByRole('button', { name: '健康 Welcome' }));
    expect(await screen.findByText(/聚合状态/)).toBeInTheDocument();
    expect(server.requests).toContain('/plugins/welcome/health');
  });

  it('reads config with expected_version, saves it, and surfaces a 409 conflict', async () => {
    const server = new MockServer();
    await login(server);
    navigate('插件');
    await screen.findByText('Welcome');

    fireEvent.click(screen.getByRole('button', { name: '配置 Welcome' }));
    const editor = await screen.findByLabelText('配置值（JSON 对象）');
    expect(editor).toHaveValue(JSON.stringify({ greeting: '你好' }, null, 2));
    fireEvent.change(editor, { target: { value: JSON.stringify({ greeting: '你好', reply: '收到' }, null, 2) } });
    fireEvent.click(screen.getByRole('button', { name: /保存（expected_version=1）/ }));
    expect(await screen.findByText(/已保存持久化版本 2/)).toBeInTheDocument();
    const put = server.requests.lastIndexOf('/plugins/welcome/config');
    expect(put).toBeGreaterThanOrEqual(0);

    server.bumpConfig('welcome');
    fireEvent.click(screen.getByRole('button', { name: /保存（expected_version=2）/ }));
    expect(await screen.findByText('版本冲突')).toBeInTheDocument();
  });

  it('validates a plugin manifest without enabling it', async () => {
    const server = new MockServer();
    await login(server);
    navigate('插件');
    await screen.findByText('Welcome');
    fireEvent.click(screen.getByRole('button', { name: '校验清单' }));
    expect(await screen.findByText('清单有效')).toBeInTheDocument();
    expect(server.requests).toContain('/plugins/validate');
  });
});

describe('sessions and action lookup', () => {
  it('lists sessions and revokes one', async () => {
    const server = new MockServer();
    await login(server);
    navigate('权限');
    expect(await screen.findByText('认证会话')).toBeInTheDocument();
    expect(server.requests).toContain('/auth/sessions');
    const revokeButtons = screen.getAllByRole('button', { name: /撤销会话/ });
    fireEvent.click(revokeButtons[0]);
    expect(await screen.findByText(/已撤销/)).toBeInTheDocument();
    expect(server.requests.some((path) => path.endsWith('/revoke'))).toBe(true);
  });

  it('shows the not_implemented explanation for Action queries', async () => {
    const server = new MockServer();
    await login(server);
    navigate('动作');
    fireEvent.change(screen.getByLabelText('action_id'), { target: { value: 'action-1' } });
    fireEvent.click(screen.getByRole('button', { name: '按 ID 查询' }));
    expect(await screen.findByText('未找到（契约标记 not_implemented）')).toBeInTheDocument();
    expect(server.requests).toContain('/actions/action-1');
  });
});

describe('pagination, filters and stream events', () => {
  it('pages through logs and requests page_size', async () => {
    const server = new MockServer();
    await login(server);
    navigate('日志');
    await screen.findByText('服务已启动');
    expect(screen.getAllByRole('row')).toHaveLength(21);
    fireEvent.click(screen.getByRole('button', { name: '下一页 →' }));
    expect(await screen.findByText('第 25 条运行日志')).toBeInTheDocument();
    expect(server.queries.some((query) => query.includes('page_token=logs-page-2'))).toBe(true);

    fireEvent.change(screen.getByLabelText('每页条数'), { target: { value: '50' } });
    await waitFor(() => expect(server.queries.some((query) => query.includes('page_size=50'))).toBe(true));
    expect(await screen.findByText('第 25 条运行日志')).toBeInTheDocument();
  });

  it('refreshes the current view when a known stream event arrives', async () => {
    const server = new MockServer();
    await login(server);
    navigate('插件');
    await screen.findByText('Welcome');
    const before = server.requests.filter((path) => path === '/plugins').length;
    act(() => server.pushEvent('plugin.updated', { id: 'welcome' }));
    await waitFor(() => expect(server.requests.filter((path) => path === '/plugins').length).toBeGreaterThan(before));
    // message.updated is metadata-only and has no REST projection: it must still trigger the
    // existing refresh, not a message fetch, and must not be dropped as an unknown event.
    act(() => server.pushEvent('message.updated', { message_id: 'message-1', conversation_id: 'conversation-1', direction: 'received' }));
    await waitFor(() => expect(server.requests.filter((path) => path === '/plugins').length).toBe(before + 2));
    act(() => server.pushEvent('future.event', { ignored: true }));
    expect(server.requests.filter((path) => path === '/plugins').length).toBe(before + 2);
  });

  it('returns to login on 4401', async () => {
    const server = new MockServer();
    await login(server);
    act(() => server.sockets[0].closeWith(4401));
    expect(await screen.findByLabelText('用户名')).toBeInTheDocument();
  });

  it('reports 4403 without retrying', async () => {
    const server = new MockServer();
    await login(server);
    act(() => server.sockets[0].closeWith(4403));
    expect(await screen.findAllByText('无权访问该数据流，已停止重连')).not.toHaveLength(0);
    expect(server.sockets).toHaveLength(1);
  });

  it('classifies 429 and 503 failures with Retry-After and request_id', async () => {
    const server = new MockServer();
    server.failures.set('/logs', { status: 429, code: 'RATE_LIMITED', retryAfter: 7 });
    server.failures.set('/audit', { status: 503, code: 'UPSTREAM_UNAVAILABLE' });
    await login(server);
    navigate('日志');
    expect(await screen.findByText('请求过于频繁')).toBeInTheDocument();
    expect(screen.getByText(/约 7 秒/)).toBeInTheDocument();
    navigate('审计');
    expect(await screen.findByText('依赖不可用')).toBeInTheDocument();
  });
});

describe('WebSocket resume and resync', () => {
  afterEach(() => vi.useRealTimers());
  it('uses hello, resumes with the last sequence, and refreshes after resync', async () => {
    vi.useFakeTimers();
    const server = new MockServer(); const statuses: string[] = []; const refresh = vi.fn();
    const stream = new ControlStream({ url: 'ws://mock/ws', accessToken: 'mock-access-token-0123456789', factory: server.socketFactory, onStatus: (status) => statuses.push(status), onRefresh: refresh });
    stream.start(); await Promise.resolve(); await Promise.resolve();
    expect(server.sockets[0].sent[0]).toEqual({ type: 'hello', access_token: 'mock-access-token-0123456789', last_sequence: 0 });
    server.sockets[0].emit({ schema_version: 'kiarabot.stream.v1', event_type: 'future.event', sequence: 4, occurred_at: '2026-01-01T00:00:00Z', payload: {}, unknown: true });
    expect(refresh).toHaveBeenCalledTimes(1);
    server.sockets[0].close(); vi.advanceTimersByTime(1000); await Promise.resolve(); await Promise.resolve();
    expect(server.sockets[1].sent[0]).toEqual({ type: 'resume', access_token: 'mock-access-token-0123456789', session_id: 'mock-session-capability-0123456789', last_sequence: 4 });
    server.sockets[1].emit({ type: 'RESYNC_REQUIRED', reason: 'expired' });
    expect(statuses).toContain('resync');
    vi.advanceTimersByTime(1000); await Promise.resolve(); await Promise.resolve();
    expect(server.sockets[2].sent[0]).toEqual({ type: 'hello', access_token: 'mock-access-token-0123456789', last_sequence: 0 });
    expect(refresh).toHaveBeenCalledTimes(3);
    stream.stop();
  });

  it('stops on terminal close codes 4403, 4400 and 1013', async () => {
    for (const [code, text] of [[4403, 'forbidden'], [4400, 'protocol'], [1013, 'throttled']] as const) {
      const server = new MockServer(); const statuses: string[] = [];
      const stream = new ControlStream({ url: 'ws://mock/ws', accessToken: 'token', factory: server.socketFactory, onStatus: (status) => statuses.push(status), onRefresh: () => {} });
      stream.start(); await Promise.resolve(); await Promise.resolve();
      server.sockets[0].closeWith(code);
      expect(statuses.at(-1)).toBe(text);
      expect(server.sockets).toHaveLength(1);
      stream.stop();
    }
  });
});

describe('authentication races and rendering', () => {
  it('does not expire a newer session when an old request receives 401', async () => {
    let respond!: (response: Response) => void;
    const api = new ApiClient(() => new Promise<Response>((resolve) => { respond = resolve; }));
    const expired = vi.fn(); api.setUnauthorizedHandler(expired); api.setToken('old');
    const pending = api.health();
    api.setToken('new');
    respond(new Response(JSON.stringify({ code: 'UNAUTHENTICATED' }), { status: 401 }));
    await expect(pending).rejects.toMatchObject({ status: 401 });
    expect(expired).not.toHaveBeenCalled();
  });

  it('renders a recoverable error boundary instead of a blank page', () => {
    vi.spyOn(console, 'error').mockImplementation(() => {});
    function Boom(): JSX.Element { throw new Error('render exploded'); }
    render(<ErrorBoundary><Boom /></ErrorBoundary>);
    expect(screen.getByText('界面渲染失败')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '重试渲染' })).toBeInTheDocument();
  });
});
