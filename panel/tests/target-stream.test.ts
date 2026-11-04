/**
 * Target scoped WS client tests (WS-06 panel side).
 *
 * These use a deterministic fake transport; the real target WSS acceptance is registered by the
 * lead/WS worker. They cover token rotation with retained cursor, stale socket isolation, first
 * frame deadline, disconnect backoff, close codes, duplicate/gap/replay/resync handling and the
 * frozen close-code/reason contract parsed from api/asyncapi.yaml.
 */
import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  TARGET_CLOSE_CODE,
  TARGET_FIRST_FRAME_TIMEOUT_MS,
  canonicalizeScope,
  compareOwnerSequence,
  parseTargetFrame,
  scopeKey,
  targetChannelUrl,
  type TargetCursorService,
  type TargetEventEnvelope,
  type TargetOwnerCursor,
  type TargetScope,
} from '../src/stream/target-protocol';
import {
  TargetScopedStream, type TargetRecovery, type TargetStreamReady, type TargetStreamStatus,
} from '../src/stream/target-stream';
import { FakeTargetSockets } from './target-fixtures';

const scope: TargetScope = { resources: [{ kind: 'RESOURCE_INSTANCE', id: 'instance-1' }] };
const READY_SESSION = 'session-capability-0123456789abcdef';

function cursor(sequence: string, epoch = 'epoch-1', source: TargetCursorService = 'runtime'): TargetOwnerCursor {
  return { source_service: source, epoch, scope, sequence };
}

function readyFrame(overrides: { last_sequence?: number; owner_cursor?: TargetOwnerCursor; session_id?: string } = {}) {
  return { type: 'SESSION_READY', session_id: READY_SESSION, last_sequence: 0, owner_cursor: cursor('0'), ...overrides };
}

function eventFrame(sequence: number, overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    schema_version: 'kiarabot.stream.v1',
    event_type: 'operation.updated',
    sequence,
    occurred_at: '2026-11-04T00:00:00Z',
    payload: {
      ref: { owner_service: 'runtime', operation_id: 'operation-1', resource: { kind: 'RESOURCE_INSTANCE', id: 'instance-1' } },
    },
    owner_cursor: cursor(String(sequence)),
    ...overrides,
  };
}

function makeStream(overrides: Partial<ConstructorParameters<typeof TargetScopedStream>[0]> = {}) {
  const sockets = new FakeTargetSockets();
  const token = { value: 'access-token-1' };
  const statuses: TargetStreamStatus[] = [];
  const readies: TargetStreamReady[] = [];
  const events: TargetEventEnvelope[] = [];
  const recoveries: TargetRecovery[] = [];
  const unauthorized = { count: 0 };
  const stream = new TargetScopedStream({
    url: targetChannelUrl('ws://mock/ws/control', 'operations', 'runtime'),
    selection: { channel: 'operations', owner: 'runtime', scope },
    factory: sockets.factory,
    tokenProvider: () => token.value,
    onStatus: (status) => statuses.push(status),
    onReady: (ready) => readies.push(ready),
    onEvent: (event) => events.push(event),
    onRecovery: (recovery) => recoveries.push(recovery),
    onUnauthorized: () => { unauthorized.count += 1; },
    ...overrides,
  });
  active.push(stream);
  return { stream, sockets, token, statuses, readies, events, recoveries, unauthorized };
}

const active: TargetScopedStream[] = [];
afterEach(() => {
  for (const stream of active.splice(0)) stream.stop();
  vi.useRealTimers();
});

describe('target channel URLs and first frame', () => {
  it('opens /ws/operations with owner_service and keeps the access token out of the URL', () => {
    const h = makeStream();
    h.stream.start();
    expect(h.sockets.sockets).toHaveLength(1);
    expect(h.sockets.last.url).toBe('ws://mock/ws/operations?owner_service=runtime');
    expect(h.sockets.last.url).not.toContain('access-token');
    h.sockets.last.open();
    expect(h.sockets.last.firstFrame).toEqual({
      type: 'hello', access_token: 'access-token-1', last_sequence: 0, scope,
    });
  });

  it('rejects an operations channel without a frozen owner selector', () => {
    expect(() => targetChannelUrl('ws://mock/ws', 'operations')).toThrow(/owner_service/);
    expect(targetChannelUrl('ws://mock/ws/control', 'control', 'control')).toBe('ws://mock/ws/control?owner_service=control');
  });

  it('canonicalizes scope by kind/id and sends 1..127 deduped exact resources', () => {
    const raw: TargetScope = {
      resources: [
        { kind: 'RESOURCE_TASK', id: 'task-2' },
        { kind: 'RESOURCE_INSTANCE', id: 'instance-1' },
        { kind: 'RESOURCE_TASK', id: 'task-2' },
        { kind: 'RESOURCE_TASK', id: 'task-1' },
      ],
    };
    expect(canonicalizeScope(raw)).toEqual({
      resources: [
        { kind: 'RESOURCE_INSTANCE', id: 'instance-1' },
        { kind: 'RESOURCE_TASK', id: 'task-1' },
        { kind: 'RESOURCE_TASK', id: 'task-2' },
      ],
    });
    expect(() => canonicalizeScope({ resources: [] })).toThrow(/at least one/);
    const h = makeStream({ selection: { channel: 'operations', owner: 'runtime', scope: raw } });
    h.stream.start();
    h.sockets.last.open();
    expect(h.sockets.last.firstFrame?.scope).toEqual({
      resources: [
        { kind: 'RESOURCE_INSTANCE', id: 'instance-1' },
        { kind: 'RESOURCE_TASK', id: 'task-1' },
        { kind: 'RESOURCE_TASK', id: 'task-2' },
      ],
    });
  });
});

describe('session ready, cursor separation and token rotation', () => {
  it('records the processed Control sequence and uint64 owner cursor before notifying the view', () => {
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.receive(readyFrame({ last_sequence: 7, owner_cursor: cursor('18446744073709551615') }));
    expect(h.readies).toHaveLength(1);
    expect(h.readies[0]).toMatchObject({ lastSequence: 7, resumed: false, sessionId: READY_SESSION });
    // uint64 stays a decimal string; converting it to a JS number would lose precision.
    expect(h.readies[0].ownerCursor?.sequence).toBe('18446744073709551615');
    expect(typeof h.stream.currentState.ownerCursor?.sequence).toBe('string');
    expect(h.stream.status).toBe('connected');
    expect(h.stream.lastConfirmedAt).toBeTypeOf('number');
    expect(compareOwnerSequence('9', '10')).toBeLessThan(0);
  });

  it('resumes with the retained session/cursor and rotates the token without losing them', () => {
    vi.useFakeTimers();
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.receive(readyFrame({ last_sequence: 7, owner_cursor: cursor('41') }));
    h.sockets.last.closeWith(TARGET_CLOSE_CODE.shutdown);
    expect(h.stream.status).toBe('disconnected');
    vi.advanceTimersByTime(1000);
    expect(h.sockets.sockets).toHaveLength(2);
    h.sockets.last.open();
    expect(h.sockets.last.firstFrame).toEqual({
      type: 'resume', access_token: 'access-token-1', session_id: READY_SESSION,
      last_sequence: 7, scope, owner_cursor: cursor('41'),
    });
    // Token rotation re-authenticates the same Identity session: resume with the new token.
    h.token.value = 'access-token-2';
    h.stream.reauthenticate();
    expect(h.sockets.sockets).toHaveLength(3);
    h.sockets.last.open();
    expect(h.sockets.last.firstFrame).toEqual({
      type: 'resume', access_token: 'access-token-2', session_id: READY_SESSION,
      last_sequence: 7, scope, owner_cursor: cursor('41'),
    });
  });

  it('resumes even when the disconnect happens before the first event', () => {
    vi.useFakeTimers();
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.receive(readyFrame({ last_sequence: 3, owner_cursor: cursor('3') }));
    expect(h.events).toHaveLength(0); // no live/replay event delivered yet
    h.sockets.last.closeWith(TARGET_CLOSE_CODE.shutdown);
    vi.advanceTimersByTime(1000);
    h.sockets.last.open();
    expect(h.sockets.last.firstFrame).toEqual({
      type: 'resume', access_token: 'access-token-1', session_id: READY_SESSION,
      last_sequence: 3, scope, owner_cursor: cursor('3'),
    });
  });

  it('destroys the old socket and ignores its messages when the scope switches', () => {
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.receive(readyFrame({ last_sequence: 3, owner_cursor: cursor('3') }));
    const old = h.sockets.last;
    h.stream.setSelection({
      channel: 'operations', owner: 'runtime',
      scope: { resources: [{ kind: 'RESOURCE_INSTANCE', id: 'instance-2' }] },
    });
    expect(old.closed).toBe(true);
    expect(h.sockets.sockets).toHaveLength(2);
    h.sockets.last.open();
    expect(h.sockets.last.firstFrame).toEqual({
      type: 'hello', access_token: 'access-token-1', last_sequence: 0,
      scope: { resources: [{ kind: 'RESOURCE_INSTANCE', id: 'instance-2' }] },
    });
    old.closed = false; // simulate an in-flight frame arriving after the switch
    old.receive(readyFrame({ last_sequence: 99 }));
    old.receive(eventFrame(100));
    expect(h.events).toHaveLength(0);
    expect(h.stream.lastSequence).toBe(0);
    expect(h.stream.ownerCursor).toBeUndefined();
  });
});

describe('sequence ordering, replay and recovery', () => {
  it('dedupes duplicate/replayed sequences and keeps replay in order', () => {
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.receive(readyFrame({ last_sequence: 1, owner_cursor: cursor('1') }));
    h.sockets.last.receive(eventFrame(2));
    expect(h.events.map((event) => event.sequence)).toEqual([2]);
    h.sockets.last.receive(eventFrame(2)); // duplicate during replay
    h.sockets.last.receive(eventFrame(1)); // already processed before resume
    expect(h.events.map((event) => event.sequence)).toEqual([2]);
    h.sockets.last.receive(eventFrame(3));
    expect(h.events.map((event) => event.sequence)).toEqual([2, 3]);
    expect(h.stream.ownerCursor?.sequence).toBe('3');
  });

  it('keeps the owner cursor independent from the Control sequence', () => {
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.receive(readyFrame({ last_sequence: 0, owner_cursor: cursor('100') }));
    // Control sequence moves by 1 while the owner boundary jumps to a different dataset offset.
    h.sockets.last.receive(eventFrame(1, { owner_cursor: cursor('105', 'epoch-1') }));
    expect(h.stream.lastSequence).toBe(1);
    expect(h.stream.ownerCursor?.sequence).toBe('105');
    // A frame whose owner boundary does not advance is not trusted as live.
    h.sockets.last.receive(eventFrame(2, { owner_cursor: cursor('105', 'epoch-1') }));
    expect(h.recoveries[0]?.trigger).toBe('cursor-mismatch');
  });

  it('uses TargetScopedHello/Resume without an owner cursor on non-operations channels', () => {
    vi.useFakeTimers();
    const sockets = new FakeTargetSockets();
    const stream = new TargetScopedStream({
      url: targetChannelUrl('ws://mock/ws', 'control', 'control'),
      selection: { channel: 'control', owner: 'control', scope },
      factory: sockets.factory,
      tokenProvider: () => 'access-token-1',
      now: () => 1,
    });
    active.push(stream);
    stream.start(); sockets.last.open();
    expect(sockets.last.firstFrame).toEqual({ type: 'hello', access_token: 'access-token-1', last_sequence: 0, scope });
    sockets.last.receive({ type: 'SESSION_READY', session_id: READY_SESSION, last_sequence: 2 });
    sockets.last.closeWith(TARGET_CLOSE_CODE.shutdown);
    vi.advanceTimersByTime(1000);
    sockets.last.open();
    expect(sockets.last.firstFrame).toEqual({
      type: 'resume', access_token: 'access-token-1', session_id: READY_SESSION, last_sequence: 2, scope,
    });
    expect(sockets.last.firstFrame?.owner_cursor).toBeUndefined();
  });

  it('enters recovery on a sequence gap instead of treating the server head as processed', () => {
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.receive(readyFrame({ last_sequence: 0, owner_cursor: cursor('0') }));
    h.sockets.last.receive(eventFrame(5));
    expect(h.events).toHaveLength(0);
    expect(h.recoveries).toEqual([{ trigger: 'sequence-gap', detail: 'expected 1' }]);
    expect(h.sockets.sockets).toHaveLength(2);
    h.sockets.last.open();
    expect(h.sockets.last.firstFrame?.type).toBe('hello'); // session capability discarded
    expect(h.stream.lastSequence).toBe(0);
  });

  it('clears the session on RESYNC_REQUIRED and starts a fresh hello session', () => {
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.receive(readyFrame({ last_sequence: 5, owner_cursor: cursor('9') }));
    h.sockets.last.receive({ type: 'RESYNC_REQUIRED', reason: 'CURSOR_EXPIRED' });
    expect(h.recoveries).toEqual([{ trigger: 'resync', reason: 'CURSOR_EXPIRED' }]);
    expect(h.statuses).toContain('resync');
    expect(h.sockets.sockets).toHaveLength(2);
    h.sockets.last.open();
    expect(h.sockets.last.firstFrame).toEqual({ type: 'hello', access_token: 'access-token-1', last_sequence: 0, scope });
    expect(h.stream.ownerCursor).toBeUndefined();
  });

  it('enters recovery on an owner cursor epoch/scope mismatch and malformed contract frames', () => {
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.receive(readyFrame({ last_sequence: 0, owner_cursor: cursor('0') }));
    h.sockets.last.receive(eventFrame(1, { owner_cursor: cursor('1', 'epoch-2') }));
    expect(h.recoveries[0]?.trigger).toBe('cursor-mismatch');
    expect(h.sockets.sockets).toHaveLength(2);
    h.sockets.last.open();
    h.sockets.last.receive(readyFrame({ last_sequence: 0, owner_cursor: cursor('0') }));
    h.sockets.last.receive(eventFrame(1, { occurred_at: undefined }));
    expect(h.recoveries[1]?.trigger).toBe('invalid-frame');
    expect(h.events).toHaveLength(0);
  });

  it('is forward compatible with unknown frames/event types but never guesses malformed envelopes', () => {
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.receive(readyFrame({ last_sequence: 0, owner_cursor: cursor('0') }));
    h.sockets.last.receive({ type: 'FUTURE_SERVER_FRAME', anything: true });
    expect(h.recoveries).toHaveLength(0);
    h.sockets.last.receive(eventFrame(1, { event_type: 'future.event' }));
    expect(h.events.map((event) => event.event_type)).toEqual(['future.event']);
    expect(h.stream.lastSequence).toBe(1);
    h.sockets.last.receive('not-json');
    expect(h.recoveries.map((recovery) => recovery.trigger)).toEqual(['invalid-frame']);
  });

  it('fails closed instead of spinning when a peer keeps answering recovery with invalid frames', () => {
    const h = makeStream();
    h.stream.start();
    for (let attempt = 0; attempt < 4; attempt += 1) h.sockets.last.receive('not-json');
    expect(h.stream.status).toBe('protocol');
    expect(h.sockets.sockets).toHaveLength(4); // initial socket + 3 bounded recovery attempts
    h.sockets.last.receive('not-json');
    expect(h.sockets.sockets).toHaveLength(4);
  });

  it('rejects binary frames and malformed SESSION_READY payloads', () => {
    expect(parseTargetFrame(new ArrayBuffer(2), { channel: 'operations', owner: 'runtime', scopeKey: scopeKey(scope) }))
      .toEqual({ kind: 'invalid', detail: 'frame is not text' });
    expect(parseTargetFrame(JSON.stringify({ type: 'SESSION_READY', session_id: 'short', last_sequence: 0 }),
      { channel: 'control', scopeKey: scopeKey(scope) })).toMatchObject({ kind: 'invalid' });
    expect(parseTargetFrame(JSON.stringify(readyFrame()), { channel: 'control', scopeKey: scopeKey(scope) }))
      .toMatchObject({ kind: 'ready' });
  });
});

describe('close codes, first frame deadline and backoff', () => {
  it('returns to the login flow on 4401 and stops', () => {
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.receive(readyFrame());
    h.sockets.last.closeWith(TARGET_CLOSE_CODE.unauthenticated);
    expect(h.unauthorized.count).toBe(1);
    expect(h.stream.status).toBe('unauthorized');
    expect(h.sockets.sockets).toHaveLength(1);
  });

  it.each([
    [TARGET_CLOSE_CODE.forbidden, 'forbidden'],
    [TARGET_CLOSE_CODE.invalidFirstFrame, 'protocol'],
    [TARGET_CLOSE_CODE.slowConsumer, 'throttled'],
  ])('stops automatic retries on terminal close code %s', (code, expected) => {
    vi.useFakeTimers();
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.closeWith(code);
    expect(h.stream.status).toBe(expected);
    vi.advanceTimersByTime(60_000);
    expect(h.sockets.sockets).toHaveLength(1);
  });

  it('backs off and resumes after transient 1011/1001 closes', () => {
    vi.useFakeTimers();
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.receive(readyFrame({ last_sequence: 2, owner_cursor: cursor('2') }));
    h.sockets.last.closeWith(TARGET_CLOSE_CODE.internal);
    expect(h.stream.status).toBe('disconnected');
    vi.advanceTimersByTime(999);
    expect(h.sockets.sockets).toHaveLength(1); // 1s first backoff
    vi.advanceTimersByTime(1);
    expect(h.sockets.sockets).toHaveLength(2);
    h.sockets.last.open();
    expect(h.sockets.last.firstFrame).toMatchObject({ type: 'resume', last_sequence: 2 });
  });

  it('closes a socket that misses the 5s first-frame deadline and falls back to retry', () => {
    vi.useFakeTimers();
    const h = makeStream();
    h.stream.start();
    expect(h.sockets.sockets).toHaveLength(1);
    const first = h.sockets.last;
    vi.advanceTimersByTime(TARGET_FIRST_FRAME_TIMEOUT_MS - 1);
    expect(first.closed).toBe(false);
    vi.advanceTimersByTime(1);
    expect(first.closed).toBe(true);
    expect(h.stream.status).toBe('disconnected');
    expect(h.recoveries.map((recovery) => recovery.trigger)).toContain('first-frame-timeout');
    vi.advanceTimersByTime(1000);
    expect(h.sockets.sockets).toHaveLength(2);
  });

  it('stops cleanly and ignores late frames after stop()', () => {
    const h = makeStream();
    h.stream.start(); h.sockets.last.open();
    h.sockets.last.receive(readyFrame());
    const socket = h.sockets.last;
    h.stream.stop();
    socket.closed = false;
    socket.receive(eventFrame(1));
    expect(h.events).toHaveLength(0);
    expect(h.stream.status).toBe('stopped');
  });
});
