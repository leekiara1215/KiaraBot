import type { components, paths } from '../generated/openapi';

export type Schemas = components['schemas'];
export type TokenPair = Schemas['TokenPair'];
export type Health = Schemas['Health'];
export type Group = Schemas['Group'];
export type User = Schemas['User'];
export type LogEntry = Schemas['LogEntry'];
export type Adapter = Schemas['Adapter'];
export type Plugin = Schemas['Plugin'];
export type GenericResource = Schemas['GenericResource'];
export type AuditEntry = Schemas['AuditEntry'];
export type LoginRequest = paths['/auth/login']['post']['requestBody']['content']['application/json'];
type ErrorBody = Schemas['Error'];
export type Page<T> = { items: T[]; page: Schemas['PageInfo'] };
export type Fetcher = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>;

export class ApiError extends Error {
  constructor(public readonly status: number, public readonly code: string, message: string, public readonly requestId?: string) { super(message); this.name = 'ApiError'; }
}

export class ApiClient {
  private accessToken?: string;
  constructor(private readonly fetcher: Fetcher = fetch, private readonly base = '/api/v1') {}
  setToken(token?: string) { this.accessToken = token; }
  async login(input: LoginRequest): Promise<TokenPair> {
    const tokens = await this.request<TokenPair>('/auth/login', { method: 'POST', body: input, auth: false });
    this.setToken(tokens.access_token);
    return tokens;
  }
  async refresh(refreshToken: string): Promise<TokenPair> {
    return this.request<TokenPair>('/auth/refresh', { method: 'POST', body: { refresh_token: refreshToken }, auth: false });
  }
  health() { return this.request<Health>('/health'); }
  bots(token?: string) { return this.list<GenericResource>('/bots', token); }
  adapters(token?: string) { return this.list<Adapter>('/adapters', token); }
  plugins(token?: string) { return this.list<Plugin>('/plugins', token); }
  models(token?: string) { return this.list<GenericResource>('/models', token); }
  groups(token?: string) { return this.list<Group>('/groups', token); }
  users(token?: string) { return this.list<User>('/users', token); }
  tasks(token?: string) { return this.list<GenericResource>('/tasks', token); }
  logs(token?: string) { return this.list<LogEntry>('/logs', token); }
  audit(token?: string) { return this.list<AuditEntry>('/audit', token); }
  private list<T>(path: string, token?: string): Promise<Page<T>> { return this.request(`${path}${token ? `?page_token=${encodeURIComponent(token)}` : ''}`); }
  private async request<T>(path: string, options: { method?: string; body?: unknown; auth?: boolean } = {}): Promise<T> {
    const headers = new Headers({ Accept: 'application/json' });
    if (options.body !== undefined) {
      headers.set('Content-Type', 'application/json');
      headers.set('Idempotency-Key', crypto.randomUUID());
    }
    if (options.auth !== false && this.accessToken) headers.set('Authorization', `Bearer ${this.accessToken}`);
    const response = await this.fetcher(`${this.base}${path}`, { method: options.method ?? 'GET', headers, body: options.body === undefined ? undefined : JSON.stringify(options.body) });
    if (!response.ok) {
      const error: Partial<ErrorBody> = await response.json().catch(() => ({}));
      throw new ApiError(response.status, error.code ?? 'INTERNAL', error.message ?? `HTTP ${response.status}`, error.request_id);
    }
    return response.json() as Promise<T>;
  }
}
