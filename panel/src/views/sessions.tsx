import { useCallback, useEffect, useState } from 'react';
import { type ApiClient, type Page, type Session } from '../api/client';
import { describeError, FailureNotice, formatTime, label, StateTag, type Failure } from '../ui';

/** 会话列表与撤销：GET /auth/sessions + POST /auth/sessions/{id}/revoke（服务端决定可见范围）。 */
export function SessionsPanel({ api, permissions, revision }: { api: ApiClient; permissions: string[]; revision: number }) {
  const [page, setPage] = useState<Page<Session>>();
  const [failure, setFailure] = useState<Failure>();
  const [loading, setLoading] = useState(true);
  const [busy, setBusy] = useState('');
  const [notice, setNotice] = useState('');
  const canRead = permissions.includes('session.read');
  const canRevoke = permissions.includes('session.revoke');

  const load = useCallback(async () => {
    setLoading(true); setFailure(undefined);
    try { setPage(await api.sessions()); } catch (error) { setFailure(describeError(error)); } finally { setLoading(false); }
  }, [api]);

  useEffect(() => { void load(); }, [load, revision]);

  const revoke = async (session: Session) => {
    setBusy(session.session_id); setNotice(''); setFailure(undefined);
    try {
      await api.revokeSession(session.session_id);
      setNotice(`会话 ${session.session_id.slice(0, 8)}… 已撤销；被撤销访问令牌的 REST 请求与 WebSocket 会立即/最迟一秒内失效。`);
      await load();
    } catch (error) {
      setFailure(describeError(error));
    } finally {
      setBusy('');
    }
  };

  return (
    <section className="table-card">
      <div className="table-head">
        <div><p className="eyebrow">SESSIONS</p><h2>认证会话</h2></div>
        <span className="hint">管理员可管理全部会话；其他角色只能看到自己的会话（跨所有者返回 NOT_FOUND）</span>
      </div>
      {loading && <p className="empty">正在加载会话…</p>}
      {failure && <FailureNotice failure={failure} />}
      {notice && <p role="status" className="feedback ok">{notice}</p>}
      {!loading && page && (page.items.length === 0 ? <p className="empty">暂无可见会话</p> : (
        <div className="table-wrap">
          <table>
            <thead><tr><th>会话</th><th>用户</th><th>创建时间</th><th>过期时间</th><th>状态</th><th>操作</th></tr></thead>
            <tbody>
              {page.items.map((session) => (
                <tr key={session.session_id}>
                  <td>{session.session_id.slice(0, 12)}…</td>
                  <td>{session.user_id}</td>
                  <td>{formatTime(session.created_at)}</td>
                  <td>{formatTime(session.expires_at)}</td>
                  <td><StateTag text={label('session', session.state)} tone={session.state === 'ACTIVE' ? 'ok' : 'muted'} /></td>
                  <td className="row-actions">
                    <button className="secondary small" aria-label={`撤销会话 ${session.session_id.slice(0, 8)}`} disabled={!canRevoke || session.state !== 'ACTIVE' || busy === session.session_id} onClick={() => void revoke(session)}>撤销</button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ))}
      {!canRead && <p className="hint">当前角色没有 session.read，会话列表不可用。</p>}
    </section>
  );
}
