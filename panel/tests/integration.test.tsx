import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { App } from '../src/App';
import { ApiClient } from '../src/api/client';
import { MockServer } from '../src/mock/server';
import { ControlStream } from '../src/stream/control-stream';

async function login(server: MockServer) {
  const api = new ApiClient(server.fetch);
  render(<App api={api} socketFactory={server.socketFactory} mock />);
  fireEvent.change(screen.getByLabelText('用户名'), { target: { value: 'admin' } });
  fireEvent.change(screen.getByLabelText('密码'), { target: { value: 'secret' } });
  fireEvent.click(screen.getByRole('button', { name: /登录/ }));
  await screen.findByText('一切运行正常');
  return api;
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
      fireEvent.click(within(screen.getByRole('navigation')).getByRole('button', { name: page }));
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
    fireEvent.click(within(screen.getByRole('navigation')).getByRole('button', { name: '日志' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('权限不足');
    act(() => server.sockets[0].close());
    expect(screen.getAllByText('连接已断开，正在重试').length).toBeGreaterThan(0);
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
    expect(refresh).not.toHaveBeenCalled();
    server.sockets[0].close(); vi.advanceTimersByTime(1000); await Promise.resolve(); await Promise.resolve();
    expect(server.sockets[1].sent[0]).toEqual({ type: 'resume', session_id: 'mock-session-capability-0123456789', last_sequence: 4 });
    server.sockets[1].emit({ type: 'RESYNC_REQUIRED', reason: 'expired' });
    expect(statuses).toContain('resync');
    vi.advanceTimersByTime(1000); await Promise.resolve(); await Promise.resolve();
    expect(server.sockets[2].sent[0]).toEqual({ type: 'hello', access_token: 'mock-access-token-0123456789', last_sequence: 0 });
    expect(refresh).toHaveBeenCalledTimes(1);
    stream.stop();
  });
});
