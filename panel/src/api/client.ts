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
} as const;

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
}

export class ApiClient {
  private accessToken?: string;
  private unauthorized?: () => void;
  setUnauthorizedHandler(handler?: () => void) { this.unauthorized = handler; }
  principal() { return this.call<Schemas['Principal']>(endpoints.principal); }
  sessions(token?: string, pageSize?: number) { return this.list<Session>(endpoints.sessions, { token, pageSize }); }
  revokeSession(sessionId: string) { return this.call<void>(endpoints.revokeSession, { param: sessionId }); }
  constructor(private readonly fetcher: Fetcher = fetch, private readonly base = '/api/v1') {}
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
  private url(endpoint: Endpoint, param?: string): string {
    if (!param) return endpoint.path;
    if (!endpoint.path.includes('{')) throw new Error(`endpoint ${endpoint.path} takes no path parameter`);
    return endpoint.path.replace(/\{[^}]+\}/, encodeURIComponent(param));
  }

  private async call<T>(endpoint: Endpoint, options: RequestOptions & { param?: string } = {}): Promise<T> {
    const query = Object.entries(options.query ?? {})
      .filter(([, value]) => value !== undefined && value !== '')
      .map(([key, value]) => `${encodeURIComponent(key)}=${encodeURIComponent(String(value))}`)
      .join('&');
    const path = this.url(endpoint, options.param) + (query ? `?${query}` : '');
    return this.request<T>(path, endpoint.method, options);
  }

  private async request<T>(path: string, method: string, options: RequestOptions): Promise<T> {
    const requestToken = this.accessToken;
    const headers = new Headers({ Accept: 'application/json' });
    if (options.body !== undefined) headers.set('Content-Type', 'application/json');
    if (method !== 'GET') headers.set('Idempotency-Key', crypto.randomUUID());
    for (const [name, value] of Object.entries(options.headers ?? {})) headers.set(name, value);
    if (options.auth !== false && this.accessToken) headers.set('Authorization', `Bearer ${this.accessToken}`);
    const response = await this.fetcher(`${this.base}${path}`, {
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
