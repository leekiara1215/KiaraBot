/**
 * Target Control scoped WebSocket client (WS-06 panel side).
 *
 * One socket binds exactly one channel, one owner and one canonical exact scope. The client owns
 * connection state and wire bookkeeping only; REST owner snapshots remain the authoritative
 * recovery source and stream notifications never prove completion.
 *
 * Recovery invariants:
 * - SESSION_READY acknowledges the client's *processed* Control sequence. It is never treated as
 *   the server's undelivered head, and it is separate from `owner_cursor` (a decimal uint64
 *   string bound to one owner/dataset/scope).
 * - A duplicate sequence is dropped; a sequence gap, malformed contract frame, cursor/epoch/scope
 *   mismatch or RESYNC_REQUIRED discards the old session capability and opens a fresh hello while
 *   asking the UI to re-read authoritative REST.
 * - Token rotation keeps the same Identity session: `reauthenticate()` closes the current socket
 *   and resumes with the token returned by the provider, never with a stale access token.
 * - A stale socket (after stop/reauthenticate/scope change) can never advance the new cursor; its
 *   handlers are ignored through socket identity checks.
 */
import {
  TARGET_CLOSE_CODE,
  TARGET_FIRST_FRAME_TIMEOUT_MS,
  canonicalizeScope,
  channelRequiresOwnerCursor,
  compareOwnerSequence,
  parseTargetFrame,
  scopeKey,
  targetHelloFrame,
  targetResumeFrame,
  type FrameContext,
  type SocketFactory,
  type SocketLike,
  type TargetChannel,
  type TargetChannelSelection,
  type TargetEventEnvelope,
  type TargetOwnerCursor,
  type TargetResyncReason,
  type TargetScope,
} from './target-protocol';

export interface TargetStreamSelection extends TargetChannelSelection {
  scope: TargetScope;
}

/**
 * Everything a view needs to open a target scoped channel. The App owns one token provider so the
 * status pages never copy access tokens (and token rotation keeps the same provider).
 */
export interface OperationsStreamConfig {
  /** Base WS URL, e.g. `/ws/control`; the client derives `/ws/operations?owner_service=...`. */
  wsUrl: string;
  socketFactory: SocketFactory;
  tokenProvider: () => string;
}

export type TargetStreamStatus =
  | 'idle' | 'connecting' | 'connected' | 'recovering' | 'resync' | 'disconnected'
  | 'unauthorized' | 'forbidden' | 'throttled' | 'protocol' | 'stopped';

export interface TargetStreamReady {
  sessionId: string;
  /** Processed Control sequence acknowledged by the server. */
  lastSequence: number;
  /** Owner/dataset/scope boundary at that exact accepted sequence (operations channel). */
  ownerCursor?: TargetOwnerCursor;
  /** True when the client sent a resume frame; false for a fresh hello session. */
  resumed: boolean;
  at: number;
}

export type TargetRecoveryTrigger =
  | 'resync' | 'sequence-gap' | 'invalid-frame' | 'cursor-mismatch' | 'first-frame-timeout';

export interface TargetRecovery {
  trigger: TargetRecoveryTrigger;
  reason?: TargetResyncReason;
  /** Stable, non-secret diagnostic used only for status/verification, never rendered as data. */
  detail?: string;
}

export interface TargetStreamOptions {
  /** Full channel URL built by `targetChannelUrl` (owner_service lives in the URL, token never). */
  url: string;
  selection: TargetStreamSelection;
  factory: SocketFactory;
  /** Reads the current access token at every (re)connect; token rotation never needs new wiring. */
  tokenProvider: () => string;
  onStatus?: (status: TargetStreamStatus) => void;
  onReady?: (ready: TargetStreamReady) => void;
  onEvent?: (event: TargetEventEnvelope) => void;
  onRecovery?: (recovery: TargetRecovery) => void;
  onUnauthorized?: () => void;
  now?: () => number;
  firstFrameTimeoutMs?: number;
  baseBackoffMs?: number;
  maxBackoffMs?: number;
}

export interface TargetStreamState {
  status: TargetStreamStatus;
  sessionId?: string;
  lastSequence: number;
  ownerCursor?: TargetOwnerCursor;
  lastConfirmedAt?: number;
}

const initialState: TargetStreamState = { status: 'idle', lastSequence: 0 };

/**
 * A peer that keeps answering recovery with another malformed/resync frame must not spin forever
 * (a broken/unsupported target channel fails closed instead of opening sockets endlessly).
 */
const MAX_CONSECUTIVE_RECOVERIES = 3;

export class TargetScopedStream {
  private readonly now: () => number;
  private readonly firstFrameTimeoutMs: number;
  private readonly baseBackoffMs: number;
  private readonly maxBackoffMs: number;
  private selection: TargetStreamSelection;
  private state: TargetStreamState = { ...initialState };
  private socket?: SocketLike;
  private reconnectTimer?: ReturnType<typeof setTimeout>;
  private firstFrameTimer?: ReturnType<typeof setTimeout>;
  private started = false;
  private stopped = true;
  private retry = 0;
  private recoveryAttempts = 0;
  private reauthenticating = false;
  private timedOut = false;

  constructor(private readonly options: TargetStreamOptions) {
    this.now = options.now ?? (() => Date.now());
    this.firstFrameTimeoutMs = options.firstFrameTimeoutMs ?? TARGET_FIRST_FRAME_TIMEOUT_MS;
    this.baseBackoffMs = options.baseBackoffMs ?? 1000;
    this.maxBackoffMs = options.maxBackoffMs ?? 16000;
    this.selection = {
      channel: options.selection.channel,
      owner: options.selection.owner,
      scope: canonicalizeScope(options.selection.scope),
    };
  }

  get currentState(): TargetStreamState { return { ...this.state }; }
  get status(): TargetStreamStatus { return this.state.status; }
  get lastConfirmedAt(): number | undefined { return this.state.lastConfirmedAt; }
  get lastSequence(): number { return this.state.lastSequence; }
  get ownerCursor(): TargetOwnerCursor | undefined { return this.state.ownerCursor; }
  get currentSelection(): TargetStreamSelection { return this.selection; }
  get scopeKey(): string { return scopeKey(this.selection.scope); }

  start() {
    if (this.started) return;
    this.started = true;
    this.stopped = false;
    this.open();
  }

  stop() {
    if (this.stopped && !this.started) return;
    this.started = false;
    this.stopped = true;
    this.clearTimers();
    const socket = this.socket;
    this.socket = undefined;
    this.setState({ status: 'stopped' });
    socket?.close();
  }

  /**
   * Switch channel/owner/scope: the old socket is destroyed and cannot advance the new cursor,
   * and the session capability/cursor are discarded because they were bound to the old scope.
   */
  setSelection(selection: TargetStreamSelection) {
    const owner = selection.owner;
    if (selection.channel === this.selection.channel && owner === this.selection.owner
      && scopeKey(selection.scope) === scopeKey(this.selection.scope)) return;
    const previous = this.socket;
    this.socket = undefined;
    this.clearTimers();
    previous?.close();
    this.selection = {
      channel: selection.channel,
      owner,
      scope: canonicalizeScope(selection.scope),
    };
    this.resetSession();
    this.setState({ status: 'idle' });
    if (this.started && !this.stopped) this.open();
  }

  /** Reconnect under the same session/cursor with the provider's current token. */
  reauthenticate() {
    if (this.stopped || !this.started) return;
    if (!this.socket) {
      this.clearReconnectTimer();
      this.open();
      return;
    }
    this.reauthenticating = true;
    this.socket.close();
  }

  private resetSession() {
    this.state = { ...this.state, sessionId: undefined, lastSequence: 0, ownerCursor: undefined };
    this.timedOut = false;
    this.retry = 0;
  }

  private setState(patch: Partial<TargetStreamState>) {
    const next = { ...this.state, ...patch };
    const changed = next.status !== this.state.status;
    this.state = next;
    if (changed) this.options.onStatus?.(next.status);
  }

  private clearReconnectTimer() {
    if (this.reconnectTimer !== undefined) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = undefined;
    }
  }

  private clearFirstFrameTimer() {
    if (this.firstFrameTimer !== undefined) {
      clearTimeout(this.firstFrameTimer);
      this.firstFrameTimer = undefined;
    }
  }

  private clearTimers() {
    this.clearReconnectTimer();
    this.clearFirstFrameTimer();
  }

  private context(): FrameContext {
    return { channel: this.selection.channel, owner: this.selection.owner, scopeKey: this.scopeKey };
  }

  private open() {
    if (this.stopped || !this.started) return;
    this.clearReconnectTimer();
    this.setState({ status: 'connecting' });
    const socket = this.options.factory(this.options.url);
    this.socket = socket;
    this.firstFrameTimer = setTimeout(() => {
      if (this.socket !== socket) return;
      this.firstFrameTimer = undefined;
      this.timedOut = true;
      socket.close();
    }, this.firstFrameTimeoutMs);
    socket.onopen = () => {
      if (this.socket !== socket || this.stopped) return;
      this.sendFirstFrame(socket);
    };
    socket.onmessage = (event: MessageEvent) => {
      if (this.socket !== socket || this.stopped) return;
      this.receive(event.data);
    };
    socket.onerror = () => {
      if (this.socket !== socket || this.stopped) return;
      socket.close();
    };
    socket.onclose = (event: CloseEvent) => {
      if (this.socket !== socket) return;
      this.socket = undefined;
      this.clearFirstFrameTimer();
      if (this.reauthenticating) {
        this.reauthenticating = false;
        this.open();
        return;
      }
      if (this.stopped) return;
      if (this.timedOut) {
        this.timedOut = false;
        this.setState({ status: 'disconnected' });
        this.options.onRecovery?.({ trigger: 'first-frame-timeout' });
        this.scheduleReconnect();
        return;
      }
      this.handleClose(event.code);
    };
  }

  private sendFirstFrame(socket: SocketLike) {
    const token = this.options.tokenProvider();
    if (!token) {
      this.setState({ status: 'unauthorized' });
      this.stopped = true;
      socket.close();
      return;
    }
    const resumable = this.state.sessionId
      && (!channelRequiresOwnerCursor(this.selection.channel) || this.state.ownerCursor);
    const frame = resumable
      ? targetResumeFrame(this.selection, {
        accessToken: token,
        sessionId: this.state.sessionId!,
        lastSequence: this.state.lastSequence,
        ownerCursor: this.state.ownerCursor,
      })
      : targetHelloFrame(this.selection, token);
    socket.send(JSON.stringify(frame));
  }

  private receive(raw: unknown) {
    const frame = parseTargetFrame(raw, this.context());
    switch (frame.kind) {
      case 'ready': return this.acceptReady(frame.ready);
      case 'resync':
        this.setState({ status: 'resync' });
        return this.beginSessionRecovery({ trigger: 'resync', reason: frame.reason });
      case 'event': return this.acceptEvent(frame.event);
      case 'invalid': return this.beginSessionRecovery({ trigger: 'invalid-frame', detail: frame.detail });
      case 'unknown': return;
    }
  }

  private acceptReady(ready: { session_id: string; last_sequence: number; owner_cursor?: TargetOwnerCursor }) {
    this.clearFirstFrameTimer();
    const previousCursor = this.state.ownerCursor;
    if (previousCursor && ready.owner_cursor && (
      previousCursor.source_service !== ready.owner_cursor.source_service
      || previousCursor.epoch !== ready.owner_cursor.epoch
      || scopeKey(previousCursor.scope) !== scopeKey(ready.owner_cursor.scope))) {
      this.beginSessionRecovery({ trigger: 'cursor-mismatch', detail: 'resumed owner cursor boundary changed' });
      return;
    }
    const resumed = Boolean(this.state.sessionId);
    this.retry = 0;
    this.recoveryAttempts = 0;
    this.setState({
      status: 'connected',
      sessionId: ready.session_id,
      lastSequence: ready.last_sequence,
      ownerCursor: ready.owner_cursor,
      lastConfirmedAt: this.now(),
    });
    this.options.onReady?.({
      sessionId: ready.session_id,
      lastSequence: ready.last_sequence,
      ownerCursor: ready.owner_cursor,
      resumed,
      at: this.now(),
    });
  }

  private acceptEvent(event: TargetEventEnvelope) {
    if (event.sequence <= this.state.lastSequence) return; // duplicate (replay or server retry)
    if (event.sequence !== this.state.lastSequence + 1) {
      this.beginSessionRecovery({ trigger: 'sequence-gap', detail: `expected ${this.state.lastSequence + 1}` });
      return;
    }
    if (this.selection.channel === 'operations' && event.owner_cursor) {
      const previous = this.state.ownerCursor;
      if (previous && (
        previous.source_service !== event.owner_cursor.source_service
        || previous.epoch !== event.owner_cursor.epoch
        || scopeKey(previous.scope) !== scopeKey(event.owner_cursor.scope))) {
        this.beginSessionRecovery({ trigger: 'cursor-mismatch', detail: 'owner cursor boundary changed' });
        return;
      }
      if (previous && compareOwnerSequence(event.owner_cursor.sequence, previous.sequence) <= 0) {
        // The Control sequence advanced while the owner boundary did not: the session cannot be
        // trusted to be gap-free, so recover from REST instead of skipping the notification.
        this.beginSessionRecovery({ trigger: 'cursor-mismatch', detail: 'owner cursor did not advance' });
        return;
      }
    }
    this.setState({
      lastSequence: event.sequence,
      ownerCursor: event.owner_cursor ?? this.state.ownerCursor,
      lastConfirmedAt: this.now(),
    });
    this.options.onEvent?.(event);
  }

  /**
   * Discard the session capability (cursor no longer authoritative), report recovery to the UI and
   * open a fresh hello. REST is re-read by the UI; a stream session is never replayed from a gap.
   */
  private beginSessionRecovery(recovery: TargetRecovery) {
    const previous = this.socket;
    this.socket = undefined;
    this.clearTimers();
    this.resetSession();
    previous?.close();
    this.recoveryAttempts += 1;
    if (this.recoveryAttempts > MAX_CONSECUTIVE_RECOVERIES) {
      this.terminate('protocol');
      return;
    }
    this.setState({ status: 'recovering' });
    this.options.onRecovery?.(recovery);
    if (this.stopped || !this.started) return;
    this.open();
  }

  private handleClose(code: number) {
    this.clearFirstFrameTimer();
    if (code === TARGET_CLOSE_CODE.unauthenticated) {
      this.terminate('unauthorized');
      this.options.onUnauthorized?.();
      return;
    }
    if (code === TARGET_CLOSE_CODE.forbidden) {
      this.terminate('forbidden');
      return;
    }
    if (code === TARGET_CLOSE_CODE.invalidFirstFrame) {
      this.terminate('protocol');
      return;
    }
    if (code === TARGET_CLOSE_CODE.slowConsumer) {
      this.terminate('throttled');
      return;
    }
    // 1000/1001/1011 and unknown codes are transient: resume (when a session exists) with backoff.
    this.setState({ status: 'disconnected' });
    this.scheduleReconnect();
  }

  private terminate(status: TargetStreamStatus) {
    this.started = false;
    this.stopped = true;
    this.clearTimers();
    this.setState({ status });
  }

  private scheduleReconnect() {
    if (this.stopped || !this.started) return;
    const delay = Math.min(this.baseBackoffMs * 2 ** this.retry, this.maxBackoffMs);
    this.retry += 1;
    this.clearReconnectTimer();
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = undefined;
      this.open();
    }, delay);
  }
}
