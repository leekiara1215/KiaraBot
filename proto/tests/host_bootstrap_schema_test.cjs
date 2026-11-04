// Contract boundary fixtures only; no Host process or secret store is exercised here.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const Ajv = require(path.resolve(process.argv[2], 'ajv'));
const addFormats = require(path.resolve(process.argv[2], 'ajv-formats'));
const api = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
const ajv = new Ajv({strict: false, allErrors: true});
addFormats(ajv);
ajv.addSchema(api, 'api');
let checks = 0;
function check(name, value, expected) {
  const validate = ajv.compile({$ref: `api#/components/schemas/${name}`});
  assert.equal(validate(value), expected, `${name}: ${JSON.stringify(validate.errors)}`);
  checks++;
}
const scope = {resources: [{kind: 'RESOURCE_HOST', id: 'host'}]};
check('TargetHostScope', scope, true);
for (const bad of [{resources: []}, {resources: [...scope.resources, ...scope.resources]},
                   {resources: [{kind: 'RESOURCE_INSTANCE', id: 'host'}]},
                   {resources: [{kind: 'RESOURCE_CATALOG', id: 'all'}]}]) check('TargetHostScope', bad, false);
check('TargetHostBeginMaintenanceRequest', {expected_version: '1'}, true);
check('TargetHostBeginMaintenanceRequest', {expected_version: '1', grace_ms: 10000}, true);
for (const bad of [{}, {expected_version: '0'}, {expected_version: '1', grace_ms: 10001},
                   {expected_version: '1', shell: 'forbidden'}]) check('TargetHostBeginMaintenanceRequest', bad, false);
check('TargetHostCancelOperationRequest', {expected_version: '1', scope}, true);
check('TargetHostCancelOperationRequest', {expected_version: '1', scope: {resources: []}}, false);
const delivery = {
  delivery_id: 'delivery', resource: {kind: 'RESOURCE_INSTANCE', id: 'instance'},
  recipient_id: 'fixed-deployment', credential_version: '1', state: 'BOOTSTRAP_DELIVERY_UNKNOWN',
  operation: {owner_service: 'runtime', operation_id: 'operation'}, observed_at: '2026-11-04T00:00:00Z'
};
check('TargetBootstrapDelivery', delivery, true);
for (const key of api.components.schemas.TargetBootstrapDelivery.required) {
  const copy = structuredClone(delivery); delete copy[key]; check('TargetBootstrapDelivery', copy, false);
}
for (const key of ['bootstrap_credential', 'digest', 'path', 'environment', 'secret']) {
  check('TargetBootstrapDelivery', {...delivery, [key]: 'forbidden'}, false);
}
check('TargetBootstrapDelivery', {...delivery, credential_version: '0'}, false);
check('TargetBootstrapDelivery', {...delivery, credential_version: '18446744073709551616'}, false);
check('TargetReissueBootstrapRequest', {recipient_id: 'fixed', expected_credential_version: '1'}, true);
for (const bad of [{recipient_id: 'fixed'}, {recipient_id: '', expected_credential_version: '1'},
                   {recipient_id: 'fixed', expected_credential_version: '0'},
                   {recipient_id: 'fixed', expected_credential_version: '1', path: '/tmp/forbidden'}]) {
  check('TargetReissueBootstrapRequest', bad, false);
}
const operation = {ref: {owner_service: 'runtime', operation_id: 'operation'}, version: '1',
  state: 'OPERATION_ACCEPTED', execution_outcome: 'EXECUTION_NOT_STARTED', observed_at: delivery.observed_at};
check('TargetReissueBootstrapResponse', {delivery, operation}, true);
check('TargetReissueBootstrapResponse', {delivery, operation, bootstrap_credential: 'forbidden'}, false);
// Existing requests remain valid without opting in; new target delivery is independently tracked.
const legacyCreate = {installation_id: 'installation', host_id: 'host'};
check('TargetRuntimeCreateInstanceRequest', legacyCreate, true);
check('TargetRuntimeCreateInstanceRequest', {...legacyCreate, bootstrap_recipient_id: 'fixed'}, true);
check('TargetRuntimeCreateInstanceRequest', {...legacyCreate, bootstrap_recipient_id: ''}, false);
const get = api.paths['/operations/{ownerService}/{operationId}'].get;
assert.equal(Object.hasOwn(get, 'x-permission'), false);
assert.deepEqual(get['x-owner-permissions'], {runtime: 'resource.read', scheduler: 'task.read', model: 'resource.read', host: 'host.read'});
assert.equal(get['x-owner-rpc'].host, 'HostManagementService.GetOperation');
assert.equal(api.paths['/hosts/{hostId}/maintenance'].post['x-permission'], 'host.execute');
assert.equal(api.paths['/accounts/{accountId}/bootstrap/reissue'].post['x-permission'], 'account.manage');
assert.equal(api.paths['/instances/{instanceId}/bootstrap/reissue'].post['x-permission'], 'plugin.manage');
console.log(`${checks} Host/bootstrap schema boundaries and exact owner permissions passed`);
