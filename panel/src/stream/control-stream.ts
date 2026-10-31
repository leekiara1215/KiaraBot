import type { Hello, Resume, SessionReady, ResyncRequired } from '../generated/asyncapi';

export interface SocketLike {
  onopen: ((event: Event) => void) | null;
  onmessage: ((event: MessageEvent) => void) | null;
  onclose: ((event: CloseEvent) => void) | null;
  onerror: ((event: Event) => void) | null;
  send(data: string): void;
  close(): void;
}
export type SocketFactory = (url: string) => SocketLike;
export type StreamStatus = 'connecting' | 'connected' | 'disconnected' | 'resync' | 'forbidden' | 'throttled' | 'protocol';

/**
 * Stream event types the panel refreshes on. Derived from the AsyncAPI envelopes
 * marked implemented: `health.updated`, `message.updated` and the messages
 * channel carry `x-contract-status: not_implemented` in api/asyncapi.yaml and
 * must not be treated as live signals. `scripts/check-contracts.mjs` compares
 * this list with the contract's implemented event types.
 */
export const KNOWN_EVENTS = ['log.updated', 'adapter.updated', 'plugin.updated', 'task.updated'] as const;
export type KnownEvent = (typeof KNOWN_EVENTS)[number];

export class ControlStream {
  private socket?: SocketLike;
  private timer?: ReturnType<typeof setTimeout>;
  private sessionId?: string;
  private sequence = 0;
  private retry = 0;
  private stopped = true;
  private resyncing = false;
  constructor(private readonly options: { url: string; accessToken: string; factory: SocketFactory; onStatus: (status: StreamStatus) => void; onRefresh: () => void; onUnauthorized?: () => void }) {}
  start() { this.stopped = false; this.open(); }
  stop() { this.stopped = true; if (this.timer) clearTimeout(this.timer); this.socket?.close(); }
  private open() {
    if (this.stopped) return;
    this.options.onStatus('connecting');
    const socket = this.options.factory(this.options.url);
    this.socket = socket;
    socket.onopen = () => {
      if (this.stopped || this.socket !== socket) return;
      const frame: Hello | Resume = this.sessionId ? { type: 'resume', access_token: this.options.accessToken, session_id: this.sessionId, last_sequence: this.sequence } : { type: 'hello', access_token: this.options.accessToken, last_sequence: 0 };
      socket.send(JSON.stringify(frame));
    };
    socket.onmessage = (event) => { if (!this.stopped && this.socket === socket) this.receive(event.data); };
    socket.onerror = () => socket.close();
    socket.onclose = (event) => {
      if (this.stopped || this.socket !== socket) return;
      // 4401: credentials rejected -> hand control back to the login flow.
      if (event.code === 4401) { this.stop(); this.options.onUnauthorized?.(); return; }
      // Terminal stream failures: retrying cannot fix them, so stop and expose a precise state.
      if (event.code === 4403) { this.stop(); this.options.onStatus('forbidden'); return; }
      if (event.code === 4400) { this.stop(); this.options.onStatus('protocol'); return; }
      if (event.code === 1013) { this.stop(); this.options.onStatus('throttled'); return; }
      // RESYNC_REQUIRED closes the socket deliberately; resume with a fresh hello instead of
      // downgrading the status to "disconnected" while the retry is already scheduled.
      if (this.resyncing) { this.resyncing = false; this.options.onStatus('connecting'); } else this.options.onStatus('disconnected');
      const delay = Math.min(1000 * 2 ** this.retry++, 16000);
      this.timer = setTimeout(() => this.open(), delay);
    };
  }
  private receive(data: unknown) {
    if (typeof data !== 'string') return;
    let value: unknown;
    try { value = JSON.parse(data); } catch { return; }
    if (!value || typeof value !== 'object') return;
    const message = value as Record<string, unknown>;
    if (message.type === 'SESSION_READY' && typeof message.session_id === 'string' && message.session_id.length >= 32 && Number.isSafeInteger(message.last_sequence) && (message.last_sequence as number) >= 0) {
      const ready = message as unknown as SessionReady;
      this.sessionId = ready.session_id; this.sequence = Math.max(this.sequence, ready.last_sequence); this.retry = 0; this.options.onStatus('connected');
      this.options.onRefresh();
      return;
    }
    if (message.type === 'RESYNC_REQUIRED' && typeof message.reason === 'string') {
      void (message as unknown as ResyncRequired);
      this.sessionId = undefined; this.sequence = 0; this.resyncing = true; this.options.onStatus('resync'); this.socket?.close(); return;
    }
    if (typeof message.event_type !== 'string' || !Number.isSafeInteger(message.sequence) || (message.sequence as number) < 0 || typeof message.schema_version !== 'string' || typeof message.occurred_at !== 'string' || !message.payload || typeof message.payload !== 'object') return;
    const event = message as { sequence: number; event_type: string };
    if (event.sequence <= this.sequence) return;
    this.sequence = event.sequence;
    if ((KNOWN_EVENTS as readonly string[]).includes(event.event_type)) this.options.onRefresh();
  }
}
