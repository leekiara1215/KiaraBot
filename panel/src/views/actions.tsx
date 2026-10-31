import { useState } from 'react';
import { type ActionResult, type ApiClient } from '../api/client';
import { ActionFeedback, describeError, FailureNotice, formatTime, label, StateTag, useAction, type Failure } from '../ui';

/**
 * Action 查询接线占位。api/openapi.yaml 把 `getActionResult` 与
 * `getActionByIdempotencyKey` 标记为 `x-contract-status: not_implemented`：
 * v1 Core 不创建 control 拥有的 Action 记录，两个查询对任何调用方都返回
 * NOT_FOUND。面板不伪造数据，只展示结果或明确错误态。
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
          ? { ...parsed, title: '未找到（契约标记 not_implemented）', detail: 'v1 REST Action 查询尚未接线：契约声明该操作对所有调用方返回 NOT_FOUND，这不代表动作失败；请以插件桥接的 Action 查询为准。' }
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
            契约状态 not_implemented：`GET /actions/{'{actionId}'}` 与 `GET /actions/by-idempotency-key` 已进入 OpenAPI，
            但 v1 Core 尚未产生 control 拥有的 Action 记录，两个查询当前都返回 NOT_FOUND。本页只做接线与错误态展示，不伪造结果。
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
