// Pure WS-01 contract regression; no target producer/transport availability claim.
// Invocation matches check-target-api.sh's pinned Ajv dependency and JSON inputs.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const Ajv = require(path.resolve(process.argv[2], 'ajv'));
const addFormats = require(path.resolve(process.argv[2], 'ajv-formats'));
const api = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
const stream = JSON.parse(fs.readFileSync(process.argv[4], 'utf8'));
const streamUri = 'https://kiarabot.invalid/api/asyncapi.yaml';
const apiUri = 'https://kiarabot.invalid/api/openapi.yaml';
const schema = structuredClone(stream);
// AsyncAPI's document identifier is not the obsolete JSON Schema `id` keyword.
delete schema.id;
schema.$id = streamUri;
const ajv = new Ajv({strict: false, allErrors: true});
addFormats(ajv);
ajv.addSchema(api, apiUri);
ajv.addSchema(schema, streamUri);
let assertions = 0;
function check(name, value, expected) {
  const validate = ajv.compile({$ref: `${streamUri}#/components/schemas/${name}`});
  assert.equal(validate(value), expected, `${name}: ${JSON.stringify(validate.errors)}`);
  ++assertions;
}
const resource = {kind: 'RESOURCE_MODEL', id: '11111111-1111-4111-8111-111111111111'};
const scope = {resources: [resource]};
const hello = {type: 'hello', access_token: 'a'.repeat(32), last_sequence: 0};
check('TargetScopedHello', {...hello, scope}, true);
check('TargetScopedHello', hello, false);
check('TargetScopedHello', {...hello, scope: {resources: []}}, false);
check('TargetScopedHello', {...hello, scope: {resources: [resource, resource]}}, false);
check('TargetScopedHello', {...hello, scope: {resources: Array.from({length: 128}, (_, id) => ({...resource, id: String(id)}))}}, false);
check('TargetScopedHello', {...hello, access_token: 'a'.repeat(4097), scope}, false);
check('TargetScopedHello', {...hello, last_sequence: 1, scope}, false);
// Reference and target message alternatives must be disjoint in channel oneOf.
check('ReferenceHello', hello, true);
check('ReferenceHello', {...hello, scope}, false);
const cursor = {source_service: 'model', epoch: 'dataset-1', scope, sequence: '0'};
const ready = {type: 'SESSION_READY', session_id: 's'.repeat(32), last_sequence: 0, owner_cursor: cursor};
check('OperationSessionReady', ready, true);
const withoutOwner = {...ready}; delete withoutOwner.owner_cursor;
check('OperationSessionReady', withoutOwner, false);
check('OperationSessionReady', {...ready, last_sequence: Number.MAX_SAFE_INTEGER}, true);
check('OperationSessionReady', {...ready, last_sequence: Number.MAX_SAFE_INTEGER + 1}, false);
check('OperationSessionReady', {...ready, owner_cursor: {...cursor, sequence: '18446744073709551615'}}, true);
check('OperationSessionReady', {...ready, owner_cursor: {...cursor, sequence: '18446744073709551616'}}, false);
check('OperationSessionReady', {...ready, owner_cursor: {...cursor, source_service: 'all'}}, false);
check('OperationSessionReady', {...ready, owner_cursor: {...cursor, scope: {resources: [{kind: 'RESOURCE_CATALOG', id: 'control'}]}}}, false);
const resume = {type: 'resume', access_token: hello.access_token, session_id: ready.session_id, last_sequence: 0, scope, owner_cursor: cursor};
check('OperationResume', resume, true);
const missingResumeCursor = {...resume}; delete missingResumeCursor.owner_cursor;
check('OperationResume', missingResumeCursor, false);
check('TargetScopedResume', resume, true);
check('ReferenceResume', resume, false);
const operation = {ref: {owner_service: 'model', operation_id: 'operation-1', resource}, version: '1', state: 'OPERATION_BLOCKED', execution_outcome: 'EXECUTION_UNKNOWN', observed_at: '2026-11-04T00:00:00Z', reason: {code: 'UPSTREAM_UNAVAILABLE', message: 'dependency unavailable', request_id: 'request', trace_id: 'trace'}, next_attempt_at: '2026-11-04T00:00:01Z'};
const envelope = {schema_version: 'kiarabot.stream.v1', event_type: 'operation.updated', sequence: 1, occurred_at: '2026-11-04T00:00:00Z', payload: operation, owner_cursor: cursor};
check('OperationEnvelope', envelope, true);
const invisibleResource = structuredClone(envelope); delete invisibleResource.payload.ref.resource;
check('OperationEnvelope', invisibleResource, false);
check('OperationEnvelope', {...envelope, payload: {...operation, bootstrap_credential: 'secret'}}, false);
for (const reason of ['CURSOR_EXPIRED', 'INVALID_CURSOR', 'SERVER_RESTART']) {
  check('ResyncRequired', {type: 'RESYNC_REQUIRED', reason}, true);
}
check('ResyncRequired', {type: 'RESYNC_REQUIRED', reason: 'provider response body'}, false);
const task = {id: 'task-1', version: '1', target: {owner_instance_id: 'instance-1', state: 'SCHEDULED_TASK_COMPLETED', updated_at: '2026-11-04T00:00:00Z', cancel_requested: false}};
check('TargetTaskPayload', task, true);
check('TargetTaskPayload', {...task, target: {...task.target, state: 'TRIGGERING'}}, false);
check('TargetTaskPayload', {...task, target: {...task.target, payload: 'secret'}}, false);
const adapter = {id: 'adapter-1', version: '1', target: {resource: {kind: 'RESOURCE_ADAPTER', id: 'adapter-1'}, version: '1', online: false, observed_at: '2026-11-04T00:00:00Z', lifecycle: 'LIFECYCLE_ACTIVE'}};
check('TargetAdapterPayload', adapter, true);
check('TargetAdapterPayload', {...adapter, target: {...adapter.target, bootstrap_credential: 'secret'}}, false);
const state = {resource: {kind: 'RESOURCE_INSTANCE', id: 'instance-1'}, version: '1', lifecycle: 'LIFECYCLE_ACTIVE', desired: 'DESIRED_RUNNING', registered: false, desired_config_version: '1', applied_config_version: '0', process_outcome: 'EXECUTION_UNKNOWN', observed_at: '2026-11-04T00:00:00Z', source_service: 'runtime'};
const plugin = {id: 'instance-1', version: '1', target: {instance_id: 'instance-1', installation_id: 'installation-1', externally_managed: true, state}};
check('TargetPluginPayload', plugin, true);
check('TargetPluginPayload', {...plugin, target: {...plugin.target, configuration: {secret: 'secret'}}}, false);
check('TargetPluginPayload', {...plugin, health_state: 'HEALTHY'}, false);
for (const name of ['control', 'health', 'logs', 'adapters', 'plugins', 'tasks', 'messages', 'operations']) {
  assert.equal(stream.channels[name]['x-target-contract-status'], 'not_implemented', `${name} must not claim target availability`);
}
assert.deepEqual(stream['x-target-contract'].operation_permissions, {runtime: 'resource.read', model: 'resource.read', scheduler: 'task.read', host: 'host.read'});
assert.deepEqual(stream['x-target-contract'].delivery_batches.first, ['operation.updated', 'health.updated', 'adapter.updated', 'plugin.updated']);
assert.deepEqual(stream['x-target-contract'].delivery_batches.second, ['task.updated', 'log.updated']);
assert.deepEqual(stream['x-target-contract'].delivery_batches.third, ['message.updated']);
for (const name of ['TargetAdapterUpdate', 'TargetPluginUpdate', 'TargetTaskUpdate', 'TargetControlAdapterUpdate', 'TargetControlPluginUpdate', 'TargetControlTaskUpdate']) {
  assert.equal(stream.components.messages[name]['x-contract-status'], 'not_implemented');
}
const close = stream['x-control-session-policy'].close_codes;
assert.equal(close.invalid_first_frame, 4400);
assert.equal(close.unauthenticated, 4401);
assert.equal(close.forbidden, 4403);
assert.equal(close.internal, 1011);
assert.equal(close.slow_consumer, 1013);
assert.equal(close.shutdown, 1001);
assert.equal(stream['x-control-session-policy'].retention_seconds, 600);
assert.deepEqual(stream['x-target-contract'].limits, {first_frame_timeout_ms: 5000, max_frame_bytes: 65536, max_pending_frames: 128, max_pending_bytes: 1048576, replay_batch_size: 32, max_replay_bytes_per_session: 4194304, max_retained_bytes: 67108864, frame_type: 'text'});
console.log(`WS expansion schema regression passed: ${assertions} boundary cases, owner permissions, rollout and close/limit policy`);
