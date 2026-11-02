/**
 * P12 preview pages: contract-first skeletons.
 *
 * Every page renders the target operations (path/method/permission/contract status)
 * and, in mock mode only, fixtures typed against the generated target schemas.
 * No page calls the API: target handlers are `not_implemented` and stay unwired
 * until P12 (and the Q1–Q8 decisions) are confirmed.
 */
import type { P12Decision, P12PageKey } from '../p12/catalog';
import { contractStatusText, DECISIONS, P12_PAGES } from '../p12/catalog';

function ContractTable({ page }: { page: P12PageKey }) {
  const spec = P12_PAGES[page];
  if (spec.operations.length === 0) return null;
  return (
    <section className="table-card">
      <div className="table-head">
        <div>
          <p className="eyebrow">TARGET CONTRACT</p>
          <h2>契约端点</h2>
        </div>
        <span className="count">{spec.operations.length} 个 operation()</span>
      </div>
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>方法</th><th>路径</th><th>operationId</th><th>权限</th><th>状态</th><th>备注</th>
            </tr>
          </thead>
          <tbody>
            {spec.operations.map((item) => (
              <tr key={item.operationId}>
                <td>{item.endpoint.method.toUpperCase()}</td>
                <td>{item.endpoint.path}</td>
                <td>{item.operationId}</td>
                <td>{item.permission}</td>
                <td><span className={`tag tone-${item.status === 'not_implemented' ? 'warn' : item.status === 'implemented' ? 'ok' : 'muted'}`}>{contractStatusText[item.status]}</span></td>
                <td>{item.note}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}

function FixtureTables({ page, mock }: { page: P12PageKey; mock: boolean }) {
  const spec = P12_PAGES[page];
  if (spec.fixtures.length === 0) return null;
  if (!mock) {
    return (
      <section className="table-card">
        <div className="table-head"><div><p className="eyebrow">MOCK-ONLY PREVIEW</p><h2>页面预览</h2></div><span className="tag tone-muted">live 模式未接线</span></div>
        <p className="empty">本页是 P12 预研骨架：live 模式不会向 not_implemented 端点发起请求；切换到 mock 模式可查看 target schema 预览。</p>
      </section>
    );
  }
  return (
    <section className="table-card">
      <div className="table-head">
        <div><p className="eyebrow">MOCK-ONLY PREVIEW · TARGET SCHEMAS</p><h2>Target 数据预览</h2></div>
        <span className="count">仅 mock 数据，不发请求</span>
      </div>
      {spec.fixtures.map((table) => (
        <div key={table.title}>
          <div className="table-head"><div><p className="eyebrow">FIXTURE</p><h3>{table.title}</h3></div><span className="count">{table.rows.length} 条</span></div>
          <div className="table-wrap">
            <table>
              <thead><tr>{table.columns.map((column) => <th key={column}>{column}</th>)}</tr></thead>
              <tbody>
                {table.rows.length === 0
                  ? <tr><td colSpan={table.columns.length}>暂无数据</td></tr>
                  : table.rows.map((row, index) => <tr key={index}>{row.map((cell, cellIndex) => <td key={cellIndex}>{cell}</td>)}</tr>)}
              </tbody>
            </table>
          </div>
        </div>
      ))}
    </section>
  );
}

function DecisionTable({ decisions }: { decisions: P12Decision['id'][] }) {
  return (
    <section className="table-card">
      <div className="table-head">
        <div><p className="eyebrow">OPEN DECISIONS · Q1–Q8</p><h2>需要用户确认的依赖</h2></div>
        <span className="count">{decisions.length} 项</span>
      </div>
      <div className="table-wrap">
        <table>
          <thead><tr><th>决策</th><th>内容</th><th>阻塞的页面/功能</th></tr></thead>
          <tbody>
            {decisions.map((id) => {
              const decision = DECISIONS[id];
              return <tr key={id}><td>{decision.id}</td><td>{decision.title}</td><td>{decision.blocked}</td></tr>;
            })}
          </tbody>
        </table>
      </div>
    </section>
  );
}

export function P12PreviewView({ page, mock }: { page: P12PageKey; mock: boolean }) {
  const spec = P12_PAGES[page];
  const pending = spec.operations.filter((item) => item.status === 'not_implemented').length;
  return (
    <div className="stack">
      <section className="hero">
        <div>
          <p className="eyebrow">{spec.eyebrow}</p>
          <h2>{spec.title}</h2>
          <p>{spec.description}</p>
        </div>
        <div>
          <span className="tag tone-warn">P12 预研 · 未接线</span>
          {pending > 0 && <span className="tag tone-muted">{pending} 个端点待 P12</span>}
        </div>
      </section>
      {page === 'p12-overview' && (
        <section className="table-card">
          <div className="table-head"><div><p className="eyebrow">SCOPE</p><h2>本次预研范围</h2></div><span className="count">0 个真实请求</span></div>
          <p>只交付页面骨架/类型/mock fixture/漂移检查：账号、安装与实例、操作进度、任务、模型配置只读、健康探针。不接真实后端、不改 Control/契约；未接线端点只展示契约与「待 P12」状态。</p>
        </section>
      )}
      <ContractTable page={page} />
      <FixtureTables page={page} mock={mock} />
      <DecisionTable decisions={spec.decisions} />
      <section className="table-card">
        <div className="table-head"><div><p className="eyebrow">IMPLEMENTATION NOTE</p><h2>待 P12 说明</h2></div></div>
        <p>{spec.preview}</p>
        <p className="muted">mock 模式仅渲染 <code>src/p12/fixtures.ts</code> 中由 target schema 约束的本地数据；live 模式保持空状态且不发起请求。</p>
      </section>
    </div>
  );
}
