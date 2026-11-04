/**
 * Target Control WebSocket wire protocol.
 *
 * Single source of truth is `api/asyncapi.yaml` (WS-01 target contract); this module only
 * encodes/validates those frozen frames for the panel. It deliberately owns no business rules:
 * REST owner snapshots stay authoritative and stream notifications never prove completion.
 *
 * Frozen by the contract and asserted by `scripts/check-contracts.mjs`:
 * - close codes 4400/4401/4403/1011/1013/1001 (x-control-session-policy.close_codes);
 * - RESYNC_REQUIRED reasons CURSOR_EXPIRED/INVALID_CURSOR/SERVER_RESTART;
 * - 5s first-frame deadline, text frames, 1..127 canonical exact resources;
 * - Control `sequence` is a JS safe integer while `owner_cursor.sequence` is a decimal uint64
 *   string. The two spaces are never mixed: `sequence` dedupes this stream session,
 *   `owner_cursor` is the owner/dataset/scope boundary a resume sends back.
 */
import type { SocketFactory, SocketLike } from './control-stream';

export type { SocketFactory, SocketLike };

export const TARGET_CHANNELS = [
  'control', 'operations', 'health', 'logs', 'adapters', 'plugins', 'tasks', 'messages',
] as const;
export type TargetChannel = (typeof TARGET_CHANNELS)[number];

/** `owner_cursor.source_service` values frozen by the operation contract. */
export const TARGET_CURSOR_SERVICES = ['runtime', 'scheduler', 'model', 'host'] as const;
export type TargetCursorService = (typeof TARGET_CURSOR_SERVICES)[number];

/** Values accepted for the `owner_service` query selector, per channel (undefined = no selector). */
export const TARGET_CHANNEL_OWNERS: Record<TargetChannel, readonly string[] | undefined> = {
  operations: TARGET_CURSOR_SERVICES,
  control: ['control', 'runtime', 'scheduler', 'identity'],
  logs: ['identity', 'runtime'],
  health: undefined,
  adapters: undefined,
  plugins: undefined,
  tasks: undefined,
  messages: undefined,
};

/** Wire limits shared with api/asyncapi.yaml. */
export const TARGET_FIRST_FRAME_TIMEOUT_MS = 5_000;
export const TARGET_MAX_SAFE_SEQUENCE = 9_007_199_254_740_991;
export const TARGET_SCOPE_LIMIT = 127;
export const TARGET_CLOSE_CODE = {
  normal: 1000,
  shutdown: 1001,
  internal: 1011,
  invalidFirstFrame: 4400,
  unauthenticated: 4401,
  forbidden: 4403,
  slowConsumer: 1013,
} as const;

export const TARGET_RESYNC_REASONS = ['CURSOR_EXPIRED', 'INVALID_CURSOR', 'SERVER_RESTART'] as const;
export type TargetResyncReason = (typeof TARGET_RESYNC_REASONS)[number];

export interface TargetResourceRef {
  kind: string;
  id: string;
}

/** Exact authorized resources, canonicalized by kind/id. Catalog is never a child grant. */
export interface TargetScope {
  resources: TargetResourceRef[];
}

/** One owner/dataset/scope boundary; not a cross-owner cursor and not an authorization token. */
export interface TargetOwnerCursor {
  source_service: TargetCursorService;
  epoch: string;
  scope: TargetScope;
  /** Decimal uint64 string; compare with BigInt, never as a JS number. */
  sequence: string;
}

export interface TargetChannelSelection {
  channel: TargetChannel;
  /** `owner_service` query selector for channels that require it. */
  owner?: string;
  scope: TargetScope;
}

export interface TargetHelloFrame {
  type: 'hello';
  access_token: string;
  last_sequence: 0;
  scope: TargetScope;
}

export interface TargetResumeFrame {
  type: 'resume';
  access_token: string;
  session_id: string;
  last_sequence: number;
  scope: TargetScope;
  owner_cursor?: TargetOwnerCursor;
}

export interface TargetEventEnvelope {
  schema_version: string;
  event_type: string;
  sequence: number;
  occurred_at: string;
  payload: Record<string, unknown>;
  owner_cursor?: TargetOwnerCursor;
}

export interface TargetReadyFrame {
  session_id: string;
  last_sequence: number;
  owner_cursor?: TargetOwnerCursor;
}

export type TargetFrame =
  | { kind: 'ready'; ready: TargetReadyFrame }
  | { kind: 'resync'; reason: TargetResyncReason }
  | { kind: 'event'; event: TargetEventEnvelope }
  /** Valid JSON object that is neither a handshake reply, resync frame nor stream envelope. */
  | { kind: 'unknown' }
  /** Malformed or contract-violating frame; callers must enter recovery, not guess. */
  | { kind: 'invalid'; detail: string };

export interface FrameContext {
  channel: TargetChannel;
  /** Expected owner selector; the owner cursor must bind exactly this service. */
  owner?: string;
  /** Canonical scope key of the opened session; a cursor naming another scope is invalid. */
  scopeKey: string;
}

export function isSafeSequence(value: unknown): value is number {
  return typeof value === 'number' && Number.isSafeInteger(value) && value >= 0 && value <= TARGET_MAX_SAFE_SEQUENCE;
}

export function isUint64String(value: unknown): value is string {
  if (typeof value !== 'string' || !/^(0|[1-9][0-9]*)$/.test(value)) return false;
  return BigInt(value) <= 18_446_744_073_709_551_615n;
}

/** Compare two decimal uint64 cursor sequences without losing precision. */
export function compareOwnerSequence(left: string, right: string): number {
  const a = BigInt(left);
  const b = BigInt(right);
  return a < b ? -1 : a > b ? 1 : 0;
}

/**
 * Sort by kind/id and drop duplicates so the first frame always carries the canonical form the
 * server expects. Empty scopes and more than 127 exact resources are programmer errors (the
 * contract rejects them), so callers fail before opening a socket.
 */
export function canonicalizeScope(scope: TargetScope): TargetScope {
  if (!scope || !Array.isArray(scope.resources)) throw new Error('target scope requires resources');
  const seen = new Set<string>();
  const resources: TargetResourceRef[] = [];
  for (const resource of scope.resources) {
    if (!resource || typeof resource.kind !== 'string' || !resource.kind
      || typeof resource.id !== 'string' || !resource.id) {
      throw new Error('target scope entries require kind and id');
    }
    const key = `${resource.kind}\u0000${resource.id}`;
    if (seen.has(key)) continue;
    seen.add(key);
    resources.push({ kind: resource.kind, id: resource.id });
  }
  if (resources.length === 0) throw new Error('target scope requires at least one resource');
  if (resources.length > TARGET_SCOPE_LIMIT) {
    throw new Error(`target scope holds ${resources.length} resources; the maximum is ${TARGET_SCOPE_LIMIT}`);
  }
  resources.sort((left, right) => (left.kind === right.kind
    ? (left.id < right.id ? -1 : left.id > right.id ? 1 : 0)
    : (left.kind < right.kind ? -1 : 1)));
  return { resources };
}

export function scopeKey(scope: TargetScope): string {
  return canonicalizeScope(scope).resources.map((resource) => `${resource.kind}:${resource.id}`).join('|');
}

export function sameScope(left: TargetScope, right: TargetScope): boolean {
  return scopeKey(left) === scopeKey(right);
}

export interface CursorExpectation {
  sourceService?: string;
  scopeKey?: string;
}

export type CursorValidation =
  | { ok: true; cursor: TargetOwnerCursor }
  | { ok: false; detail: string };

export function validateOwnerCursor(value: unknown, expected: CursorExpectation = {}): CursorValidation {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return { ok: false, detail: 'owner_cursor is not an object' };
  const cursor = value as Record<string, unknown>;
  if (typeof cursor.source_service !== 'string'
    || !(TARGET_CURSOR_SERVICES as readonly string[]).includes(cursor.source_service)) {
    return { ok: false, detail: 'owner_cursor.source_service is not a frozen owner service' };
  }
  if (typeof cursor.epoch !== 'string' || cursor.epoch.length === 0) {
    return { ok: false, detail: 'owner_cursor.epoch is missing' };
  }
  if (!isUint64String(cursor.sequence)) return { ok: false, detail: 'owner_cursor.sequence is not a uint64 string' };
  let scope: TargetScope;
  try {
    scope = canonicalizeScope(cursor.scope as TargetScope);
  } catch {
    return { ok: false, detail: 'owner_cursor.scope is not a canonical exact scope' };
  }
  if (expected.sourceService && cursor.source_service !== expected.sourceService) {
    return { ok: false, detail: 'owner_cursor.source_service does not match the selected owner' };
  }
  if (expected.scopeKey && scopeKey(scope) !== expected.scopeKey) {
    return { ok: false, detail: 'owner_cursor.scope does not match the session scope' };
  }
  return {
    ok: true,
    cursor: {
      source_service: cursor.source_service as TargetCursorService,
      epoch: cursor.epoch,
      scope,
      sequence: cursor.sequence,
    },
  };
}

function invalid(detail: string): TargetFrame {
  return { kind: 'invalid', detail };
}

function parseEnvelope(message: Record<string, unknown>, context: FrameContext): TargetFrame {
  if (typeof message.schema_version !== 'string' || message.schema_version.length === 0) return invalid('event schema_version is missing');
  if (typeof message.event_type !== 'string' || message.event_type.length === 0) return invalid('event_type is missing');
  if (!isSafeSequence(message.sequence)) return invalid('event sequence is not a safe integer');
  if (typeof message.occurred_at !== 'string' || message.occurred_at.length === 0) return invalid('occurred_at is missing');
  if (!message.payload || typeof message.payload !== 'object' || Array.isArray(message.payload)) return invalid('payload is not an object');
  let ownerCursor: TargetOwnerCursor | undefined;
  if (message.owner_cursor !== undefined) {
    const verdict = validateOwnerCursor(message.owner_cursor, {
      sourceService: context.channel === 'operations' ? context.owner : undefined,
      scopeKey: context.channel === 'operations' ? context.scopeKey : undefined,
    });
    if (!verdict.ok) return invalid(verdict.detail);
    ownerCursor = verdict.cursor;
  } else if (context.channel === 'operations') {
    return invalid('operation.updated requires owner_cursor');
  }
  const event: TargetEventEnvelope = {
    schema_version: message.schema_version,
    event_type: message.event_type,
    sequence: message.sequence,
    occurred_at: message.occurred_at,
    payload: message.payload as Record<string, unknown>,
  };
  if (ownerCursor) event.owner_cursor = ownerCursor;
  return { kind: 'event', event };
}

/**
 * Parse one text frame. Unknown-but-well-formed frames are `unknown` (forward compatible);
 * anything that claims to be a contract frame yet violates it is `invalid` so the client enters
 * recovery instead of silently dropping data.
 */
export function parseTargetFrame(raw: unknown, context: FrameContext): TargetFrame {
  if (typeof raw !== 'string') return invalid('frame is not text');
  let value: unknown;
  try {
    value = JSON.parse(raw);
  } catch {
    return invalid('frame is not JSON');
  }
  if (!value || typeof value !== 'object' || Array.isArray(value)) return invalid('frame is not an object');
  const message = value as Record<string, unknown>;
  if (message.type === 'SESSION_READY') {
    if (typeof message.session_id !== 'string' || message.session_id.length < 32 || message.session_id.length > 256) {
      return invalid('SESSION_READY.session_id is not an opaque capability');
    }
    if (!isSafeSequence(message.last_sequence)) return invalid('SESSION_READY.last_sequence is not a safe integer');
    let ownerCursor: TargetOwnerCursor | undefined;
    if (message.owner_cursor !== undefined) {
      const verdict = validateOwnerCursor(message.owner_cursor, {
        sourceService: context.channel === 'operations' ? context.owner : undefined,
        scopeKey: context.channel === 'operations' ? context.scopeKey : undefined,
      });
      if (!verdict.ok) return invalid(verdict.detail);
      ownerCursor = verdict.cursor;
    } else if (context.channel === 'operations') {
      return invalid('OperationSessionReady requires owner_cursor');
    }
    const ready: TargetReadyFrame = { session_id: message.session_id, last_sequence: message.last_sequence };
    if (ownerCursor) ready.owner_cursor = ownerCursor;
    return { kind: 'ready', ready };
  }
  if (message.type === 'RESYNC_REQUIRED') {
    if (typeof message.reason !== 'string'
      || !(TARGET_RESYNC_REASONS as readonly string[]).includes(message.reason)) {
      return invalid('RESYNC_REQUIRED.reason is not a frozen resync reason');
    }
    return { kind: 'resync', reason: message.reason as TargetResyncReason };
  }
  if (typeof message.event_type === 'string') return parseEnvelope(message, context);
  return { kind: 'unknown' };
}

/**
 * Build the channel URL. Non-secret selectors only: the access token always travels in the first
 * frame and never in a URL, log line or storage.
 */
export function targetChannelUrl(base: string, channel: TargetChannel, owner?: string): string {
  const allowed = TARGET_CHANNEL_OWNERS[channel];
  if (allowed) {
    if (!owner || !allowed.includes(owner)) {
      throw new Error(`target channel ${channel} requires owner_service (${allowed.join('|')})`);
    }
  } else if (owner !== undefined) {
    throw new Error(`target channel ${channel} does not accept owner_service`);
  }
  const trimmed = base.split('?')[0].replace(/\/+$/, '');
  const root = /\/ws$/.test(trimmed) ? trimmed : trimmed.replace(/\/ws\/[a-z]+$/, '/ws');
  const query = allowed ? `?owner_service=${encodeURIComponent(owner!)}` : '';
  return `${root}/${channel}${query}`;
}

export function targetHelloFrame(selection: TargetChannelSelection, accessToken: string): TargetHelloFrame {
  return {
    type: 'hello',
    access_token: accessToken,
    last_sequence: 0,
    scope: canonicalizeScope(selection.scope),
  };
}

export function targetResumeFrame(
  selection: TargetChannelSelection,
  options: { accessToken: string; sessionId: string; lastSequence: number; ownerCursor?: TargetOwnerCursor },
): TargetResumeFrame {
  const frame: TargetResumeFrame = {
    type: 'resume',
    access_token: options.accessToken,
    session_id: options.sessionId,
    last_sequence: options.lastSequence,
    scope: canonicalizeScope(selection.scope),
  };
  if (selection.channel === 'operations') frame.owner_cursor = options.ownerCursor;
  return frame;
}

export function channelRequiresOwnerCursor(channel: TargetChannel): boolean {
  return channel === 'operations';
}
