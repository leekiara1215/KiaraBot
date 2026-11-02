/**
 * P12 preview catalog: page skeleton metadata, target operations and the
 * Q1–Q8 dependency list. This module is intentionally declarative — no network
 * calls and no mutations. Target endpoints are pulled from the typed endpoint
 * table in `api/client.ts`; `scripts/check-contracts.mjs` verifies every one of
 * them still exists in `api/openapi.yaml`.
 */
import { endpoints, type Endpoint } from '../api/client';
import * as fixtures from './fixtures';

export type P12PageKey =
  | 'p12-overview'
  | 'p12-accounts'
  | 'p12-installations'
  | 'p12-operations'
  | 'p12-tasks'
  | 'p12-model-config'
  | 'p12-health';

export type ContractStatus = 'not_implemented' | 'implemented' | 'reference';

export interface P12Operation {
  endpoint: Endpoint;
  operationId: string;
  /** Target permission declared by `x-permission` in OpenAPI. */
  permission: string;
  status: ContractStatus;
  /** Why the row is not wired yet / what it depends on. */
  note: string;
}

export interface P12FixtureTable {
  title: string;
  columns: string[];
  rows: string[][];
}

export interface P12Decision {
  id: 'Q1' | 'Q2' | 'Q3' | 'Q4' | 'Q5' | 'Q6' | 'Q7' | 'Q8';
  title: string;
  blocked: string;
}

export interface P12PageSpec {
  key: P12PageKey;
  label: string;
  icon: string;
  eyebrow: string;
  title: string;
  description: string;
  permission: string;
  operations: P12Operation[];
  fixtures: P12FixtureTable[];
  decisions: P12Decision['id'][];
  preview: string;
}

const operation = (
  endpoint: Endpoint,
  operationId: string,
  permission: string,
  status: ContractStatus,
  note: string,
): P12Operation => ({ endpoint, operationId, permission, status, note });

export const DECISIONS: Record<P12Decision['id'], P12Decision> = {
  Q1: {
    id: 'Q1',
    title: 'Action 管理查询的授权模型（QueryAction 调用方/action.inspect）',
    blocked: 'P12 动作管理页（按 action_id/幂等键查询、元数据列表）',
  },
  Q2: {
    id: 'Q2',
    title: '审计/日志查询归属（5 个 owner 查询 RPC 或显式 503 降级）',
    blocked: '日志/审计目标页迁移；现有 reference 页面在 P12 前保持只读占位',
  },
  Q3: {
    id: 'Q3',
    title: 'GET /groups 与群白名单产品语义（改用账号 ActionPolicy 或保留独立群实体）',
    blocked: '群白名单/账号危险动作策略页',
  },
  Q4: {
    id: 'Q4',
    title: '用户/主体目录（新增 IdentityService.ListPrincipals 或仅展示当前主体）',
    blocked: '用户与主体目录页；权限页保持现有 reference 用户/群组视图',
  },
  Q5: {
    id: 'Q5',
    title: '健康聚合契约（组件聚合写进 target /health，或仅六进程探针 + Control 本地聚合）',
    blocked: '健康与组件详情页；/health 聚合口径与降级展示',
  },
  Q6: {
    id: 'Q6',
    title: 'v1 /plugins* 迁移与 installations/instances Panel 语义',
    blocked: '安装/实例页的启用、停用、配置修改交互；只读列表可先展示骨架',
  },
  Q7: {
    id: 'Q7',
    title: '授权词表与固定角色扩展（含新能力的敏感操作显式授予规则）',
    blocked: '模型配置只读权限词表（model.manage vs 读权限）、各页权限门禁最终口径',
  },
  Q8: {
    id: 'Q8',
    title: 'P12 完成度口径（核心管理面 live + 未裁定能力显式 503 是否算验收通过）',
    blocked: 'P12 整体验收边界；本预览页保持“未接线”状态直到口径确认',
  },
};

const accounts: P12PageSpec = {
  key: 'p12-accounts',
  label: '账号（P12）',
  icon: '◍',
  eyebrow: 'P12 PREVIEW · ACCOUNTS',
  title: '账号管理',
  description: '目标 Runtime ListAccounts 的只读投影：账号、平台、生命周期与版本。当前仅预览 target schema，不发起请求。',
  permission: 'account.read',
  operations: [
    operation(endpoints.accounts, 'listAccounts', 'account.read', 'not_implemented', '待 P12 接线：catalog + 分页快照'),
  ],
  fixtures: [
    {
      title: 'TargetAccountRecord 预览',
      columns: ['账号 ID', '平台账号', '显示名', '版本', '生命周期'],
      rows: fixtures.accounts.map((account) => [
        account.account_id,
        `${account.platform}:${account.platform_account_id}`,
        account.display_name ?? '—',
        account.version,
        account.state,
      ]),
    },
  ],
  decisions: ['Q7', 'Q8'],
  preview: '账号详情、停用/恢复与 adapter 绑定不在本骨架内；写入操作随 P12 接线和 Q7 权限口径实施。',
};

const installations: P12PageSpec = {
  key: 'p12-installations',
  label: '安装与实例（P12）',
  icon: '⌗',
  eyebrow: 'P12 PREVIEW · INSTALLATIONS',
  title: '安装与实例',
  description: '目标 installations/instances 只读列表：插件安装、实例绑定、期望状态与进程事实。启用/配置交互等待 Q6。',
  permission: 'plugin.read',
  operations: [
    operation(endpoints.installations, 'listInstallations', 'plugin.read', 'not_implemented', '待 P12 接线'),
    operation(endpoints.installation, 'getInstallation', 'plugin.read', 'not_implemented', '待 P12 接线：按安装读取'),
    operation(endpoints.instances, 'listInstances', 'plugin.read', 'not_implemented', '待 P12 接线'),
    operation(endpoints.instance, 'getInstance', 'plugin.read', 'not_implemented', '待 P12 接线：按实例读取'),
  ],
  fixtures: [
    {
      title: 'TargetInstallationRecord 预览',
      columns: ['安装 ID', '插件类型', '版本', '生命周期'],
      rows: fixtures.installations.map((installation) => [
        installation.installation_id,
        installation.plugin_type_id,
        installation.version,
        installation.state,
      ]),
    },
    {
      title: 'TargetInstanceRecord 预览',
      columns: ['实例 ID', '安装', 'Host', '期望状态', '进程结果'],
      rows: fixtures.instances.map((instance) => [
        instance.instance_id,
        instance.installation_id,
        instance.host_id ?? '（外部管理）',
        instance.state.desired,
        instance.state.process_outcome,
      ]),
    },
  ],
  decisions: ['Q6', 'Q8'],
  preview: 'v1 /plugins 页面仍按 reference 行为运行；目标启停/配置修改需要 Q6 决定迁移语义。',
};

const operations: P12PageSpec = {
  key: 'p12-operations',
  label: '操作进度（P12）',
  icon: '◔',
  eyebrow: 'P12 PREVIEW · OPERATIONS',
  title: '操作与进度',
  description: '按 owner service 查询 operation：状态、execution_outcome、参与者进度与 stalled 标记。当前只做契约预览，不轮询。',
  permission: 'resource.read',
  operations: [
    operation(endpoints.operation, 'getOperation', 'resource.read', 'not_implemented', '待 P12 接线：owner + scope + operation_id'),
  ],
  fixtures: [
    {
      title: 'TargetOperation 预览',
      columns: ['owner', 'operation', '状态', 'execution_outcome', 'stalled'],
      rows: fixtures.operations.map((item) => [
        item.ref.owner_service,
        item.ref.operation_id,
        item.state,
        item.execution_outcome,
        item.stalled ? 'yes' : 'no',
      ]),
    },
    {
      title: '参与者进度',
      columns: ['operation', '参与者', '状态'],
      rows: fixtures.operations.flatMap((item) =>
        (item.participants ?? []).map((participant) => [
          item.ref.operation_id,
          participant.participant,
          participant.state,
        ])),
    },
  ],
  decisions: ['Q8'],
  preview: '操作进度轮询/推送属 P12 接线；页面在 live 模式下保持空状态，避免对 not_implemented 端点发请求。',
};

const tasks: P12PageSpec = {
  key: 'p12-tasks',
  label: '任务（P12）',
  icon: '◷',
  eyebrow: 'P12 PREVIEW · TASKS',
  title: '任务（目标记录）',
  description: '目标 Scheduler GetTask 的只读投影：owner instance、目标会话、payload 字节、调度状态与 occurrence。',
  permission: 'task.read',
  operations: [
    operation(endpoints.targetTask, 'getTask', 'task.read', 'not_implemented', '待 P12 接线：按 task_id + scope 读取'),
  ],
  fixtures: [
    {
      title: 'TargetTaskRecord 预览',
      columns: ['任务 ID', '实例', '目标', '状态', 'run_at'],
      rows: fixtures.tasks.map((task) => [
        task.task_id,
        task.owner_instance_id,
        `${task.target.type}:${task.target.id}`,
        task.state,
        task.run_at,
      ]),
    },
  ],
  decisions: ['Q8'],
  preview: 'v1 /tasks reference 列表页面保持现有行为；目标任务详情/取消接线随 P12 实施。',
};

const modelConfig: P12PageSpec = {
  key: 'p12-model-config',
  label: '模型配置（P12）',
  icon: '⚙',
  eyebrow: 'P12 PREVIEW · MODEL CONFIG',
  title: '模型配置（只读）',
  description: '目标 ModelService 只读配置投影：provider 元数据、endpoint、超时与配置版本。密钥轮换永远不在本页展示或提交。',
  permission: 'model.manage',
  operations: [
    operation(endpoints.modelMetadata, 'getModelMetadata', 'model.read', 'not_implemented', '待 P12 接线：模型元数据'),
    operation(endpoints.modelConfiguration, 'getModelConfiguration', 'model.manage', 'not_implemented', '待 P12 接线：只读配置；读权限口径待 Q7'),
  ],
  fixtures: [
    {
      title: 'TargetModelMetadata / TargetModelConfiguration 预览',
      columns: ['model', 'alias', 'provider', 'endpoint', 'timeout_ms', 'version'],
      rows: [[fixtures.modelMetadata.model_id, fixtures.modelMetadata.alias,
        fixtures.modelMetadata.provider_kind, fixtures.modelConfiguration.endpoint,
        String(fixtures.modelConfiguration.timeout_ms), fixtures.modelConfiguration.version]],
    },
  ],
  decisions: ['Q7', 'Q8'],
  preview: '配置读取目前声明为 model.manage；若 Q7 决定拆分只读权限，本页权限门禁随之调整。密钥轮换不进入 Panel 骨架。',
};

const health: P12PageSpec = {
  key: 'p12-health',
  label: '健康（P12）',
  icon: '♥',
  eyebrow: 'P12 PREVIEW · HEALTH',
  title: '健康与探针',
  description: '进程探针（live/ready）已实现；组件聚合 /health 的目标口径等待 Q5。本页只展示探针契约与只读预览。',
  permission: 'health.read',
  operations: [
    operation(endpoints.healthLive, 'getProcessLiveness', 'health.read', 'implemented', '进程存活探针已实现；本预览仍不请求'),
    operation(endpoints.healthReady, 'getProcessReadiness', 'health.read', 'implemented', '进程就绪探针已实现；本预览仍不请求'),
    operation(endpoints.health, 'getHealth', 'health.read', 'reference', 'reference 组件聚合；target 聚合契约待 Q5'),
  ],
  fixtures: [
    {
      title: 'ProcessProbe 预览',
      columns: ['探针', '状态', '观测时间'],
      rows: (Object.entries(fixtures.readiness.probes) as ['live' | 'ready', { state: string }][]).map(
        ([name, probe]) => [name, probe.state, fixtures.readiness.observed_at],
      ),
    },
  ],
  decisions: ['Q5'],
  preview: '组件明细、聚合降级规则与健康事件推送等待 Q5；本骨架不新增健康契约。',
};

const overview: P12PageSpec = {
  key: 'p12-overview',
  label: 'P12 预研总览',
  icon: '◌',
  eyebrow: 'P12 PREVIEW · OVERVIEW',
  title: 'P12 管理面预研',
  description: '本次只交付 Panel 骨架：target 类型、operation() 描述符、mock fixture 与漂移检查；不接真实后端，Q1–Q8 决策前不实施对应功能。',
  permission: 'health.read',
  operations: [],
  fixtures: [],
  decisions: ['Q1', 'Q2', 'Q3', 'Q4', 'Q5', 'Q6', 'Q7', 'Q8'],
  preview: '页面预览仅读取本机 target fixture；live 模式下不向 not_implemented 端点发起请求。',
};

export const P12_PAGES: Record<P12PageKey, P12PageSpec> = {
  'p12-overview': overview,
  'p12-accounts': accounts,
  'p12-installations': installations,
  'p12-operations': operations,
  'p12-tasks': tasks,
  'p12-model-config': modelConfig,
  'p12-health': health,
};

export const P12_PAGE_KEYS = Object.keys(P12_PAGES) as P12PageKey[];

/** Stable display ordering for navigation and tests. */
export const P12_NAV: { key: P12PageKey; label: string; icon: string }[] = P12_PAGE_KEYS.map(
  (key) => ({ key, label: P12_PAGES[key].label, icon: P12_PAGES[key].icon }),
);

export const contractStatusText: Record<ContractStatus, string> = {
  not_implemented: '待 P12（not_implemented）',
  implemented: '已实现（本页仍为只读预览）',
  reference: 'reference 契约（target 口径待决策）',
};
