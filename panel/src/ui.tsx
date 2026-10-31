import { Component, useCallback, useState, type ErrorInfo, type ReactNode } from 'react';
import { ApiError, type Schemas } from './api/client';

export type FailureKind =
  | 'unauthorized' | 'forbidden' | 'not-found' | 'conflict' | 'rate-limited'
  | 'unavailable' | 'timeout' | 'invalid' | 'internal' | 'network' | 'unknown';

export interface Failure {
  kind: FailureKind;
  title: string;
  detail: string;
  status?: number;
  code?: string;
  requestId?: string;
  retryAfter?: number;
}

/** Map transport/server failures to stable, user-facing categories (no business rules). */
export function describeError(error: unknown): Failure {
  if (error instanceof ApiError) {
    const base = { status: error.status, code: error.code, requestId: error.requestId, retryAfter: error.retryAfter };
    switch (error.status) {
      case 401: return { ...base, kind: 'unauthorized', title: '会话已失效', detail: '登录凭据已过期或被撤销，请重新登录。' };
      case 403: return { ...base, kind: 'forbidden', title: '权限不足', detail: '服务端拒绝该操作（PERMISSION_DENIED），当前角色没有所需权限。' };
      case 404: return { ...base, kind: 'not-found', title: '未找到', detail: '资源不存在、不可见，或该契约操作尚未实现（NOT_FOUND 不代表操作失败）。' };
      case 409: return { ...base, kind: 'conflict', title: '状态冲突', detail: '资源已被其他请求修改或幂等键载荷不一致，请重新读取后再提交。' };
      case 429: return { ...base, kind: 'rate-limited', title: '请求过于频繁', detail: `已被限流，请稍后重试${error.retryAfter ? `（约 ${error.retryAfter} 秒）` : '（见 Retry-After）'}。` };
      case 400: return { ...base, kind: 'invalid', title: '请求参数无效', detail: '服务端拒绝该输入（INVALID_ARGUMENT）。' };
      case 503: return { ...base, kind: 'unavailable', title: '依赖不可用', detail: 'Core、插件或能力暂不可用（UPSTREAM_UNAVAILABLE / CAPABILITY_NOT_SUPPORTED）。' };
      case 504: return { ...base, kind: 'timeout', title: '请求超时', detail: '等待依赖超时（TIMEOUT）；写操作结果未知时，重试不等于确认失败。' };
      default: return { ...base, kind: 'internal', title: '服务端错误', detail: `HTTP ${error.status}${error.code ? `（${error.code}）` : ''}。` };
    }
  }
  if (error instanceof Error) return { kind: 'network', title: '请求失败', detail: error.message };
  return { kind: 'unknown', title: '未知错误', detail: '请稍后重试。' };
}

export function FailureNotice({ failure }: { failure: Failure }) {
  return (
    <div role="alert" className={`failure ${failure.kind}`}>
      <strong>{failure.title}</strong>
      <p>{failure.detail}</p>
      <p className="hint">
        {failure.status !== undefined && <span>HTTP {failure.status} </span>}
        {failure.code && <span>code={failure.code} </span>}
        {failure.requestId && <span>request_id={failure.requestId}</span>}
      </p>
    </div>
  );
}

export type ActionPhase = 'idle' | 'running' | 'ok' | 'error';
export interface ActionState { phase: ActionPhase; message?: string; failure?: Failure }

/** Small shared helper for "loading / success / failure" per operation. */
export function useAction() {
  const [state, setState] = useState<ActionState>({ phase: 'idle' });
  const run = useCallback(async (label: string, task: () => Promise<string | void>) => {
    setState({ phase: 'running', message: `${label}…` });
    try {
      const done = await task();
      setState({ phase: 'ok', message: done || `${label}成功` });
    } catch (error) {
      setState({ phase: 'error', failure: describeError(error) });
    }
  }, []);
  return { state, run, reset: () => setState({ phase: 'idle' }) };
}

export function ActionFeedback({ state }: { state: ActionState }) {
  if (state.phase === 'idle') return null;
  if (state.phase === 'error' && state.failure) return <FailureNotice failure={state.failure} />;
  return <p role="status" className={`feedback ${state.phase}`}>{state.phase === 'running' ? '… ' : '✓ '}{state.message}</p>;
}

const pluginStates: Record<string, string> = {
  PLUGIN_STATE_UNSPECIFIED: '状态未知', PLUGIN_STATE_DISABLED: '已禁用', PLUGIN_STATE_STARTING: '启动中',
  PLUGIN_STATE_READY: '就绪', PLUGIN_STATE_RUNNING: '运行中', PLUGIN_STATE_STOPPING: '停止中',
  PLUGIN_STATE_STOPPED: '已停止', PLUGIN_STATE_UNAVAILABLE: '不可用', PLUGIN_STATE_FAILED: '失败',
};
const healthStates: Record<string, string> = { HEALTH_STATE_UNSPECIFIED: '状态未知', HEALTHY: '正常', DEGRADED: '降级', UNHEALTHY: '不可用' };
const sessionStates: Record<string, string> = { ACTIVE: '有效', REVOKED: '已撤销', EXPIRED: '已过期' };
const deliveryStates: Record<string, string> = { UNAVAILABLE: '无活动连接', PENDING: '待应用', APPLIED: '已应用', REJECTED: '被拒绝' };
const actionStates: Record<string, string> = { ACTION_STATUS_UNSPECIFIED: '未知', ACCEPTED: '已接受（未终结）', SUCCEEDED: '成功', FAILED: '失败', EXPIRED: '已过期', CANCELLED: '已取消' };
const logLevels: Record<string, string> = { DEBUG: '调试', INFO: '信息', WARNING: '警告', WARN: '警告', ERROR: '错误' };

/** Convert server enums to display labels; unknown values (forward compatibility) are shown verbatim. */
export function label(kind: 'plugin' | 'health' | 'session' | 'delivery' | 'action' | 'log', value?: string): string {
  if (!value) return '—';
  const table = kind === 'plugin' ? pluginStates : kind === 'health' ? healthStates : kind === 'session' ? sessionStates
    : kind === 'delivery' ? deliveryStates : kind === 'action' ? actionStates : logLevels;
  return table[value] ?? value;
}

export function formatTime(value?: string): string {
  if (!value) return '—';
  const date = new Date(value);
  return Number.isNaN(date.valueOf()) ? value : date.toLocaleString('zh-CN');
}

export function StateTag({ text, tone = 'ok' }: { text: string; tone?: 'ok' | 'warn' | 'bad' | 'muted' }) {
  return <span className={`tag tone-${tone}`}>{text}</span>;
}

export function JsonField({ id, label: text, value, onChange, rows = 6, disabled }: {
  id: string; label: string; value: string; onChange: (value: string) => void; rows?: number; disabled?: boolean;
}) {
  return (
    <label className="json-field" htmlFor={id}>
      {text}
      <textarea id={id} rows={rows} value={value} disabled={disabled} spellCheck={false} onChange={(event) => onChange(event.target.value)} />
    </label>
  );
}

/** Render an untrusted server object as text inside <pre>; never as HTML. */
export function JsonPreview({ value }: { value: unknown }) {
  return <pre className="json-preview">{JSON.stringify(value, null, 2)}</pre>;
}

interface BoundaryState { error?: Error }

/** Last-resort UI guard: a render exception shows a retry card instead of a blank page. */
export class ErrorBoundary extends Component<{ children: ReactNode; onReset?: () => void }, BoundaryState> {
  state: BoundaryState = {};
  static getDerivedStateFromError(error: Error): BoundaryState { return { error }; }
  componentDidCatch(error: Error, info: ErrorInfo) { console.error('panel render failure', error, info.componentStack); }
  render() {
    if (!this.state.error) return this.props.children;
    return (
      <div role="alert" className="shell-error">
        <h2>界面渲染失败</h2>
        <p>已捕获意外异常，其他功能仍在运行。</p>
        <p className="hint">{this.state.error.message}</p>
        <button className="secondary" onClick={() => { this.setState({ error: undefined }); this.props.onReset?.(); }}>重试渲染</button>
      </div>
    );
  }
}

export type { Schemas };
