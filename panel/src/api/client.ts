import type { components, paths } from '../generated/openapi';

export type Schemas = components['schemas'];
export type TokenPair = Schemas['TokenPair'];
export type Health = Schemas['Health'];
export type Group = Schemas['Group'];
export type User = Schemas['User'];
export type Session = Schemas['Session'];
export type LogEntry = Schemas['LogEntry'];
export type Adapter = Schemas['Adapter'];
export type Plugin = Schemas['Plugin'];
export type GenericResource = Schemas['GenericResource'];
export type AuditEntry = Schemas['AuditEntry'];
export type PluginManifest = Schemas['PluginManifest'];
export type ValidationResult = Schemas['ValidationResult'];
export type PluginConfig = Schemas['PluginConfig'];
export type PluginConfigUpdate = Schemas['PluginConfigUpdate'];
export type ActionResult = Schemas['ActionResult'];
export type LoginRequest = paths['/auth/login']['post']['requestBody']['content']['application/json'];
export type Page<T> = { items: T[]; page: Schemas['PageInfo'] };
export type Fetcher = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>;

/**
 * Endpoint descriptors are declared through `operation()`, whose type parameters
 * are keys of the generated `paths` type. Renaming or removing an operation in
 * `api/openapi.yaml` therefore fails `npm run lint` (tsc), and
 * `scripts/check-contracts.mjs` re-validates the same table against the YAML.
 */
export function operation<P extends keyof paths, M extends keyof paths[P] & string>(path: P, method: M) {
  return { path, method } as const;
}
export interface Endpoint {
  readonly path: string;
  readonly method: string;
}

export const endpoints = {
  login: operation('/auth/login', 'post'),
  refresh: operation('/auth/refresh', 'post'),
  principal: operation('/auth/me', 'get'),
  sessions: operation('/auth/sessions', 'get'),
  revokeSession: operation('/auth/sessions/{sessionId}/revoke', 'post'),
  health: operation('/health', 'get'),
  bots: operation('/bots', 'get'),
  adapters: operation('/adapters', 'get'),
  plugins: operation('/plugins', 'get'),
  models: operation('/models', 'get'),
  groups: operation('/groups', 'get'),
  users: operation('/users', 'get'),
  tasks: operation('/tasks', 'get'),
  logs: operation('/logs', 'get'),
  audit: operation('/audit', 'get'),
  pluginValidate: operation('/plugins/validate', 'post'),
  pluginEnable: operation('/plugins/{pluginId}/enable', 'post'),
  pluginDisable: operation('/plugins/{pluginId}/disable', 'post'),
  pluginConfig: operation('/plugins/{pluginId}/config', 'get'),
  pluginConfigUpdate: operation('/plugins/{pluginId}/config', 'put'),
  pluginHealth: operation('/plugins/{pluginId}/health', 'get'),
  actionById: operation('/actions/{actionId}', 'get'),
  actionByKey: operation('/actions/by-idempotency-key', 'get'),
  // P12 target management surface. These descriptors are declared so
  // `scripts/check-contracts.mjs` and the generated `paths` type keep the panel
  // honest; the live pages consume them through the ApiClient methods below.
  healthLive: operation('/health/live', 'get'),
  healthReady: operation('/health/ready', 'get'),
  accounts: operation('/accounts', 'get'),
  account: operation('/accounts/{accountId}', 'get'),
  disableAccount: operation('/accounts/{accountId}/disable', 'post'),
  installations: operation('/installations', 'get'),
  installation: operation('/installations/{installationId}', 'get'),
  instances: operation('/instances', 'get'),
  instance: operation('/instances/{instanceId}', 'get'),
  operation: operation('/operations/{ownerService}/{operationId}', 'get'),
  targetTask: operation('/tasks/{taskId}', 'get'),
  modelMetadata: operation('/models/{modelId}', 'get'),
  modelConfiguration: operation('/models/{modelId}/configuration', 'get'),
  // P12d live management surface for the P12a contract operations.
  bindAdapter: operation('/adapter-bindings/{accountId}', 'put'),
  unbindAdapter: operation('/adapter-bindings/{accountId}', 'delete'),
  principals: operation('/principals', 'get'),
  createInstallation: operation('/installations', 'post'),
  createInstance: operation('/installations/{installationId}/instances', 'post'),
  modelUpdate: operation('/models/{modelId}', 'put'),
  modelSecret: operation('/models/{modelId}/secret', 'put'),
  actionMetadata: operation('/actions/metadata', 'get'),
  actionPolicy: operation('/accounts/{accountId}/action-policy', 'get'),
  actionPolicyUpdate: operation('/accounts/{accountId}/action-policy', 'put'),
  instanceConfig: operation('/instances/{instanceId}/config', 'get'),
  instanceConfigUpdate: operation('/instances/{instanceId}/config', 'put'),
  instanceDesiredState: operation('/instances/{instanceId}/desired-state', 'put'),
  cancelTask: operation('/tasks/{taskId}/cancel', 'post'),
} as const;

// Target projection types used by the P12 preview fixtures. They come from the
// same generated OpenAPI document as every other panel type.
export type TargetAccountRecord = Schemas['TargetAccountRecord'];
export type TargetInstallationRecord = Schemas['TargetInstallationRecord'];
export type TargetInstanceRecord = Schemas['TargetInstanceRecord'];
export type TargetModelConfiguration = Schemas['TargetModelConfiguration'];
export type TargetModelMetadata = Schemas['TargetModelMetadata'];
export type TargetResourceRef = Schemas['TargetResourceRef'];
export type TargetActionMetadata = Schemas['TargetActionMetadata'];
export type TargetActionPolicy = Schemas['TargetActionPolicy'];
export type TargetOperation = Schemas['TargetOperation'];
export type TargetTaskRecord = Schemas['TargetTaskRecord'];
export type ProcessProbe = Schemas['ProcessProbe'];
export type TargetAccountPage = Schemas['TargetRuntimeListAccountsResponse'];
export type TargetInstallationPage = Schemas['TargetRuntimeListInstallationsResponse'];
export type TargetOperationResponse = Schemas['TargetOwnerOperationResponse'];
export type TargetTaskResponse = Schemas['TargetSchedulerGetTaskResponse'];
export type TargetModelMetadataResponse = Schemas['TargetModelGetModelMetadataResponse'];
export type TargetModelConfigurationResponse = Schemas['TargetModelGetModelConfigurationResponse'];
export type TargetActionMetadataResponse = Schemas['TargetRuntimeListActionMetadataResponse'];
export type TargetAdapterBindingRequest = Schemas['TargetRuntimeBindAdapterRequest'];
export type TargetAdapterBindingResponse = Schemas['TargetRuntimeBindAdapterResponse'];
export type TargetUnbindAdapterRequest = Schemas['TargetRuntimeUnbindAdapterRequest'];
export type TargetInstanceDesiredStateRequest = Schemas['TargetRuntimeSetDesiredStateRequest'];
export type TargetInstanceConfigUpdateRequest = Schemas['TargetRuntimeUpdateConfigRequest'];
export type TargetInstanceConfigResponse = Schemas['TargetRuntimeGetInstanceConfigResponse'];
export type TargetCancelTaskRequest = Schemas['TargetSchedulerCancelTaskRequest'];
export type TargetActionPolicyUpdateRequest = Schemas['TargetRuntimeUpdateActionPolicyRequest'];
export type TargetResourceScope = Schemas['TargetResourceScope'];
export type TargetCatalogRef = Schemas['TargetCatalogRef'];
export type TargetTaskPage = Schemas['TaskPage'];
export type TargetPrincipalPage = Schemas['TargetIdentityListPrincipalsResponse'];
export type TargetPrincipalSummary = Schemas['TargetIdentityPrincipalSummary'];
export type TargetCreateInstallationRequest = Schemas['TargetRuntimeCreateInstallationRequest'];
export type TargetCreateInstallationResponse = Schemas['TargetRuntimeCreateInstallationResponse'];
export type TargetCreateInstanceRequest = Schemas['TargetRuntimeCreateInstanceRequest'];
export type TargetCreateInstanceResponse = Schemas['TargetRuntimeCreateInstanceResponse'];
export type TargetModelUpdateRequest = Schemas['TargetModelUpdateModelRequest'];
export type TargetModelUpdateResponse = Schemas['TargetModelUpdateModelResponse'];
export type TargetModelRotateSecretRequest = Schemas['TargetModelRotateProviderSecretRequest'];
export type TargetModelRotateSecretResponse = Schemas['TargetModelRotateProviderSecretResponse'];
export type TargetOperationRef = Schemas['TargetOperationRef'];

/** Query encoding for JSON object parameters (`scope`, `catalog`) used by P12c owner gateways. */
export function queryJson(value: unknown): string | undefined {
  return value === undefined || value === null ? undefined : JSON.stringify(value);
}

export class ApiError extends Error {
  constructor(
    public readonly status: number,
    public readonly code: string,
    message: string,
    public readonly requestId?: string,
    public readonly retryAfter?: number,
  ) {
    super(message);
    this.name = 'ApiError';
  }
}

interface RequestOptions {
  body?: unknown;
  auth?: boolean;
  headers?: Record<string, string>;
  query?: Record<string, string | number | undefined>;
  params?: Record<string, string>;
}

export class ApiClient {
  private accessToken?: string;
  private unauthorized?: () => void;
  setUnauthorizedHandler(handler?: () => void) { this.unauthorized = handler; }
  principal() { return this.call<Schemas['Principal']>(endpoints.principal); }
  sessions(token?: string, pageSize?: number) { return this.list<Session>(endpoints.sessions, { token, pageSize }); }
  revokeSession(sessionId: string) { return this.call<void>(endpoints.revokeSession, { param: sessionId }); }
  constructor(private readonly fetcher: Fetcher = fetch.bind(globalThis), private readonly base = '/api/v1') {}
  setToken(token?: string) { this.accessToken = token; }
  async login(input: LoginRequest): Promise<TokenPair> {
    const tokens = await this.call<TokenPair>(endpoints.login, { body: input, auth: false });
    this.setToken(tokens.access_token);
    return tokens;
  }
  async refresh(refreshToken: string): Promise<TokenPair> {
    const previous = this.accessToken;
    const tokens = await this.call<TokenPair>(endpoints.refresh, { body: { refresh_token: refreshToken }, auth: false });
    if (this.accessToken === previous) this.setToken(tokens.access_token);
    return tokens;
  }
  health() { return this.call<Health>(endpoints.health); }
  healthLive() { return this.callRoot<ProcessProbe>(endpoints.healthLive); }
  healthReady() { return this.callRoot<ProcessProbe>(endpoints.healthReady); }
  accountPage(catalog?: TargetCatalogRef, token?: string, pageSize?: number) {
    return this.call<TargetAccountPage>(endpoints.accounts, {
      query: { catalog: queryJson(catalog), page_token: token, page_size: pageSize } });
  }
  account(accountId: string) { return this.call<Schemas['TargetRuntimeGetAccountResponse']>(endpoints.account, { param: accountId }); }
  disableAccount(accountId: string, expectedVersion: string) {
    return this.call<Schemas['TargetRuntimeDisableAccountResponse']>(endpoints.disableAccount, {
      param: accountId, body: { account_id: accountId, expected_version: expectedVersion } });
  }
  bindAdapter(accountId: string, input: TargetAdapterBindingRequest) {
    return this.call<TargetAdapterBindingResponse>(endpoints.bindAdapter, { param: accountId, body: input });
  }
  unbindAdapter(accountId: string, input: TargetUnbindAdapterRequest) {
    return this.call<Schemas['TargetRuntimeUnbindAdapterResponse']>(endpoints.unbindAdapter, { param: accountId, body: input });
  }
  installationPage(catalog?: TargetCatalogRef, token?: string, pageSize?: number) {
    return this.call<TargetInstallationPage>(endpoints.installations, {
      query: { catalog: queryJson(catalog), page_token: token, page_size: pageSize } });
  }
  createInstallation(input: TargetCreateInstallationRequest) {
    return this.call<TargetCreateInstallationResponse>(endpoints.createInstallation, { body: input });
  }
  createInstance(installationId: string, input: TargetCreateInstanceRequest) {
    return this.call<TargetCreateInstanceResponse>(endpoints.createInstance, {
      param: installationId, body: input });
  }
  principalPage(token?: string, pageSize?: number, state?: string) {
    return this.call<TargetPrincipalPage>(endpoints.principals, {
      query: { page_token: token, page_size: pageSize, state } });
  }
  taskPage(scope?: TargetResourceScope, token?: string, pageSize?: number) {
    return this.call<TargetTaskPage>(endpoints.tasks, {
      query: { scope: queryJson(scope), page_token: token, page_size: pageSize } });
  }
  operation(ownerService: string, operationId: string, scope?: TargetResourceScope) {
    return this.call<TargetOperationResponse>(endpoints.operation, {
      params: { ownerService, operationId }, query: { scope: queryJson(scope) } });
  }
  targetTask(taskId: string, scope?: TargetResourceScope) {
    return this.call<TargetTaskResponse>(endpoints.targetTask, { param: taskId, query: { scope: queryJson(scope) } });
  }
  cancelTask(taskId: string, input: TargetCancelTaskRequest, scope?: TargetResourceScope) {
    return this.call<Schemas['TargetSchedulerCancelTaskResponse']>(endpoints.cancelTask, {
      param: taskId, body: input, query: { scope: queryJson(scope) } });
  }
  modelMetadata(modelId: string, scope?: TargetResourceScope) {
    return this.call<TargetModelMetadataResponse>(endpoints.modelMetadata, { param: modelId, query: { scope: queryJson(scope) } });
  }
  modelConfiguration(modelId: string, scope?: TargetResourceScope) {
    return this.call<TargetModelConfigurationResponse>(endpoints.modelConfiguration, { param: modelId, query: { scope: queryJson(scope) } });
  }
  updateModel(modelId: string, input: TargetModelUpdateRequest) {
    return this.call<TargetModelUpdateResponse>(endpoints.modelUpdate, { param: modelId, body: input });
  }
  rotateModelSecret(modelId: string, input: TargetModelRotateSecretRequest) {
    return this.call<TargetModelRotateSecretResponse>(endpoints.modelSecret, { param: modelId, body: input });
  }
  actionMetadata(filters: { accountId: string; pageSize?: number; pageToken?: string; state?: string; kind?: string }) {
    return this.call<TargetActionMetadataResponse>(endpoints.actionMetadata, {
      query: { account_id: filters.accountId, page_size: filters.pageSize, page_token: filters.pageToken, state: filters.state, kind: filters.kind } });
  }
  actionPolicy(accountId: string) { return this.call<Schemas['TargetRuntimeGetActionPolicyResponse']>(endpoints.actionPolicy, { param: accountId }); }
  updateActionPolicy(accountId: string, input: TargetActionPolicyUpdateRequest) {
    return this.call<Schemas['TargetRuntimeUpdateActionPolicyResponse']>(endpoints.actionPolicyUpdate, { param: accountId, body: input });
  }
  instanceConfig(instanceId: string, scope?: TargetResourceScope) {
    return this.call<TargetInstanceConfigResponse>(endpoints.instanceConfig, { param: instanceId, query: { scope: queryJson(scope) } });
  }
  updateInstanceConfig(instanceId: string, input: TargetInstanceConfigUpdateRequest) {
    return this.call<Schemas['TargetRuntimeUpdateConfigResponse']>(endpoints.instanceConfigUpdate, { param: instanceId, body: input });
  }
  setInstanceDesiredState(instanceId: string, input: TargetInstanceDesiredStateRequest) {
    return this.call<Schemas['TargetRuntimeSetDesiredStateResponse']>(endpoints.instanceDesiredState, { param: instanceId, body: input });
  }
  bots(token?: string, pageSize?: number) { return this.list<GenericResource>(endpoints.bots, { token, pageSize }); }
  adapters(token?: string, pageSize?: number) { return this.list<Adapter>(endpoints.adapters, { token, pageSize }); }
  plugins(token?: string, pageSize?: number) { return this.list<Plugin>(endpoints.plugins, { token, pageSize }); }
  models(token?: string, pageSize?: number) { return this.list<GenericResource>(endpoints.models, { token, pageSize }); }
  groups(token?: string, pageSize?: number) { return this.list<Group>(endpoints.groups, { token, pageSize }); }
  users(token?: string, pageSize?: number) { return this.list<User>(endpoints.users, { token, pageSize }); }
  tasks(token?: string, pageSize?: number) { return this.list<GenericResource>(endpoints.tasks, { token, pageSize }); }
  logs(token?: string, pageSize?: number) { return this.list<LogEntry>(endpoints.logs, { token, pageSize }); }
  audit(token?: string, pageSize?: number) { return this.list<AuditEntry>(endpoints.audit, { token, pageSize }); }
  validatePlugin(manifest: PluginManifest) {
    return this.call<ValidationResult>(endpoints.pluginValidate, { body: manifest });
  }
  async enablePlugin(pluginId: string) { await this.call<void>(endpoints.pluginEnable, { param: pluginId }); }
  async disablePlugin(pluginId: string) { await this.call<void>(endpoints.pluginDisable, { param: pluginId }); }
  pluginConfig(pluginId: string) { return this.call<PluginConfig>(endpoints.pluginConfig, { param: pluginId }); }
  updatePluginConfig(pluginId: string, update: PluginConfigUpdate) {
    return this.call<PluginConfig>(endpoints.pluginConfigUpdate, { param: pluginId, body: update });
  }
  pluginHealth(pluginId: string) { return this.call<Health>(endpoints.pluginHealth, { param: pluginId }); }
  action(actionId: string) { return this.call<ActionResult>(endpoints.actionById, { param: actionId }); }
  actionByKey(key: string) {
    return this.call<ActionResult>(endpoints.actionByKey, { headers: { 'Idempotency-Key': key } });
  }

  private list<T>(endpoint: Endpoint, options: { token?: string; pageSize?: number } = {}): Promise<Page<T>> {
    return this.call<Page<T>>(endpoint, {
      query: { page_token: options.token, page_size: options.pageSize },
    });
  }

  /** Substitute `{name}` placeholders; the descriptor table above is typed against `paths`. */
  private url(endpoint: Endpoint, param?: string, params: Record<string, string> = {}): string {
    let path = endpoint.path;
    if (param !== undefined) {
      if (!path.includes('{')) throw new Error(`endpoint ${path} takes no path parameter`);
      path = path.replace(/\{[^}]+\}/, encodeURIComponent(param));
    }
    for (const [name, value] of Object.entries(params)) path = path.replace(`{${name}}`, encodeURIComponent(value));
    if (path.includes('{')) throw new Error(`endpoint ${endpoint.path} has an unsubstituted parameter`);
    return path;
  }

  private async call<T>(endpoint: Endpoint, options: RequestOptions & { param?: string } = {}): Promise<T> {
    return this.dispatch<T>(endpoint, options, this.base);
  }
  /** Process probes are served at the root, not under /api/v1. */
  private async callRoot<T>(endpoint: Endpoint, options: RequestOptions = {}): Promise<T> {
    return this.dispatch<T>(endpoint, options, '');
  }
  private async dispatch<T>(endpoint: Endpoint, options: RequestOptions & { param?: string }, base: string): Promise<T> {
    const query = Object.entries(options.query ?? {})
      .filter(([, value]) => value !== undefined && value !== '')
      .map(([key, value]) => `${encodeURIComponent(key)}=${encodeURIComponent(String(value))}`)
      .join('&');
    const path = this.url(endpoint, options.param, options.params) + (query ? `?${query}` : '');
    return this.request<T>(path, endpoint.method, options, base);
  }

  private async request<T>(path: string, method: string, options: RequestOptions, base = this.base): Promise<T> {
    const requestToken = this.accessToken;
    const headers = new Headers({ Accept: 'application/json' });
    if (options.body !== undefined) headers.set('Content-Type', 'application/json');
    if (method !== 'GET') headers.set('Idempotency-Key', crypto.randomUUID());
    for (const [name, value] of Object.entries(options.headers ?? {})) headers.set(name, value);
    if (options.auth !== false && this.accessToken) headers.set('Authorization', `Bearer ${this.accessToken}`);
    const response = await this.fetcher(`${base}${path}`, {
      method,
      headers,
      body: options.body === undefined ? undefined : JSON.stringify(options.body),
    });
    if (!response.ok) {
      if (response.status === 401 && options.auth !== false && requestToken === this.accessToken) this.unauthorized?.();
      const error: Partial<Schemas['Error']> = await response.json().catch(() => ({}));
      const retryAfter = Number(response.headers.get('Retry-After'));
      throw new ApiError(
        response.status,
        error.code ?? 'INTERNAL',
        error.message ?? `HTTP ${response.status}`,
        error.request_id,
        Number.isFinite(retryAfter) && retryAfter > 0 ? retryAfter : undefined,
      );
    }
    if (response.status === 204 || response.status === 202) return undefined as T;
    return response.json() as Promise<T>;
  }
}
