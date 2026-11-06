/**
 * Authoritative REST snapshot synchronization for a WS-recovered panel resource.
 *
 * Recovery sequence (docs/modules/control-panel.md "实时状态", WS plan 3.4):
 * 1. SESSION_READY records the processed Control sequence / owner cursor in the stream client
 *    **before** any REST read starts.
 * 2. The authoritative REST snapshot is loaded; events that arrive while it is in flight mark the
 *    snapshot dirty, so one follow-up load runs after it (the view never settles on a snapshot
 *    older than an event already seen).
 * 3. RESYNC_REQUIRED / sequence gap / token-independent session restart bumps the generation so an
 *    in-flight (now obsolete) snapshot is discarded instead of overwriting the recovered view;
 *    then a full authoritative reload runs.
 * 4. Resume completion also triggers a refresh: notifications may have been replayed/lost while
 *    disconnected, and only the owner REST snapshot is authoritative.
 *
 * This class is framework agnostic: pages/hooks own React state and pass `load`/`apply` callbacks.
 */

export type SnapshotReason =
  | 'session-ready' | 'resume' | 'event' | 'recovery' | 'fallback' | 'manual';

export interface SnapshotSyncOptions<T> {
  /** Authoritative owner REST read. Must not mutate business state. */
  load: () => Promise<T>;
  /** Apply only generation-current snapshots. */
  apply: (value: T) => void;
  onError?: (error: unknown) => void;
  now?: () => number;
}

export class SnapshotSync<T> {
  private epoch = 0;
  private inFlight?: Promise<void>;
  private running = false;
  private dirty = false;
  private appliedCount = 0;
  private attemptedCount = 0;
  private appliedAt?: number;
  private failure?: unknown;
  private reasons: SnapshotReason[] = [];

  constructor(private readonly options: SnapshotSyncOptions<T>) {}

  /** Generation counter; a recovery/resync bumps it and invalidates in-flight loads. */
  get generation(): number { return this.epoch; }
  get lastAppliedAt(): number | undefined { return this.appliedAt; }
  get lastAppliedCount(): number { return this.appliedCount; }
  get lastAttemptedCount(): number { return this.attemptedCount; }
  get lastError(): unknown { return this.failure; }
  /** Reasons recorded for the loads that were actually started (verification/debug only). */
  get loadReasons(): readonly SnapshotReason[] { return this.reasons; }
  get pending(): boolean { return this.inFlight !== undefined; }

  /**
   * Coalescing snapshot. Calling it while a load is in flight marks the snapshot dirty and starts
   * one follow-up load after it; calls never overlap, so a late old response cannot be applied on
   * top of a newer one (the generation check additionally discards pre-recovery results).
   */
  sync(reason: SnapshotReason = 'manual'): Promise<void> {
    this.dirty = true;
    this.reasons.push(reason);
    if (this.running) return this.inFlight!;
    this.running = true;
    // Start on the next microtask so `inFlight` is observable even if `load` were re-entrant.
    this.inFlight = Promise.resolve().then(() => this.run());
    return this.inFlight;
  }

  /**
   * RESYNC_REQUIRED or a client-detected gap: the old stream capability is gone, so any in-flight
   * snapshot belongs to a superseded generation and must not be applied. A full reload follows.
   */
  recover(reason: SnapshotReason = 'recovery'): Promise<void> {
    this.epoch += 1;
    return this.sync(reason);
  }

  private async run(): Promise<void> {
    try {
      while (this.dirty) {
        this.dirty = false;
        const epoch = this.epoch;
        this.attemptedCount += 1;
        try {
          const value = await this.options.load();
          if (epoch !== this.epoch) continue; // superseded by recovery/reset: discard stale snapshot
          this.options.apply(value);
          this.appliedCount += 1;
          this.appliedAt = (this.options.now ?? Date.now)();
          this.failure = undefined;
        } catch (error) {
          this.failure = error;
          this.options.onError?.(error);
        }
      }
    } finally {
      this.running = false;
      this.inFlight = undefined;
    }
  }
}
