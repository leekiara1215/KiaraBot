/**
 * App-level recovery UI tests: a missing/failed realtime stream must never block independently
 * authorized REST pages, and the fallback marks "实时不可用" plus the last confirmed time.
 */
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { App } from '../src/App';
import { ApiClient } from '../src/api/client';
import { MockServer } from '../src/mock/server';
import { ControlStream } from '../src/stream/control-stream';
import { silentSocketFactory } from './target-fixtures';

async function login(server: MockServer, socketFactory = server.socketFactory) {
  const api = new ApiClient(server.fetch);
  const rendered = render(<App api={api} socketFactory={socketFactory} />);
  fireEvent.change(screen.getByLabelText('用户名'), { target: { value: 'admin' } });
  fireEvent.change(screen.getByLabelText('密码'), { target: { value: 'secret' } });
  fireEvent.click(screen.getByRole('button', { name: /登录/ }));
  return rendered;
}

describe('App REST fallback while the stream is unavailable', () => {
  it('renders REST pages before any SESSION_READY and marks the fallback as unconfirmed', async () => {
    const server = new MockServer();
    await login(server, silentSocketFactory);
    // REST health is shown even though the socket never opens/SESSION_READY never arrives.
    expect(await screen.findByText('一切运行正常')).toBeInTheDocument();
    expect(screen.getByText(/实时连接不可用，当前为 REST 快照（非最新）/)).toBeInTheDocument();
    expect(screen.getByText(/最后确认：尚未确认/)).toBeInTheDocument();
    fireEvent.click(within(screen.getByRole('navigation')).getByRole('button', { name: '账号（P12）' }));
    expect(await screen.findByText('account-qq-10001')).toBeInTheDocument();
    expect(server.requests).toContain('/accounts');
  });

  it('shows the last confirmed time after a live reference stream disconnects', async () => {
    const server = new MockServer();
    const rendered = await login(server);
    await waitFor(() => expect(server.sockets[0]?.ready).toBe(true));
    await waitFor(() => expect(screen.getAllByText('实时连接正常').length).toBeGreaterThan(0));
    act(() => server.sockets[0].closeWith(1001));
    expect(await screen.findByText(/实时连接不可用，当前为 REST 快照（非最新）/)).toBeInTheDocument();
    expect(screen.getByText(/最后确认：/)).toBeInTheDocument();
    expect(screen.queryByText(/最后确认：尚未确认/)).not.toBeInTheDocument();
    // REST pages stay usable while the stream retries.
    fireEvent.click(within(screen.getByRole('navigation')).getByRole('button', { name: '账号（P12）' }));
    expect(await screen.findByText('account-qq-10001')).toBeInTheDocument();
    rendered.unmount();
  });
});

describe('reference stream token rotation', () => {
  it('resumes with the new access token and keeps the session/sequence', async () => {
    const server = new MockServer();
    let token = 'mock-access-token-0123456789';
    const statuses: string[] = [];
    const stream = new ControlStream({
      url: 'ws://mock/ws/control',
      accessToken: () => token,
      factory: server.socketFactory,
      onStatus: (status) => statuses.push(status),
      onRefresh: () => {},
    });
    stream.start();
    await Promise.resolve(); await Promise.resolve(); await Promise.resolve();
    expect(server.sockets[0].sent[0]).toEqual({ type: 'hello', access_token: token, last_sequence: 0 });
    server.sockets[0].emit({
      schema_version: 'kiarabot.stream.v1', event_type: 'future.event', sequence: 4,
      occurred_at: '2026-11-04T00:00:00Z', payload: {},
    });
    token = 'mock-access-token-rotated-0123456789';
    stream.reauthenticate();
    expect(server.sockets).toHaveLength(2);
    await Promise.resolve(); await Promise.resolve(); await Promise.resolve();
    expect(server.sockets[1].sent[0]).toEqual({
      type: 'resume',
      access_token: token,
      session_id: 'mock-session-capability-0123456789',
      last_sequence: 4,
    });
    expect(statuses).toContain('connected');
    stream.stop();
  });

});
