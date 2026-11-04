/**
 * OperationTracker recovery tests: target `/ws/operations` notifications only trigger an
 * authoritative owner GetOperation verification. HTTP 200 is not completion, BLOCKED/UNKNOWN stay
 * unknown, a resync forces a full REST re-read and no management write is ever re-sent.
 */
import { act, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { ApiClient, type TargetResourceScope } from '../src/api/client';
import { MockServer } from '../src/mock/server';
import { OperationTracker } from '../src/views/p12-live';
import { FakeTargetSockets } from './target-fixtures';

const scope = { resources: [{ kind: 'RESOURCE_INSTANCE', id: 'instance-welcome-01' }] } as TargetResourceScope;
const READY_SESSION = 'session-capability-0123456789abcdef';
const TOKEN = 'mock-access-token-0123456789';
const OPERATION_PATH = '/operations/runtime/operation-mock-01';

function ownerScope() {
  return { resources: [{ kind: 'RESOURCE_INSTANCE', id: 'instance-welcome-01' }] };
}

function sessionReady(lastSequence = 0, ownerSequence = '0') {
  return {
    type: 'SESSION_READY', session_id: READY_SESSION, last_sequence: lastSequence,
    owner_cursor: { source_service: 'runtime', epoch: 'epoch-1', scope: ownerScope(), sequence: ownerSequence },
  };
}

function operationUpdated(sequence: number, ownerSequence: string, operationId = 'operation-mock-01') {
  return {
    schema_version: 'kiarabot.stream.v1',
    event_type: 'operation.updated',
    sequence,
    occurred_at: '2026-11-04T00:00:00Z',
    payload: {
      ref: {
        owner_service: 'runtime', operation_id: operationId,
        resource: { kind: 'RESOURCE_INSTANCE', id: 'instance-welcome-01' },
      },
    },
    owner_cursor: { source_service: 'runtime', epoch: 'epoch-1', scope: ownerScope(), sequence: ownerSequence },
  };
}

function setup(stream = true) {
  const server = new MockServer();
  const methods: string[] = [];
  const api = new ApiClient((input, init) => { methods.push(String(init?.method ?? 'GET').toUpperCase()); return server.fetch(input, init); });
  api.setToken(TOKEN);
  const sockets = new FakeTargetSockets();
  const streamConfig = stream
    ? { wsUrl: 'ws://mock/ws/control', socketFactory: sockets.factory, tokenProvider: () => TOKEN }
    : undefined;
  render(<OperationTracker
    api={api}
    operation={{ owner_service: 'runtime', operation_id: 'operation-mock-01' }}
    scope={scope}
    stream={streamConfig}
  />);
  const count = () => server.requests.filter((path) => path === OPERATION_PATH).length;
  return { server, api, sockets, count, methods };
}

afterEach(() => { vi.useRealTimers(); });

describe('OperationTracker target stream recovery', () => {
  it('loads the authoritative owner snapshot and labels the REST fallback when no stream is wired', async () => {
    const { server, count } = setup(false);
    expect((await screen.findAllByText('执行中')).length).toBeGreaterThan(0);
    expect(count()).toBe(1);
    expect(screen.getByText(/未配置 target operations 频道；REST 快照回退/)).toBeInTheDocument();
    expect(screen.getByText(/200 不等于完成/)).toBeInTheDocument();
    expect(server.requests).toContain(OPERATION_PATH);
  });

  it('verifies through GetOperation on SESSION_READY and on matching operation.updated events only', async () => {
    const { sockets, count } = setup();
    await screen.findAllByText('执行中');
    expect(count()).toBe(1);
    expect(sockets.sockets).toHaveLength(1);
    expect(sockets.last.url).toBe('ws://mock/ws/operations?owner_service=runtime');
    const socket = sockets.last;
    act(() => {
      socket.open();
      socket.receive(sessionReady(0, '0'));
    });
    // SESSION_READY (owner cursor recorded first) forces an authoritative REST re-read.
    await waitFor(() => expect(count()).toBe(2));
    await screen.findByText(/实时已连接/);
    expect(screen.getByText(/最后确认：/)).toBeInTheDocument();
    expect(screen.queryByText(/最后确认：尚未确认/)).not.toBeInTheDocument();

    act(() => socket.receive(operationUpdated(1, '1')));
    await waitFor(() => expect(count()).toBe(3));

    // Notification for another operation must not trigger a read for this tracker.
    act(() => socket.receive(operationUpdated(2, '2', 'operation-other')));
    await act(async () => { await Promise.resolve(); });
    expect(count()).toBe(3);
  });

  it('re-reads REST and opens a fresh hello session after RESYNC_REQUIRED', async () => {
    const { sockets, count } = setup();
    await screen.findAllByText('执行中');
    const first = sockets.last;
    act(() => { first.open(); first.receive(sessionReady(0, '0')); });
    await waitFor(() => expect(count()).toBe(2));

    act(() => first.receive({ type: 'RESYNC_REQUIRED', reason: 'INVALID_CURSOR' }));
    await waitFor(() => expect(sockets.sockets).toHaveLength(2));
    await waitFor(() => expect(count()).toBeGreaterThanOrEqual(3)); // full authoritative reload
    const second = sockets.last;
    act(() => second.open());
    expect(second.firstFrame?.type).toBe('hello');
    act(() => second.receive(sessionReady(0, '0')));
    await screen.findByText(/实时已连接/);
    expect(screen.queryByText(/实时不可用/)).not.toBeInTheDocument();
  });

  it('keeps REST verification when the target stream is throttled/terminated (1013)', async () => {
    const { sockets, count } = setup();
    await screen.findAllByText('执行中');
    const socket = sockets.last;
    act(() => { socket.open(); socket.receive(sessionReady(0, '0')); });
    await waitFor(() => expect(count()).toBe(2));
    act(() => socket.closeWith(1013));
    await screen.findByText(/服务端限流（实时不可用）；REST 快照回退中/);
    expect(screen.getByText(/最后确认：/)).toBeInTheDocument();
    // Terminal stream state does not stop the tracker from remaining usable.
    expect(screen.getAllByText('执行中').length).toBeGreaterThan(0);
  });

  it('destroys the old operations socket when the tracked operation/scope changes', async () => {
    const server = new MockServer();
    const api = new ApiClient(server.fetch);
    api.setToken(TOKEN);
    const sockets = new FakeTargetSockets();
    const streamConfig = { wsUrl: 'ws://mock/ws/control', socketFactory: sockets.factory, tokenProvider: () => TOKEN };
    const otherScope = { resources: [{ kind: 'RESOURCE_INSTANCE', id: 'instance-reminder-01' }] } as TargetResourceScope;
    const { rerender } = render(<OperationTracker
      api={api}
      operation={{ owner_service: 'runtime', operation_id: 'operation-mock-01' }}
      scope={scope}
      stream={streamConfig}
    />);
    await screen.findAllByText('执行中');
    const first = sockets.last;
    expect(first.url).toBe('ws://mock/ws/operations?owner_service=runtime');
    rerender(<OperationTracker
      api={api}
      operation={{ owner_service: 'runtime', operation_id: 'operation-lifecycle-01' }}
      scope={otherScope}
      stream={streamConfig}
    />);
    expect(first.closed).toBe(true);
    expect(sockets.sockets).toHaveLength(2);
    act(() => sockets.last.open());
    expect(sockets.last.firstFrame).toEqual({
      type: 'hello', access_token: TOKEN, last_sequence: 0,
      scope: { resources: [{ kind: 'RESOURCE_INSTANCE', id: 'instance-reminder-01' }] },
    });
    await waitFor(() => expect(server.requests).toContain('/operations/runtime/operation-lifecycle-01'));
    expect((await screen.findAllByText('受阻')).length).toBeGreaterThan(0);
  });

  it('never infers completion from HTTP 200 and keeps BLOCKED/UNKNOWN unknown', async () => {
    const server = new MockServer();
    const methods: string[] = [];
    const api = new ApiClient((input, init) => { methods.push(String(init?.method ?? 'GET').toUpperCase()); return server.fetch(input, init); });
    api.setToken(TOKEN);
    render(<OperationTracker
      api={api}
      operation={{ owner_service: 'runtime', operation_id: 'operation-lifecycle-01' }}
      scope={scope}
    />);
    expect((await screen.findAllByText('受阻')).length).toBeGreaterThan(0);
    expect(screen.getByText(/结果未知（不得推断完成）/)).toBeInTheDocument();
    expect(screen.queryByText('状态成功')).not.toBeInTheDocument();
    expect(screen.getByText(/BLOCKED\/UNKNOWN 保持未知/)).toBeInTheDocument();
    expect(methods.every((method) => method === 'GET')).toBe(true);
  });

  it('polls REST on a slow fallback interval instead of the old 2s loop', async () => {
    vi.useFakeTimers();
    const { count } = setup(false);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(count()).toBe(1);
    await act(async () => { await vi.advanceTimersByTimeAsync(2000); });
    expect(count()).toBe(1); // the 2s REST polling loop is gone
    await act(async () => { await vi.advanceTimersByTimeAsync(8000); });
    expect(count()).toBe(2); // slow fallback snapshot after 10s
  });
});
