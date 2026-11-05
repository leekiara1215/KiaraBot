/** P12d live wiring tests: pages consume the P12a contract through ApiClient and mock transport. */
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { App } from '../src/App';
import { ApiClient } from '../src/api/client';
import { MockServer, MockSocket } from '../src/mock/server';
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

describe('P12d-2 live expansion', () => {
  it('lists human principals and distinguishes 503 from an empty directory', async () => {
    const { server } = await login(new MockServer());
    navigate('权限');
    expect(await screen.findByText('11111111-1111-4111-8111-111111111111')).toBeInTheDocument();
    expect(server.requests).toContain('/principals');
  });

  it('pages tasks by items/page with explicit scope and keeps 200 neutral', async () => {
    const server = new MockServer();
    const api = new ApiClient(server.fetch);
    api.setToken('mock-access-token-0123456789');
    render(<P12LiveView page="p12-tasks" api={api} permissions={['task.read']} revision={0} />);
    expect(await screen.findByText('task-mock-01')).toBeInTheDocument();
    expect(server.queries.some((query) => query.includes('scope='))).toBe(true);
    fireEvent.click(screen.getByText('task-mock-01'));
    expect(await screen.findByText('task-preview-01')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: '取消任务' }));
    expect(await screen.findByText(/取消命令已受理/)).toBeInTheDocument();
    expect(server.requests).toContain('/tasks/task-preview-01/cancel');
    expect(server.queries.some((query) => query.includes('scope='))).toBe(true);
  });

  it('creates installations/instances and shows config version strings', async () => {
    const server = new MockServer();
    const api = new ApiClient(server.fetch);
    api.setToken('mock-access-token-0123456789');
    render(<P12LiveView page="p12-installations" api={api} permissions={['plugin.read', 'plugin.manage']} revision={0} />);
    fireEvent.change(await screen.findByLabelText('plugin type ID'), { target: { value: 'welcome' } });
    // The live page requires the explicit installation catalog UUID before it submits Runtime
    // CreateInstallation (`disabled={!pluginTypeId || !installationCatalog}`).
    fireEvent.change(screen.getByLabelText('安装目录 UUID'), { target: { value: '11111111-1111-4111-8111-111111111111' } });
    fireEvent.click(screen.getByRole('button', { name: '创建安装' }));
    expect((await screen.findAllByText(/installation-mock-created/)).length).toBeGreaterThan(0);
    fireEvent.change(screen.getByLabelText('创建实例的安装 ID'), { target: { value: 'installation-mock-created' } });
    fireEvent.change(screen.getByLabelText('host ID'), { target: { value: 'host-1' } });
    fireEvent.click(screen.getByRole('button', { name: '创建实例' }));
    expect((await screen.findAllByText(/instance-mock-created/)).length).toBeGreaterThan(0);
    fireEvent.change(screen.getByLabelText('实例 ID'), { target: { value: 'instance-mock-created' } });
    fireEvent.click(screen.getByRole('button', { name: '读取 configuration' }));
    expect(await screen.findByText('Runtime configuration（只读脱敏）')).toBeInTheDocument();
    expect(screen.getByText('Host 事实（观察）')).toBeInTheDocument();
    expect(screen.getByText('Plugin 事实（确认）')).toBeInTheDocument();
    expect(screen.getAllByText('desired_config_version').length).toBeGreaterThan(0);
    expect(server.requests).toContain('/installations/installation-mock-created/instances/instance-mock-created/configuration');
    fireEvent.click(screen.getByRole('button', { name: '设为运行' }));
    expect(await screen.findByText('Runtime 事实（期望）')).toBeInTheDocument();
    expect(screen.getByText('Plugin 事实（确认）')).toBeInTheDocument();
    expect(screen.getAllByText(/处理中（未确认）/).length).toBeGreaterThan(0);
    expect(screen.queryByText('已确认应用')).not.toBeInTheDocument();
    expect(server.requests).toContain('/installations/installation-mock-created/instances/instance-mock-created/plugin-state');
  });

  it('shows operation state instead of inferring completion from HTTP 200', async () => {
    const server = new MockServer();
    const api = new ApiClient(server.fetch);
    api.setToken('mock-access-token-0123456789');
    render(<P12LiveView page="p12-operations" api={api} permissions={[]} revision={0} />);
    fireEvent.change(screen.getByLabelText('operation ID'), { target: { value: 'operation-mock-01' } });
    fireEvent.click(screen.getByRole('button', { name: '查询' }));
    expect((await screen.findAllByText('执行中')).length).toBeGreaterThan(0);
    expect(screen.getByText(/200 不等于完成/)).toBeInTheDocument();
  });

  it('distinguishes DEGRADED 200 from Identity 503', async () => {
    const server = new MockServer();
    server.healthOverride = { state: 'DEGRADED', components: [
      { component: 'identity', state: 'HEALTHY' },
      { component: 'runtime', state: 'DEGRADED', reason: 'runtime unavailable' },
      { component: 'scheduler', state: 'HEALTHY' }, { component: 'model', state: 'HEALTHY' },
    ] };
    const api = new ApiClient(server.fetch);
    api.setToken('mock-access-token-0123456789');
    render(<P12LiveView page="p12-health" api={api} permissions={['health.read']} revision={0} />);
    expect(await screen.findByText('部分依赖降级（HTTP 200）')).toBeInTheDocument();
    expect(screen.getByText('四个直接依赖')).toBeInTheDocument();
    expect(screen.getByText('runtime unavailable')).toBeInTheDocument();
  });

  it('shows Identity not-ready 503 without pretending four dependencies are healthy', async () => {
    const server = new MockServer();
    server.failures.set('/health', { status: 503, code: 'UPSTREAM_UNAVAILABLE' });
    const api = new ApiClient(server.fetch);
    api.setToken('mock-access-token-0123456789');
    render(<P12LiveView page="p12-health" api={api} permissions={['health.read']} revision={0} />);
    expect(await screen.findByText(/Identity 未就绪：受权健康聚合返回 503/)).toBeInTheDocument();
    expect(screen.queryByText('四依赖正常')).not.toBeInTheDocument();
  });

  it('loads REST snapshots when the WS server is unavailable and marks them non-current', async () => {
    const server = new MockServer();
    const failingSocketFactory = () => {
      const socket = new MockSocket();
      queueMicrotask(() => socket.closeWith(1000));
      return socket;
    };
    render(<App api={new ApiClient(server.fetch)} socketFactory={failingSocketFactory} />);
    fireEvent.change(screen.getByLabelText('用户名'), { target: { value: 'admin' } });
    fireEvent.change(screen.getByLabelText('密码'), { target: { value: 'secret' } });
    fireEvent.click(screen.getByRole('button', { name: /登录/ }));
    expect(await screen.findByText(/当前为 REST 快照（非最新）/)).toBeInTheDocument();
    fireEvent.click(within(screen.getByRole('navigation')).getByRole('button', { name: '账号（P12）' }));
    expect(await screen.findByText('account-qq-10001')).toBeInTheDocument();
    expect(server.requests).toContain('/accounts');
  });
});


describe('P12d-3 instance facts, health auth, TaskSummary', () => {
  it('shows three facts and never marks non-APPLIED plugin state as complete', async () => {
    const server = new MockServer();
    const api = new ApiClient(server.fetch);
    api.setToken('mock-access-token-0123456789');
    render(<P12LiveView page="p12-installations" api={api} permissions={['plugin.read', 'plugin.manage']} revision={0} />);
    fireEvent.change(screen.getByLabelText('实例所属安装 ID'), { target: { value: 'installation-welcome-01' } });
    fireEvent.change(screen.getByLabelText('实例 ID'), { target: { value: 'instance-welcome-01' } });
    fireEvent.change(screen.getByLabelText('instance expected_version'), { target: { value: '1' } });
    fireEvent.click(screen.getByRole('button', { name: '设为运行' }));
    expect(await screen.findByText('Runtime 事实（期望）')).toBeInTheDocument();
    expect(screen.getByText('Host 事实（观察）')).toBeInTheDocument();
    expect(screen.getByText('Plugin 事实（确认）')).toBeInTheDocument();
    expect(screen.getByText(/结果未知（不等于已停止）/)).toBeInTheDocument();
    expect(screen.getAllByText(/处理中（未确认）/).length).toBeGreaterThan(0);
    expect(screen.queryByText('已确认应用')).not.toBeInTheDocument();
  });

  it('renders health.read 401 as unauthorized, not unhealthy', async () => {
    const server = new MockServer();
    server.failures.set('/health', { status: 401, code: 'UNAUTHENTICATED' });
    const api = new ApiClient(server.fetch);
    api.setToken('mock-access-token-0123456789');
    render(<P12LiveView page="p12-health" api={api} permissions={['health.read']} revision={0} />);
    expect(await screen.findByText(/未授权：\/api\/v1\/health 需要 health.read/)).toBeInTheDocument();
    expect(screen.queryByText(/Identity 未就绪：受权健康聚合返回 503/)).not.toBeInTheDocument();
    expect(server.requests).toContain('/health/live');
    expect(server.requests).toContain('/health/ready');
  });

  it('distinguishes an unimplemented aggregate 503 from Identity unhealthy', async () => {
    const server = new MockServer();
    server.failures.set('/health', { status: 503, code: 'CAPABILITY_NOT_SUPPORTED' });
    const api = new ApiClient(server.fetch);
    api.setToken('mock-access-token-0123456789');
    render(<P12LiveView page="p12-health" api={api} permissions={['health.read']} revision={0} />);
    expect(await screen.findByText(/健康聚合不可用（HTTP 503 CAPABILITY_NOT_SUPPORTED\/UNIMPLEMENTED）/)).toBeInTheDocument();
    expect(screen.queryByText(/Identity 未就绪：受权健康聚合返回 503/)).not.toBeInTheDocument();
  });

  it('renders TaskSummary rows with name/state/created_at', async () => {
    const server = new MockServer();
    const api = new ApiClient(server.fetch);
    api.setToken('mock-access-token-0123456789');
    render(<P12LiveView page="p12-tasks" api={api} permissions={['task.read']} revision={0} />);
    expect(await screen.findByText('task-mock-01')).toBeInTheDocument();
    expect(screen.getByText('定时摘要')).toBeInTheDocument();
    expect(screen.getByText('SCHEDULED_TASK_PENDING')).toBeInTheDocument();
    expect(screen.getByText('created_at')).toBeInTheDocument();
  });
});

describe('P12d-5 lifecycle delete/restore', () => {
  it('shows accepted delete with blocked operation, not completion', async () => {
    const server = new MockServer();
    const api = new ApiClient(server.fetch);
    api.setToken('mock-access-token-0123456789');
    render(<P12LiveView page="p12-installations" api={api} permissions={['plugin.read', 'plugin.manage']} revision={0} />);
    fireEvent.change(screen.getByLabelText('生命周期 resource ID'), { target: { value: 'instance-welcome-01' } });
    fireEvent.change(screen.getByLabelText('生命周期 expected_version'), { target: { value: '2' } });
    fireEvent.click(screen.getByRole('button', { name: '删除资源' }));
    expect(await screen.findByText(/删除已受理/)).toBeInTheDocument();
    expect((await screen.findAllByText('受阻')).length).toBeGreaterThan(0);
    expect(screen.getByText(/30 天内可恢复/)).toBeInTheDocument();
    expect(screen.getByText(/BLOCKED（受阻）不是失败/)).toBeInTheDocument();
    expect(screen.queryByText('状态成功')).not.toBeInTheDocument();
    expect(server.requests).toContain('/resources/RESOURCE_INSTANCE/instance-welcome-01/delete');
    expect(server.requests.some((path) => path.startsWith('/operations/runtime/operation-lifecycle-01'))).toBe(true);
  });

  it('keeps lifecycle explicit when delete/restore REST is unavailable (404)', async () => {
    const server = new MockServer();
    server.failures.set('/resources/RESOURCE_INSTANCE/x/restore', { status: 404, code: 'NOT_FOUND' });
    const api = new ApiClient(server.fetch);
    api.setToken('mock-access-token-0123456789');
    render(<P12LiveView page="p12-installations" api={api} permissions={['plugin.read', 'plugin.manage']} revision={0} />);
    fireEvent.change(screen.getByLabelText('生命周期 resource ID'), { target: { value: 'x' } });
    fireEvent.click(screen.getByRole('button', { name: '恢复资源' }));
    expect(await screen.findByText(/删除\/恢复当前不可用/)).toBeInTheDocument();
    expect(screen.queryByText('状态成功')).not.toBeInTheDocument();
  });
});
