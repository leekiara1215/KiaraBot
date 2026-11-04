// @vitest-environment node
/**
 * SnapshotSync tests: authoritative REST recovery around stream notifications.
 *
 * Covers the WS-06 recovery rules: an event during an in-flight read dirties the snapshot and
 * forces a follow-up load; a RESYNC/gap bumps the generation so a late obsolete response can never
 * overwrite the recovered view; load failures surface without blocking a later snapshot.
 */
import { describe, expect, it, vi } from 'vitest';
import { SnapshotSync } from '../src/stream/snapshot-sync';

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((resolveValue, rejectValue) => { resolve = resolveValue; reject = rejectValue; });
  return { promise, resolve, reject };
}

describe('SnapshotSync', () => {
  it('applies one authoritative snapshot and records when it was applied', async () => {
    const applied: string[] = [];
    const sync = new SnapshotSync<string>({ load: async () => 'snapshot-1', apply: (value) => applied.push(value), now: () => 42 });
    await sync.sync('session-ready');
    expect(applied).toEqual(['snapshot-1']);
    expect(sync.lastAppliedAt).toBe(42);
    expect(sync.lastAppliedCount).toBe(1);
    expect(sync.pending).toBe(false);
  });

  it('reloads after an event arrived while the snapshot was in flight (dirty)', async () => {
    const first = deferred<string>(); const second = deferred<string>();
    const loads: string[] = []; const applied: string[] = [];
    const sync = new SnapshotSync<string>({
      load: () => { loads.push('load'); return loads.length === 1 ? first.promise : second.promise; },
      apply: (value) => applied.push(value),
    });
    const pending = sync.sync('session-ready');
    await Promise.resolve();
    expect(loads).toHaveLength(1);
    // operation.updated arrives while the authoritative REST read is still in flight.
    void sync.sync('event');
    first.resolve('stale-base');
    await Promise.resolve();
    expect(loads).toHaveLength(2);
    expect(applied).toEqual(['stale-base']); // base applied, then a follow-up read is required
    second.resolve('fresh');
    await pending;
    expect(applied).toEqual(['stale-base', 'fresh']); // view settles on the post-event snapshot
    expect(sync.lastAppliedCount).toBe(2);
  });

  it('discards a late pre-recovery snapshot instead of overwriting the recovered view', async () => {
    const stale = deferred<string>(); const fresh = deferred<string>();
    const applied: string[] = []; const loads: string[] = [];
    const sync = new SnapshotSync<string>({
      load: () => { loads.push('load'); return loads.length === 1 ? stale.promise : fresh.promise; },
      apply: (value) => applied.push(value),
    });
    const pending = sync.sync('session-ready');
    await Promise.resolve();
    expect(sync.generation).toBe(0);
    // RESYNC_REQUIRED/sequence gap: the old stream capability is gone; restart the snapshot.
    const recovered = sync.recover();
    await Promise.resolve();
    stale.resolve('obsolete');
    await Promise.resolve();
    expect(applied).toEqual([]); // stale generation never applied
    fresh.resolve('authoritative');
    await Promise.all([pending, recovered]);
    expect(applied).toEqual(['authoritative']);
    expect(sync.generation).toBe(1);
  });

  it('surfaces load failures without blocking a later snapshot', async () => {
    const applied: string[] = []; const errors: unknown[] = [];
    let fail = true;
    const sync = new SnapshotSync<string>({
      load: async () => { if (fail) throw new Error('503 upstream unavailable'); return 'recovered'; },
      apply: (value) => applied.push(value),
      onError: (error) => errors.push(error),
    });
    await sync.sync('session-ready');
    expect(applied).toEqual([]);
    expect(errors).toHaveLength(1);
    expect(sync.lastError).toBeInstanceOf(Error);
    fail = false;
    await sync.sync('fallback');
    expect(applied).toEqual(['recovered']);
    expect(sync.lastError).toBeUndefined();
  });

  it('coalesces concurrent refreshes into one follow-up load in order', async () => {
    const first = deferred<string>(); const second = deferred<string>();
    const loads = vi.fn(() => (loads.mock.calls.length === 1 ? first.promise : second.promise));
    const applied: string[] = [];
    const sync = new SnapshotSync<string>({ load: loads, apply: (value) => applied.push(value) });
    const pending = sync.sync('session-ready');
    await Promise.resolve();
    void sync.sync('event');
    void sync.sync('event');
    void sync.sync('resume');
    expect(loads).toHaveBeenCalledTimes(1);
    first.resolve('first');
    await Promise.resolve();
    expect(applied).toEqual(['first']);
    expect(loads).toHaveBeenCalledTimes(2);
    second.resolve('second');
    await pending;
    expect(applied).toEqual(['first', 'second']);
    expect(loads).toHaveBeenCalledTimes(2);
  });
});
