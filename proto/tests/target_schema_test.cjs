// Run with the Ajv runtime bundled with the pinned lint install; no server behavior claim.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const Ajv = require(path.resolve(process.argv[2], 'ajv'));
const addFormats = require(path.resolve(process.argv[2], 'ajv-formats'));
const api = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
const ajv = new Ajv({strict: false, allErrors: true});
addFormats(ajv);
ajv.addSchema(api, 'api');
function check(schema, value, expected) {
  const validate = ajv.compile({$ref: `api#/components/schemas/${schema}`});
  assert.equal(validate(value), expected, `${schema}: ${JSON.stringify(value)} ${JSON.stringify(validate.errors)}`);
}
for (const name of ['TargetPositiveUint64', 'TargetUint64']) {
  check(name, '18446744073709551615', true);
  for (const bad of ['18446744073709551616', '99999999999999999999', '184467440737095516150', '-1', '01', '1.0', '']) check(name, bad, false);
  check(name, '0', name === 'TargetUint64');
}
const resource = {kind: 'RESOURCE_INSTANCE', id: 'instance'};
const fence = {instance_id: 'instance', execution_generation: '1', connection_generation: '1', runtime_epoch: 'epoch'};
const ref = {owner_service: 'runtime', operation_id: 'operation', resource};
const account = {account_id: 'account', platform: 'qq', platform_account_id: '123', version: '1', state: 'LIFECYCLE_ACTIVE'};
const operation = {ref, version: '1', state: 'OPERATION_ACCEPTED', execution_outcome: 'EXECUTION_NOT_STARTED', observed_at: '2026-11-01T00:00:00Z'};
for (const [name, value] of Object.entries({TargetResourceRef: resource, TargetFence: fence, TargetOperationRef: ref, TargetAccountRecord: account, TargetOperation: operation, TargetRuntimeCreateAccountResponse: {account}})) {
  check(name, value, true);
  check(name, {}, false);
  const required = api.components.schemas[name].required;
  for (const key of required) {
    const copy = structuredClone(value); delete copy[key]; check(name, copy, false);
  }
}
check('TargetOperation', {...operation, state: 'OPERATION_BLOCKED'}, false);
check('TargetOperation', {...operation, state: 'OPERATION_BLOCKED', reason: {code: 'UPSTREAM_UNAVAILABLE', message: 'dependency unavailable', request_id: 'request', trace_id: 'trace'}, next_attempt_at: '2026-11-01T00:00:01Z'}, true);
check('TargetResourceRef', {kind: 'RESOURCE_INSTANCE', id: ''}, false);
check('TargetFence', {...fence, connection_generation: '0'}, false);
check('TargetFence', {...fence, execution_generation: '18446744073709551616'}, false);
check('TargetAccountRecord', {...account, version: '0'}, false);
check('TargetRuntimeCreateAccountResponse', {account: {}}, false);
const state = {resource, version: '1', lifecycle: 'LIFECYCLE_ACTIVE', desired: 'DESIRED_RUNNING', registered: false, desired_config_version: '1', applied_config_version: '0', process_outcome: 'EXECUTION_NOT_STARTED', observed_at: '2026-11-01T00:00:00Z', source_service: 'runtime'};
check('TargetVersionedResourceState', state, true);
check('TargetVersionedResourceState', {...state, registered: true}, false);
check('TargetVersionedResourceState', {...state, registered: true, fence}, true);
// Managed instances bind to a Host; external instances register directly with Runtime.
for (const [name, base] of [
  ['TargetRuntimeCreateInstanceRequest', {installation_id: 'installation'}],
  ['TargetInstanceRecord', {instance_id: 'instance', installation_id: 'installation', state}]
]) {
  check(name, {...base, externally_managed: false, host_id: 'host'}, true);
  check(name, {...base, externally_managed: false}, false);
  check(name, {...base, externally_managed: false, host_id: ''}, false);
  check(name, {...base, externally_managed: true}, true);
  check(name, {...base, externally_managed: true, host_id: ''}, false);
  check(name, base, false);
}
check('TargetRuntimeCreateInstanceRequest', {installation_id: 'installation', host_id: 'host'}, true);
check('TargetInstanceRecord', {instance_id: 'instance', installation_id: 'installation', state, host_id: 'host'}, false);
const modelConfiguration = {model_id: 'model', alias: 'local', provider_kind: 'http', endpoint: 'https://example.invalid/chat/completions', provider_model: 'text', timeout_ms: 30000, version: '1'};
check('TargetModelGetModelConfigurationResponse', {config: modelConfiguration, secret_configured: false, observed_at: '2026-11-01T00:00:00Z'}, true);
check('TargetModelGetModelConfigurationResponse', {config: modelConfiguration}, false);
check('TargetModelGetModelConfigurationResponse', {config: {...modelConfiguration, secret: 'forbidden'}, secret_configured: true, observed_at: '2026-11-01T00:00:00Z'}, false);
check('TargetModelConfiguration', {...modelConfiguration, endpoint: 'https://secret@example.invalid/chat'}, false);
check('TargetModelConfiguration', {...modelConfiguration, endpoint: 'https://example.invalid/chat?api_key=secret'}, false);
check('TargetOwnerOperationResponse', {operation}, true);
check('TargetOwnerOperationResponse', {}, false);
const catalog = {kind: 'RESOURCE_CATALOG', id: '11111111-1111-4111-8111-111111111111'};
check('TargetCatalogRef', catalog, true);
check('TargetCatalogRef', {}, false);
check('TargetCatalogRef', {...catalog, kind: 'all'}, false);
check('TargetCatalogRef', {...catalog, id: 'all'}, false);
check('TargetCatalogRef', {...catalog, kind: 'RESOURCE_ACCOUNT'}, false);
const adapter = {resource: {kind: 'RESOURCE_ADAPTER', id: 'adapter'}, version: '1', online: false, observed_at: '2026-11-01T00:00:00Z', lifecycle: 'LIFECYCLE_ACTIVE'};
check('TargetAdapterRecord', adapter, true);
check('TargetAdapterRecord', {...adapter, resource: {kind: 'RESOURCE_ACCOUNT', id: 'account'}}, false);
check('TargetAdapterRecord', {...adapter, online: true}, false);
check('TargetAdapterRecord', {...adapter, online: true, current_fence: fence}, true);
check('TargetAdapterRecord', {...adapter, current_fence: fence}, false);
const configuration = {instance: resource, desired_version: '1', values: {}, applied_version: '0', apply_state: 'CONFIG_APPLY_PENDING', observed_at: '2026-11-01T00:00:00Z'};
check('TargetInstanceConfiguration', configuration, true);
check('TargetInstanceConfiguration', {...configuration, apply_state: 'CONFIG_APPLY_REJECTED'}, false);
check('TargetInstanceConfiguration', {...configuration, apply_state: 'CONFIG_APPLY_REJECTED', error: {code: 'INVALID_ARGUMENT', message: 'invalid config', request_id: 'request', trace_id: 'trace'}}, true);
check('TargetRuntimeGetInstanceConfigResponse', {}, false);
const snapshot = {snapshot_id: 'snapshot', observed_at: '2026-11-01T00:00:00Z', expires_at: '2026-11-01T00:10:00Z', authorization_version: '1', owner_service: 'runtime', page: {page_size: 20}};
check('TargetRuntimeListAccountsResponse', {accounts: [], snapshot}, true);
check('TargetRuntimeListAccountsResponse', {accounts: []}, false);
const binding = {assignment_id: 'grant', principal_id: 'person', version: '1', scope: {resources: [resource]}, role: 'ROLE_VIEWER'};
check('TargetAssignmentRecord', binding, true);
check('TargetAssignmentRecord', {...binding, permissions: ['PERMISSION_MESSAGE_READ']}, false);
check('TargetAssignmentRecord', {...binding, role: 'FIXED_ROLE_UNSPECIFIED', permissions: ['PERMISSION_MESSAGE_READ']}, true);
check('TargetAssignmentRecord', {...binding, role: 'FIXED_ROLE_UNSPECIFIED', permissions: []}, false);
check('TargetAssignmentRecord', {...binding, scope: {resources: []}}, false);
const principalView = {user_id: 'person', session_id: 'session', roles: ['viewer'], permissions: ['resource.read'], assignments: [binding], authorization_version: '1', checked_at: '2026-11-01T00:10:00Z'};
check('TargetCurrentPrincipal', principalView, true);
for (const field of ['assignments', 'authorization_version', 'checked_at']) { const missing = {...principalView}; delete missing[field]; check('TargetCurrentPrincipal', missing, false); }
const sessionView = {session_id: 'session', user_id: 'person', created_at: '2026-11-01T00:00:00Z', expires_at: '2026-11-01T00:15:00Z', state: 'ACTIVE'};
check('TargetCurrentSessionPage', {items: [sessionView], page: {page_size: 20}, checked_at: '2026-11-01T00:10:00Z'}, true);
check('TargetCurrentSessionPage', {items: [sessionView], page: {page_size: 20}}, false);
const credential = {credential_id: 'credential', principal_id: 'person', binding: resource, version: '1', state: 'CREDENTIAL_PENDING', created_at: '2026-11-01T00:00:00Z', checked_at: '2026-11-01T00:00:00Z'};
check('TargetCredentialStatus', credential, true);
check('TargetCredentialStatus', {...credential, new_secret: 'forbidden'}, false);
check('TargetCredentialStatus', {...credential, state: 'CREDENTIAL_ACTIVE'}, false);
check('TargetCredentialStatus', {...credential, expires_at: '2026-11-02T00:00:00Z'}, false);
const grant = {principal_id: 'person', role: 'ROLE_VIEWER', scope: {resources: [resource]}};
check('TargetIdentitySetAssignmentRequest', {assignment: grant}, true);
check('TargetIdentitySetAssignmentRequest', {assignment: grant, expected_version: '1'}, false);
check('TargetIdentitySetAssignmentRequest', {assignment: grant, assignment_id: '11111111-1111-4111-8111-111111111111', expected_version: '1'}, true);
console.log('Target schemas reject missing identity, empty result, zero generation and uint64 overflow; valid fixtures pass');
