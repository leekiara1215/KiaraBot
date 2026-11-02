/** P12d-4 logs/audit partition tests: independent permissions, opaque pagination, filters,
 * owner annotation, redaction text, and P12c-6-pending 503/401/403/400 rendering. */
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { ApiClient } from '../src/api/client';
import { MockServer } from '../src/mock/server';
import { LogsAuditView } from '../src/views/logs-audit';

function client(server: MockServer) {
  const api = new ApiClient(server.fetch);
  api.setToken('mock-access-token-0123456789');
  return api;
}

describe('P12d-4 logs/audit partitions', () => {
  it('renders redacted logs with owner, filters and opaque keyset pagination', async () => {
    const server = new MockServer();
    const api = client(server);
    render(<LogsAuditView api={api} kind="logs" revision={0} />);
    expect(await screen.findByText('服务已启动')).toBeInTheDocument();
    expect(screen.getAllByText('identity').length).toBeGreaterThan(0);
    expect(screen.getByText(/日志只接受 log\.read；不会回退使用 audit\.read/)).toBeInTheDocument();
    expect(server.requests).toContain('/logs');
    expect(server.requests).not.toContain('/audit');

    fireEvent.click(screen.getByRole('button', { name: '下一页 →' }));
    await waitFor(() => expect(server.queries.some((q) => q.includes('page_token=logs-page-2'))).toBe(true));
    fireEvent.change(screen.getByLabelText('logs occurred_from'), { target: { value: '2026-10-30T02:10:00Z' } });
    fireEvent.click(screen.getByRole('button', { name: '查询' }));
    await waitFor(() => expect(server.queries.some((q) => q.includes('occurred_from='))).toBe(true));
  });

  it('keeps audit independent and uses only audit.read with owner/actor/resource mapping', async () => {
    const server = new MockServer();
    const api = client(server);
    render(<LogsAuditView api={api} kind="audit" revision={0} />);
    expect(await screen.findByText('control-panel')).toBeInTheDocument();
    expect(screen.getByText(/审计只接受 audit\.read；不会回退使用 log\.read/)).toBeInTheDocument();
    expect(server.requests).toContain('/audit');
    expect(server.requests).not.toContain('/logs');
    fireEvent.change(screen.getByLabelText('audit actor'), { target: { value: 'admin' } });
    fireEvent.change(screen.getByLabelText('audit resource'), {
      target: { value: '{"kind":"RESOURCE_ACCOUNT","id":"control-panel"}' },
    });
    fireEvent.click(screen.getByRole('button', { name: '查询' }));
    await waitFor(() => expect(server.queries.some((q) => q.includes('actor=admin') && q.includes('resource='))).toBe(true));
  });

  it('shows P12c-6 pending 503 as unavailable, never as an empty page', async () => {
    const server = new MockServer();
    server.failures.set('/logs', { status: 503, code: 'UPSTREAM_UNAVAILABLE' });
    render(<LogsAuditView api={client(server)} kind="logs" revision={0} />);
    expect(await screen.findByText(/数据源暂不可用（503）/)).toBeInTheDocument();
    expect(screen.getByText(/P12c-6 尚未把 Control/)).toBeInTheDocument();
    expect(screen.queryByText('暂无数据（仅在 200 成功响应且 items 为空时显示）')).not.toBeInTheDocument();
  });

  it('shows 403 as permission denied and 400 as invalid parameters', async () => {
    const server = new MockServer();
    server.failures.set('/audit', { status: 403, code: 'PERMISSION_DENIED' });
    const { unmount } = render(<LogsAuditView api={client(server)} kind="audit" revision={0} />);
    expect(await screen.findByText('权限不足')).toBeInTheDocument();
    unmount();
    server.failures.set('/audit', { status: 400, code: 'INVALID_ARGUMENT' });
    render(<LogsAuditView api={client(server)} kind="audit" revision={0} />);
    expect(await screen.findByText('请求参数无效')).toBeInTheDocument();
  });
});
