/**
 * P12d-4 / P12c-6 logs and audit pages.
 *
 * The two partitions are independent: /logs requires log.read, /audit requires audit.read; neither
 * permission implies the other. Until P12c-6 wires Control's owner aggregation, both endpoints
 * return 503 and the pages show "data source unavailable" rather than an empty page. After P12c-6,
 * the server owns occurred_at + stable-ID keyset pagination and redaction; the panel passes the
 * opaque token/filter mapping through unchanged and only renders redacted fields. P12e already
 * declares Identity/Runtime ListAudit/ListLogs; P12c-6 is the remaining aggregation wiring.
 */
import { useCallback, useEffect, useState } from 'react';
import { ApiClient, ApiError, type AuditEntry, type LogEntry, type LogQuery } from '../api/client';
import { describeError, FailureNotice, formatTime, label, type Failure } from '../ui';

type OwnerMarked = { owner_service?: string; owner?: string };
type LogRow = LogEntry & OwnerMarked;
type AuditRow = AuditEntry & OwnerMarked;

/** Owner annotation is optional until P12c-6 freezes it; never invent an owner. */
function ownerOf(item: OwnerMarked): string {
  return item.owner ?? item.owner_service ?? '未标注（待 P12c-6）';
}

const sourceUnavailable = (error: unknown) =>
  error instanceof ApiError && error.status === 503;

function Filters({ kind, values, onChange, onPageSize, onApply, busy }: {
  kind: 'logs' | 'audit';
  values: LogQuery;
  onChange: (next: LogQuery) => void;
  onPageSize: (size: number) => void;
  onApply: () => void;
  busy: boolean;
}) {
  return <div className="row-actions">
    <label>occurred_from
      <input aria-label={`${kind} occurred_from`} placeholder="RFC3339" value={values.occurredFrom ?? ''}
        onChange={(event) => onChange({ ...values, occurredFrom: event.target.value })} />
    </label>
    <label>occurred_until
      <input aria-label={`${kind} occurred_until`} placeholder="RFC3339" value={values.occurredUntil ?? ''}
        onChange={(event) => onChange({ ...values, occurredUntil: event.target.value })} />
    </label>
    <label>actor
      <input aria-label={`${kind} actor`} value={values.actor ?? ''}
        onChange={(event) => onChange({ ...values, actor: event.target.value })} />
    </label>
    <label>resource（JSON TargetResourceRef）
      <input aria-label={`${kind} resource`} placeholder='{"kind":"RESOURCE_TASK","id":"task-01"}' value={values.resource ?? ''}
        onChange={(event) => onChange({ ...values, resource: event.target.value })} />
    </label>
    <label>每页<select aria-label={kind === 'logs' ? '每页条数' : `${kind} page size`} value={values.pageSize ?? 20}
      onChange={(event) => onPageSize(Number(event.target.value))}>
      <option value={20}>20</option><option value={50}>50</option><option value={100}>100</option>
    </select></label>
    <button className="secondary" onClick={onApply} disabled={busy}>{busy ? '查询中…' : '查询'}</button>
  </div>;
}

export function LogsAuditView({ api, kind, revision }: {
  api: ApiClient; kind: 'logs' | 'audit'; revision: number;
}) {
  const [filters, setFilters] = useState<LogQuery>({ pageSize: 20 });
  const [applied, setApplied] = useState<LogQuery>({ pageSize: 20 });
  const [page, setPage] = useState<{ logs?: LogEntry[]; audit?: AuditEntry[]; next?: string }>();
  const [failure, setFailure] = useState<Failure>();
  const [busy, setBusy] = useState(false);
  const required = kind === 'logs' ? 'log.read' : 'audit.read';
  const load = useCallback(async () => {
    setBusy(true); setFailure(undefined);
    try {
      if (kind === 'logs') {
        const reply = await api.logPage(applied);
        setPage({ logs: reply.items as LogRow[], next: reply.page.next_page_token });
      } else {
        const reply = await api.auditPage(applied);
        setPage({ audit: reply.items as AuditRow[], next: reply.page.next_page_token });
      }
    } catch (error) {
      setPage(undefined);
      if (sourceUnavailable(error)) {
        // P12c-6 pending: this is an explicit degradation, not an empty result set.
        setFailure(undefined);
        setPage({ next: undefined });
        setUnavailable(true);
        return;
      }
      setUnavailable(false);
      setFailure(describeError(error));
    } finally { setBusy(false); }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [api, kind, applied, revision]);
  const [unavailable, setUnavailable] = useState(false);
  useEffect(() => { void load(); }, [load]);
  const apply = () => setApplied({ ...filters, pageToken: undefined });
  const safeLogs = (page?.logs ?? []) as LogRow[];
  const safeAudit = (page?.audit ?? []) as AuditRow[];
  return <div className="stack">
    <BackendNotice />
    <section className="table-card">
      <div className="table-head"><div>
        <p className="eyebrow">{kind === 'logs' ? 'REDACTED OPERATIONAL LOGS' : 'AUDIT · IMMUTABLE OWNER FACTS'}</p>
        <h2>{kind === 'logs' ? '日志（P12e）' : '审计（P12e）'}</h2></div>
        <span className="count">{kind === 'logs' ? safeLogs.length : safeAudit.length} 条</span></div>
      <p className="muted">权限独立：{kind === 'logs' ? '日志只接受 log.read；不会回退使用 audit.read' : '审计只接受 audit.read；不会回退使用 log.read'}。分页由服务端按 occurred_at + 稳定 ID 的 opaque token 决定，面板不重排、不跨页合并；消息/资源已由 Control 脱敏，不包含凭据、消息正文或模型密钥。</p>
      <Filters kind={kind} values={filters} onChange={setFilters} onPageSize={(size) => { const next = { ...applied, pageSize: size, pageToken: undefined }; setFilters(next); setApplied(next); }} onApply={apply} busy={busy} />
      {unavailable && <div role="alert" className="failure unavailable">
        <strong>数据源暂不可用（503）</strong>
        <p>P12c-6 尚未把 Control 的 {kind === 'logs' ? '/logs' : '/audit'} 聚合接到 P12e 的 Identity/Runtime List{kind === 'logs' ? 'Logs' : 'Audit'}；该页显式降级，不显示空数据、不伪造“没有记录”。合入后按服务端 occurred_at+ID token 与本页过滤映射复验。</p>
      </div>}
      {failure && <FailureNotice failure={failure} />}
      {kind === 'logs' && safeLogs.length > 0 && <div className="table-wrap"><table>
        <thead><tr><th>owner</th><th>occurred_at</th><th>级别</th><th>组件</th><th>消息（脱敏）</th></tr></thead>
        <tbody>{safeLogs.map((item) => <tr key={item.id}>
          <td>{ownerOf(item)}</td><td>{formatTime(item.occurred_at)}</td>
          <td>{label('log', item.level)}</td><td>{item.component}</td><td>{item.message}</td></tr>)}</tbody>
      </table></div>}
      {kind === 'audit' && safeAudit.length > 0 && <div className="table-wrap"><table>
        <thead><tr><th>owner</th><th>occurred_at</th><th>actor</th><th>action</th><th>resource</th><th>request_id</th><th>account</th><th>conversation</th></tr></thead>
        <tbody>{safeAudit.map((item) => <tr key={item.id}>
          <td>{ownerOf(item)}</td><td>{formatTime(item.occurred_at)}</td><td>{item.actor}</td>
          <td>{item.action}</td><td>{item.resource ?? '—'}</td><td>{item.request_id ?? '—'}</td>
          <td>{item.account_id ?? '—'}</td><td>{item.conversation_id ?? '—'}</td></tr>)}</tbody>
      </table></div>}
      {!unavailable && !failure && ((kind === 'logs' ? safeLogs.length : safeAudit.length) === 0) &&
        <p className="empty">暂无数据（仅在 200 成功响应且 items 为空时显示）</p>}
      <div className="pagination">
        {applied.pageToken && <button className="secondary" onClick={() => setApplied({ ...applied, pageToken: undefined })}>返回首页</button>}
        {page?.next && <button className="secondary" onClick={() => setApplied({ ...applied, pageToken: page.next })}>下一页 →</button>}
        <span className="muted">required permission: {required}</span>
      </div>
    </section>
  </div>;
}

function BackendNotice() {
  return <section className="hero"><div><p className="eyebrow">P12e · P12c-6</p>
    <h2>日志与审计（两个独立分区）</h2>
    <p>P12e 已声明 Identity/Runtime ListAudit/ListLogs；P12c-6 负责 Control 聚合接线。接线前 /logs 与 /audit 均返回 503，本页只显示显式“数据源暂不可用”，不显示空页。owner 标注、时间窗/actor/resource 过滤与 opaque keyset token 需在 P12c-6 冻结后复验。</p>
  </div></section>;
}
