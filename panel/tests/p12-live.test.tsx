/** P12d live wiring tests: pages consume the P12a contract through ApiClient and mock transport. */
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { App } from '../src/App';
import { ApiClient } from '../src/api/client';
import { MockServer } from '../src/mock/server';
import { P12LiveView } from '../src/views/p12-live';

async function login(server: MockServer, permissions?: string[]) {
  if (permissions) server.permissions = permissions;
  const api = new ApiClient(server.fetch);
  render(<App api={api} socketFactory={server.socketFactory} />);
  fireEvent.change(screen.getByLabelText('用户名'), { target: { value: 'admin' } });
  fireEvent.change(screen.getByLabelText('密码'), { target: { value: 'secret' } });
  fireEvent.click(screen.getByRole('button', { name: /登录/ }));
  await screen.findByText('一切运行正常');
  await waitFor(() => expect(server.sockets[0]?.ready).toBe(true));
  return { api, server };
}

function navigate(name: string) {
  fireEvent.click(within(screen.getByRole('navigation')).getByRole('button', { name }));
}

describe('P12 live pages', () => {
  it('loads accounts from the live API and never falls back to fixtures', async () => {
    const { server } = await login(new MockServer());
    navigate('账号（P12）');
    expect(await screen.findByRole('heading', { name: '账号（目标 Runtime）' })).toBeInTheDocument();
    expect(await screen.findByText('account-qq-10001')).toBeInTheDocument();
    expect(server.requests).toContain('/accounts');
    expect(screen.queryByText('TargetAccountRecord 预览')).not.toBeInTheDocument();
  });

  it('renders live process probes and the authenticated component health', async () => {
    const { server } = await login(new MockServer());
    navigate('健康（P12）');
    expect(await screen.findByText('live')).toBeInTheDocument();
    expect(server.requests).toEqual(expect.arrayContaining(['/health/live', '/health/ready', '/health']));
    expect((await screen.findAllByText('HEALTHY')).length).toBeGreaterThan(0);
  });

  it('shows operation progress from the REST snapshot and distinguishes blocked fields', async () => {
    const { server } = await login(new MockServer());
    navigate('操作进度（P12）');
    fireEvent.change(screen.getByLabelText('operation ID'), { target: { value: 'operation-mock-01' } });
    fireEvent.click(screen.getByRole('button', { name: '查询' }));
    expect(await screen.findByText(/operation-preview-01/)).toBeInTheDocument();
    expect(server.requests.some((path) => path.startsWith('/operations/runtime/operation-mock-01'))).toBe(true);
  });

  it('treats a NOT_FOUND task as not found instead of an execution failure', async () => {
    const { api, server } = await login(new MockServer());
    server.failures.set('/tasks/task-missing', { status: 404, code: 'NOT_FOUND' });
    render(<P12LiveView page="p12-tasks" api={api} permissions={['task.read']} revision={0} />);
    fireEvent.change(screen.getByLabelText('task ID'), { target: { value: 'task-missing' } });
    fireEvent.click(screen.getByRole('button', { name: '查询' }));
    expect(await screen.findByText(/未找到该任务/)).toBeInTheDocument();
    expect(screen.queryByText('任务失败')).not.toBeInTheDocument();
  });

  it('gates full model configuration behind model.manage but still shows metadata', async () => {
    const server = new MockServer();
    await login(server, ['health.read', 'account.read', 'plugin.read', 'task.read', 'session.read',
      'session.revoke', 'model.read', 'resource.read']);
    const api = new ApiClient(server.fetch);
    api.setToken('mock-access-token-0123456789');
    render(<P12LiveView page="p12-model-config" api={api} permissions={['model.read']} revision={0} />);
    fireEvent.change(screen.getByLabelText('model ID'), { target: { value: 'model-primary' } });
    fireEvent.click(screen.getByRole('button', { name: '读取' }));
    expect(await screen.findByText(/model-preview-primary/)).toBeInTheDocument();
    expect(await screen.findByText('model.manage')).toBeInTheDocument();
    expect(server.requests).not.toContain('/models/model-primary/configuration');
  });

  it('recovers from a WS resync by reloading the authoritative REST snapshot', async () => {
    const { server } = await login(new MockServer());
    navigate('账号（P12）');
    await screen.findByText('account-qq-10001');
    const before = server.requests.filter((path) => path === '/accounts').length;
    act(() => {
      server.sockets[0].emit({ type: 'RESYNC_REQUIRED', reason: 'expired' });
    });
    await waitFor(() => expect(server.sockets.length).toBeGreaterThan(1));
    await waitFor(() => expect(server.requests.filter((path) => path === '/accounts').length)
      .toBeGreaterThan(before));
  });

  it('queries action metadata only with action.inspect and keeps NOT_FOUND neutral', async () => {
    const { server } = await login(new MockServer());
    const api = new ApiClient(server.fetch);
    api.setToken('mock-access-token-0123456789');
    render(<P12LiveView page="p12-accounts" api={api} permissions={['account.read', 'action.inspect']} revision={0} />);
    fireEvent.change(screen.getByLabelText('动作账号 ID'), { target: { value: 'account-qq-10001' } });
    fireEvent.click(screen.getByRole('button', { name: '查询动作' }));
    await waitFor(() => expect(server.requests).toContain('/actions/metadata'));
    expect(screen.queryByText(/动作执行失败/)).not.toBeInTheDocument();
  });
});
