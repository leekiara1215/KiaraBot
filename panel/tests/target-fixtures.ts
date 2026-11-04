/** Deterministic fake WebSocket transport for the target scoped client tests. */
import type { SocketLike } from '../src/stream/control-stream';

export class FakeTargetSocket implements SocketLike {
  onopen: ((event: Event) => void) | null = null;
  onmessage: ((event: MessageEvent) => void) | null = null;
  onclose: ((event: CloseEvent) => void) | null = null;
  onerror: ((event: Event) => void) | null = null;
  sent: unknown[] = [];
  closed = false;
  constructor(readonly url: string) {}
  open() { if (!this.closed) this.onopen?.(new Event('open')); }
  send(data: string) { if (!this.closed) this.sent.push(JSON.parse(data)); }
  receive(value: unknown) {
    if (this.closed) return;
    this.onmessage?.(new MessageEvent('message', { data: typeof value === 'string' ? value : JSON.stringify(value) }));
  }
  closeWith(code: number) {
    if (this.closed) return;
    this.closed = true;
    this.onclose?.(new CloseEvent('close', { code }));
  }
  failWith(code: number) { this.closeWith(code); }
  close() { this.closeWith(1000); }
  get firstFrame(): Record<string, unknown> | undefined { return this.sent[0] as Record<string, unknown> | undefined; }
}

export class FakeTargetSockets {
  sockets: FakeTargetSocket[] = [];
  factory = (url: string): SocketLike => {
    const socket = new FakeTargetSocket(url);
    this.sockets.push(socket);
    return socket;
  };
  get last(): FakeTargetSocket { return this.sockets[this.sockets.length - 1]; }
}

/** A transport that never opens or answers: proves REST is not blocked by stream setup. */
export class SilentSocket implements SocketLike {
  onopen: ((event: Event) => void) | null = null;
  onmessage: ((event: MessageEvent) => void) | null = null;
  onclose: ((event: CloseEvent) => void) | null = null;
  onerror: ((event: Event) => void) | null = null;
  sent: unknown[] = [];
  send(data: string) { this.sent.push(JSON.parse(data)); }
  close() { /* never confirms a close either: the panel must still fall back to REST */ }
}

export const silentSocketFactory = () => new SilentSocket();
