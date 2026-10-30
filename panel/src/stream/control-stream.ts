import type { Hello, Resume, SessionReady, ResyncRequired, StreamEnvelope } from '../generated/asyncapi';

export interface SocketLike {
  onopen: ((event: Event) => void) | null;
  onmessage: ((event: MessageEvent) => void) | null;
  onclose: ((event: CloseEvent) => void) | null;
  onerror: ((event: Event) => void) | null;
  send(data: string): void;
  close(): void;
}
export type SocketFactory = (url: string) => SocketLike;
export type StreamStatus = 'connecting' | 'connected' | 'disconnected' | 'resync';
export class ControlStream {
  private socket?: SocketLike;
  private timer?: ReturnType<typeof setTimeout>;
  private sessionId?: string;
  private sequence = 0;
  private retry = 0;
  private stopped = true;
  private needsSnapshot = false;
  constructor(private readonly options: { url: string; accessToken: string; factory: SocketFactory; onStatus: (status: StreamStatus) => void; onRefresh: () => void }) {}
  start() { this.stopped = false; this.open(); }
  stop() { this.stopped = true; if (this.timer) clearTimeout(this.timer); this.socket?.close(); }
  private open() {
    if (this.stopped) return;
    this.options.onStatus('connecting');
    const socket = this.options.factory(this.options.url);
    this.socket = socket;
    socket.onopen = () => {
      const frame: Hello | Resume = this.sessionId ? { type: 'resume', session_id: this.sessionId, last_sequence: this.sequence } : { type: 'hello', access_token: this.options.accessToken, last_sequence: this.sequence };
      socket.send(JSON.stringify(frame));
    };
    socket.onmessage = (event) => this.receive(event.data);
    socket.onerror = () => socket.close();
    socket.onclose = () => {
      if (this.stopped) return;
      this.options.onStatus('disconnected');
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
      if (this.needsSnapshot) { this.needsSnapshot = false; this.options.onRefresh(); }
      return;
    }
    if (message.type === 'RESYNC_REQUIRED' && typeof message.reason === 'string') {
      void (message as unknown as ResyncRequired);
      this.sessionId = undefined; this.sequence = 0; this.needsSnapshot = true; this.options.onStatus('resync'); this.socket?.close(); return;
    }
    if (typeof message.event_type !== 'string' || !Number.isSafeInteger(message.sequence) || (message.sequence as number) < 0 || typeof message.schema_version !== 'string' || typeof message.occurred_at !== 'string' || !message.payload || typeof message.payload !== 'object') return;
    const event = message as unknown as StreamEnvelope;
    if (event.sequence <= this.sequence) return;
    this.sequence = event.sequence;
    if (event.event_type === 'log.updated') this.options.onRefresh();
  }
}
