import { useState } from 'react';
import { type ActionResult, type ApiClient } from '../api/client';
import { ActionFeedback, describeError, FailureNotice, formatTime, label, StateTag, useAction, type Failure } from '../ui';

/**
 * Action 查询已接线到 Control → RuntimeService.QueryActionResult
 * （getActionResult / getActionByIdempotencyKey）。不存在、不可见或已过期结果返回
 * NOT_FOUND；面板不伪造数据，只展示结果或明确错误态。
 */
export function ActionsView({ api, permissions }: { api: ApiClient; permissions: string[] }) {
  const [actionId, setActionId] = useState('');
  const [key, setKey] = useState('');
  const [result, setResult] = useState<ActionResult>();
  const [failure, setFailure] = useState<Failure>();
  const query = useAction();
  const canRead = permissions.includes('action.read');

  const run = (labelText: string, task: () => Promise<ActionResult>) => {
    setResult(undefined); setFailure(undefined);
    void query.run(labelText, async () => {
      try { setResult(await task()); return '查询完成'; }
      catch (error) {
        const parsed = describeError(error);
        setFailure(parsed.kind === 'not-found'
          ? { ...parsed, title: '未找到（NOT_FOUND）', detail: 'Runtime QueryActionResult 对不存在/不可见/过期结果返回 NOT_FOUND，这不代表动作执行失败；请核对 action_id 或幂等键。' }
          : parsed);
        throw error;
      }
    });
  };

  return (
    <div className="stack">
      <section className="table-card panel-card">
        <div className="table-head">
          <div><p className="eyebrow">ACTION LOOKUP</p><h2>Action 结果查询</h2></div>
          <span className="hint">只读；需要 action.read</span>
        </div>
        <div className="panel-body">
          <div role="note" className="contract-note">
            `GET /actions/{'{actionId}'}` 与 `GET /actions/by-idempotency-key` 经 Control 映射到 RuntimeService.QueryActionResult
            （action.inspect + 显式 account scope）；不存在/不可见/过期结果返回 NOT_FOUND。本页不伪造结果，幂等键不回显/不记录。
          </div>
          <div className="query-grid">
            <form className="query-form" onSubmit={(event) => { event.preventDefault(); run('按 action_id 查询', () => api.action(actionId)); }}>
              <label>action_id<input value={actionId} onChange={(event) => setActionId(event.target.value)} placeholder="action-..." pattern="[A-Za-z0-9._-]+" /></label>
              <button className="primary small" disabled={!canRead || !actionId}>按 ID 查询</button>
            </form>
            <form className="query-form" onSubmit={(event) => { event.preventDefault(); run('按幂等键查询', () => api.actionByKey(key)); }}>
              <label>Idempotency-Key<input value={key} onChange={(event) => setKey(event.target.value)} placeholder="幂等键（不回显、不记录）" minLength={16} maxLength={128} /></label>
              <button className="primary small" disabled={!canRead || key.length < 16}>按幂等键查询</button>
            </form>
          </div>
          <ActionFeedback state={query.state} />
          {failure && <FailureNotice failure={failure} />}
          {result && (
            <div className="result-card">
              <p>action_id <code>{result.action_id}</code> <StateTag text={label('action', result.status)} tone={result.status === 'SUCCEEDED' ? 'ok' : result.status === 'FAILED' ? 'bad' : 'warn'} /></p>
              {result.platform_message_id && <p>platform_message_id <code>{result.platform_message_id}</code></p>}
              {result.completed_at && <p>completed_at {formatTime(result.completed_at)}</p>}
              {result.error && <p>error <code>{result.error.code}</code> — {result.error.message}（request_id={result.error.request_id}）</p>}
            </div>
          )}
          {!canRead && <p className="hint">当前角色没有 action.read，查询不可用。</p>}
        </div>
      </section>
    </div>
  );
}
