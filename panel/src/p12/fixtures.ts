/**
 * P12 preview fixtures.
 *
 * These values are typed against the target OpenAPI records (`Schemas['Target*']`)
 * so `npm run lint` fails if a target field/required member drifts. They are only
 * rendered by the preview pages in mock mode; no preview page sends these values
 * to a server and no live request is made before P12 wiring.
 */
import type { Schemas } from '../api/client';

/** The generated anyOf widens TargetInstanceRecord into partial branches; the runtime record is
 * the branch carrying the required identity fields, so previews use that projection. */
export type TargetInstanceRecord = Extract<Schemas['TargetInstanceRecord'], { instance_id: string }>;

const observedAt = '2026-11-02T07:00:00Z';
const expiresAt = '2026-11-02T07:10:00Z';

export const accounts: Schemas['TargetAccountRecord'][] = [
  {
    account_id: 'account-qq-10001',
    platform: 'qq',
    platform_account_id: '10001',
    display_name: 'Kiara 测试账号',
    version: '7',
    state: 'LIFECYCLE_ACTIVE',
  },
  {
    account_id: 'account-qq-10002',
    platform: 'qq',
    platform_account_id: '10002',
    display_name: '待停用测试账号',
    version: '2',
    state: 'LIFECYCLE_DISABLED',
  },
];

export const installations: Schemas['TargetInstallationRecord'][] = [
  {
    installation_id: 'installation-welcome-01',
    plugin_type_id: 'welcome',
    manifest: {
      id: 'welcome',
      name: 'Welcome',
      version: '1.2.0',
      api_version: 'kiarabot.plugin.v1',
      runtime: 'python-rpc',
      entrypoint: 'welcome:create',
      capabilities: ['event.message.received'],
      permissions: ['message.read', 'message.send'],
      dependencies: [],
      config_schema: '{}',
      isolation: 'process',
      ordered: false,
    },
    version: '4',
    state: 'LIFECYCLE_ACTIVE',
  },
  {
    installation_id: 'installation-reminder-01',
    plugin_type_id: 'reminder',
    manifest: {
      id: 'reminder',
      name: 'Reminder',
      version: '0.9.0',
      api_version: 'kiarabot.plugin.v1',
      runtime: 'python-rpc',
      entrypoint: 'reminder:create',
      capabilities: ['event.timer.triggered'],
      permissions: ['task.schedule', 'message.send'],
      dependencies: [],
      config_schema: '{}',
      isolation: 'process',
      ordered: true,
    },
    version: '1',
    state: 'LIFECYCLE_ACTIVE',
  },
];

const instanceState = (instanceId: string, version: string, desired: 'DESIRED_RUNNING' | 'DESIRED_STOPPED'): TargetInstanceRecord['state'] => ({
  resource: { kind: 'RESOURCE_INSTANCE', id: instanceId },
  version,
  lifecycle: 'LIFECYCLE_ACTIVE',
  desired,
  registered: true,
  fence: {
    instance_id: instanceId,
    execution_generation: '3',
    connection_generation: '9',
    runtime_epoch: 'epoch-preview-01',
  },
  desired_config_version: '2',
  applied_config_version: '1',
  process_outcome: 'EXECUTION_KNOWN_SUCCEEDED',
  observed_at: observedAt,
  source_service: 'host',
});

export const instances: TargetInstanceRecord[] = [
  {
    instance_id: 'instance-welcome-01',
    installation_id: 'installation-welcome-01',
    host_id: 'host-local-01',
    externally_managed: false,
    state: instanceState('instance-welcome-01', '5', 'DESIRED_RUNNING'),
  },
  {
    instance_id: 'instance-reminder-01',
    installation_id: 'installation-reminder-01',
    externally_managed: true,
    state: instanceState('instance-reminder-01', '2', 'DESIRED_STOPPED'),
  },
];

export const operations: Schemas['TargetOperation'][] = [
  {
    ref: {
      owner_service: 'runtime',
      operation_id: 'operation-preview-01',
      resource: { kind: 'RESOURCE_INSTANCE', id: 'instance-welcome-01' },
    },
    version: '6',
    state: 'OPERATION_RUNNING',
    execution_outcome: 'EXECUTION_NOT_STARTED',
    participants: [
      { participant: 'runtime', state: 'OPERATION_RUNNING' },
      { participant: 'host', state: 'OPERATION_SUCCEEDED' },
    ],
    observed_at: observedAt,
    stalled: false,
  },
  {
    ref: {
      owner_service: 'scheduler',
      operation_id: 'operation-preview-02',
      resource: { kind: 'RESOURCE_TASK', id: 'task-preview-01' },
    },
    version: '2',
    state: 'OPERATION_BLOCKED',
    execution_outcome: 'EXECUTION_UNKNOWN',
    participants: [{ participant: 'scheduler', state: 'OPERATION_BLOCKED' }],
    observed_at: observedAt,
    reason: {
      code: 'UPSTREAM_UNAVAILABLE',
      message: '等待 Host 心跳',
      request_id: 'request-preview-02',
      trace_id: 'trace-preview-02',
      retryable: true,
    },
    next_attempt_at: '2026-11-02T07:00:30Z',
    stalled: true,
  },
];

export const tasks: Schemas['TargetTaskRecord'][] = [
  {
    task_id: 'task-preview-01',
    owner_instance_id: 'instance-reminder-01',
    installation_id: 'installation-reminder-01',
    target: { id: 'group-20001', type: 'GROUP', account_id: 'account-qq-10001' },
    payload: 'eyJ0ZXh0IjoiaGVsbG8ifQ==',
    run_at: '2026-11-03T01:00:00Z',
    state: 'SCHEDULED_TASK_PENDING',
    version: '3',
    occurrence_id: 'occurrence-preview-01',
  },
];

export const modelMetadata: Schemas['TargetModelMetadata'] = {
  model_id: 'model-preview-primary',
  alias: 'default',
  config_version: '4',
  state: 'LIFECYCLE_ACTIVE',
  provider_kind: 'openai-compatible',
};

export const modelConfiguration: Schemas['TargetModelConfiguration'] = {
  model_id: 'model-preview-primary',
  alias: 'default',
  provider_kind: 'openai-compatible',
  endpoint: 'https://model.example.test/v1',
  provider_model: 'preview-model',
  timeout_ms: 30000,
  version: '4',
};

export const readiness: { observed_at: string; expires_at: string; probes: Record<'live' | 'ready', Schemas['ProcessProbe']> } = {
  observed_at: observedAt,
  expires_at: expiresAt,
  probes: {
    live: { state: 'HEALTHY' },
    ready: { state: 'DEGRADED' },
  },
};
