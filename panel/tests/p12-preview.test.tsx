/**
 * P12 preview skeleton tests: contract-only pages render typed fixtures in mock
 * mode, never call target endpoints, and keep Q1–Q8 dependencies visible.
 */
import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { App } from '../src/App';
import { ApiClient } from '../src/api/client';
import { MockServer } from '../src/mock/server';
import { DECISIONS, P12_NAV, P12_PAGES } from '../src/p12/catalog';
import * as fixtures from '../src/p12/fixtures';
import { P12PreviewView } from '../src/views/p12-preview';

const TARGET_REQUEST_PREFIXES = [
  '/accounts', '/installations', '/instances', '/operations/',
  '/tasks/task-preview-01', '/models/model-preview-primary',
  '/models/model-preview-primary/configuration', '/health/live', '/health/ready',
];

async function login(server: MockServer, permissions?: string[]) {
  if (permissions) server.permissions = permissions;
  const api = new ApiClient(server.fetch);
  render(<App api={api} socketFactory={server.socketFactory} mock />);
  fireEvent.change(screen.getByLabelText('用户名'), { target: { value: 'admin' } });
  fireEvent.change(screen.getByLabelText('密码'), { target: { value: 'secret' } });
  fireEvent.click(screen.getByRole('button', { name: /登录/ }));
  await screen.findByText('一切运行正常');
  await waitFor(() => expect(server.sockets[0]?.ready).toBe(true));
  return server;
}

function navigate(name: string) {
  fireEvent.click(within(screen.getByRole('navigation')).getByRole('button', { name }));
}

function expectNoTargetRequests(server: MockServer) {
  const target = server.requests.filter((path) =>
    TARGET_REQUEST_PREFIXES.some((prefix) => path.startsWith(prefix)));
  expect(target).toEqual([]);
}

describe('P12 preview skeleton', () => {
  it('lists every Q1–Q8 dependency on the overview page without calling target endpoints', async () => {
    const server = await login(new MockServer());
    navigate('P12 预研总览');
    expect(await screen.findByRole('heading', { name: 'P12 管理面预研' })).toBeInTheDocument();
    expect(screen.getByText('本次预研范围')).toBeInTheDocument();
    expect(screen.getAllByText('P12 预研 · 未接线').length).toBeGreaterThan(0);
    for (const decision of Object.values(DECISIONS)) {
      expect(screen.getAllByText(decision.id).length).toBeGreaterThan(0);
      expect(screen.getAllByText(decision.title).length).toBeGreaterThan(0);
    }
    expectNoTargetRequests(server);
  });

  for (const { key, label } of P12_NAV.filter((item) => item.key !== 'p12-overview')) {
    it(`renders the ${label} skeleton and never requests its target endpoints`, async () => {
      const server = await login(new MockServer());
      navigate(label);
      const spec = P12_PAGES[key];
      expect(await screen.findByRole('heading', { name: spec.title })).toBeInTheDocument();
      expect(screen.getByText(spec.description)).toBeInTheDocument();
      expect(screen.getAllByText('P12 预研 · 未接线').length).toBeGreaterThan(0);

      for (const operation of spec.operations) {
        expect(screen.getAllByText(operation.endpoint.path).length).toBeGreaterThan(0);
        expect(screen.getAllByText(operation.operationId).length).toBeGreaterThan(0);
      }
      for (const fixture of spec.fixtures) {
        expect(screen.getByText(fixture.title)).toBeInTheDocument();
        expect(screen.getAllByText(fixture.columns[0]).length).toBeGreaterThan(0);
      }
      for (const decision of spec.decisions) {
        expect(screen.getAllByText(decision).length).toBeGreaterThan(0);
      }
      expectNoTargetRequests(server);
    });
  }

  it('shows the unwired live-mode notice instead of mock fixtures', () => {
    render(<P12PreviewView page="p12-accounts" mock={false} />);
    expect(screen.getByText(/live 模式不会向 not_implemented 端点发起请求/)).toBeInTheDocument();
    expect(screen.queryByText('TargetAccountRecord 预览')).not.toBeInTheDocument();
  });

  it('hides preview pages whose permission the principal lacks', async () => {
    await login(new MockServer(), [
      'health.read', 'plugin.read', 'task.read', 'session.read', 'session.revoke',
    ]);
    const navigation = screen.getByRole('navigation');
    expect(within(navigation).queryByRole('button', { name: '账号（P12）' })).not.toBeInTheDocument();
    expect(within(navigation).queryByRole('button', { name: '安装与实例（P12）' })).toBeInTheDocument();
    expect(within(navigation).queryByRole('button', { name: '任务（P12）' })).toBeInTheDocument();
    expect(within(navigation).queryByRole('button', { name: '健康（P12）' })).toBeInTheDocument();
  });

  it('keeps mock fixtures on the target required fields', () => {
    expect(fixtures.accounts.every((account) =>
      account.account_id && account.platform && account.platform_account_id && account.version && account.state)).toBe(true);
    expect(fixtures.installations.every((installation) =>
      installation.installation_id && installation.plugin_type_id && installation.version && installation.state)).toBe(true);
    expect(fixtures.instances.every((instance) =>
      instance.instance_id && instance.installation_id
      && typeof instance.externally_managed === 'boolean'
      && instance.state.desired && instance.state.process_outcome)).toBe(true);
    expect(fixtures.operations.every((operation) =>
      operation.ref.owner_service && operation.ref.operation_id && operation.state && operation.execution_outcome)).toBe(true);
    expect(fixtures.tasks.every((task) =>
      task.task_id && task.target.type && task.state && task.occurrence_id)).toBe(true);
    expect(fixtures.modelConfiguration.model_id).toBe(fixtures.modelMetadata.model_id);
    expect(Object.keys(fixtures.readiness.probes).sort()).toEqual(['live', 'ready']);
  });
});
