// @vitest-environment node
import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';
import { existsSync } from 'node:fs';
import { once } from 'node:events';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { ApiClient, ApiError } from '../src/api/client';
import { ControlStream, type SocketLike } from '../src/stream/control-stream';

/**
 * Live suite prerequisites (documented in doc/07-control-panel.md):
 *   - a C++ build tree containing `core/kiarabot-core` and
 *     `control/api/kiarabot-control-credentials` (default `build/proto`,
 *     override with `KIARABOT_ACCEPTANCE_BUILD=build/full`), produced by
 *     `cmake --preset full && cmake --build --preset full`;
 *   - `control/api/tests/.venv` with the fixture dependencies, produced by
 *     `uv sync --project control/api/tests`.
 * When they are absent the suite is skipped with the reason instead of failing,
 * so `npm test` stays reproducible on a machine without the C++/Python tools.
 */
const repoRoot = resolve(fileURLToPath(new URL('.', import.meta.url)), '../..');
const buildDir = resolve(repoRoot, process.env.KIARABOT_ACCEPTANCE_BUILD ?? 'build/proto');
const python = resolve(repoRoot, 'control/api/tests/.venv/bin/python');
const fixture = resolve(repoRoot, 'control/api/tests/live_fixture.py');
const missing = [
  !existsSync(python) && 'missing control/api/tests/.venv (run: uv sync --project control/api/tests)',
  !existsSync(resolve(buildDir, 'core/kiarabot-core')) && `missing ${buildDir}/core/kiarabot-core (run: cmake --preset full && cmake --build --preset full)`,
  !existsSync(resolve(buildDir, 'control/api/kiarabot-control-credentials')) && `missing ${buildDir}/control/api/kiarabot-control-credentials`,
  !existsSync(fixture) && `missing fixture ${fixture}`,
].filter(Boolean) as string[];
if (missing.length) console.warn(`[live.test] skipped live panel suite: ${missing.join('; ')}`);

let child: ChildProcessWithoutNullStreams;
let base: string;
let password: string;
async function until(condition: () => boolean, timeout = 6000) {
  const end = Date.now() + timeout;
  while (!condition()) {
    if (Date.now() >= end) throw new Error('Live service condition timed out');
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
}
describe.skipIf(missing.length > 0)('TypeScript against real C++ HTTP/WebSocket', () => {
  beforeAll(async () => {
    child = spawn(python, [fixture], { cwd: repoRoot, env: { ...process.env, KIARABOT_ACCEPTANCE_BUILD: buildDir } });
    let output = ''; let errors = '';
    child.stdout.on('data', (data) => { output += data; });
    child.stderr.on('data', (data) => { errors += data; });
    let exited = false;
    child.once('error', () => { exited = true; });
    child.once('exit', () => { exited = true; });
    await until(() => output.includes('\n') || exited, 15000);
    if (exited || !output.includes('\n')) throw new Error(`Live fixture failed: ${errors}`);
    const configuration = JSON.parse(output.split('\n')[0]);
    base = configuration.base; password = configuration.password;
  }, 20000);
  afterAll(async () => {
    if (!child || child.exitCode !== null) return;
    const exited = once(child, 'exit');
    child.stdin.end('\n');
    const timeout = setTimeout(() => child.kill('SIGKILL'), 8000);
    try { expect((await exited)[0]).toBe(0); } finally { clearTimeout(timeout); }
  });

  it('uses server roles, rotates tokens, preserves 403 and expires on 401', async () => {
    const api = new ApiClient(fetch, base);
    const pair = await api.login({ username: 'viewer', password });
    const principal = await api.principal();
    expect(principal.roles).toEqual(['viewer']);
    expect(principal.permissions).not.toContain('log.read');
    let expired = false;
    api.setUnauthorizedHandler(() => { expired = true; });
    await expect(api.logs()).rejects.toMatchObject({ status: 403, code: 'PERMISSION_DENIED' });
    expect(expired).toBe(false);
    const next = await api.refresh(pair.refresh_token);
    expect(next.access_token).not.toBe(pair.access_token);
    expect((await api.principal()).session_id).toBe(principal.session_id);
    await api.revokeSession(principal.session_id);
    await expect(api.health()).rejects.toBeInstanceOf(ApiError);
    expect(expired).toBe(true);
  });
  it('establishes before snapshot, refreshes known events and resumes after disconnect', async () => {
    const api = new ApiClient(fetch, base);
    const pair = await api.login({ username: 'admin', password });
    const sockets: SocketLike[] = [];
    let status = ''; let refreshes = 0;
    const stream = new ControlStream({ url: base.replace('http:', 'ws:').replace('/api/v1', '/ws/control'),
      accessToken: pair.access_token, factory: (url) => { const socket = new WebSocket(url); sockets.push(socket); return socket; },
      onStatus: (value) => { status = value; }, onRefresh: () => { refreshes++; } });
    stream.start();
    try {
      await until(() => status === 'connected');
      expect((await api.plugins()).items[0].id).toBe('test.plugin');
      const initial = refreshes;
      const response = await fetch(`${base}/plugins/test.plugin/disable`, { method: 'POST',
        headers: { Authorization: `Bearer ${pair.access_token}`, 'Idempotency-Key': crypto.randomUUID() } });
      expect(response.status).toBe(202);
      await until(() => refreshes > initial);
      expect((await api.plugins()).items[0].state).toBe('PLUGIN_STATE_DISABLED');
      sockets[0].close();
      await until(() => sockets.length === 2 && status === 'connected');
      expect(refreshes).toBeGreaterThan(initial + 1);
      const logs = await api.logs();
      expect(JSON.stringify(logs)).not.toContain(pair.access_token);
      expect(JSON.stringify(logs)).not.toContain(password);
      // W4: management reads against the real service (server-side RBAC decides visibility).
      expect(Array.isArray((await api.sessions()).items)).toBe(true);
      const config = await api.pluginConfig('test.plugin').catch((error: unknown) => {
        // 未配置的插件返回 NOT_FOUND，属于契约内行为；只要不是 401/403 即可。
        expect(error).toBeInstanceOf(ApiError);
        expect((error as ApiError).status).toBe(404);
        return undefined;
      });
      if (config) expect(config.version.length).toBeGreaterThan(0);
    } finally { stream.stop(); }
  });
});
