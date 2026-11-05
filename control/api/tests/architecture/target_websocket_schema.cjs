// WS-04 schema evidence: validate the frames actually produced by
// kiarabot_control_target_websocket_test against the real api/asyncapi.yaml and
// api/openapi.yaml documents with the repository-pinned Ajv. Hand-written examples cannot drift
// from the C++ projection this way.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {execFileSync} = require('node:child_process');

const repo = process.argv[2];
const binary = process.argv[3];
const output = process.argv[4];
if (!repo || !binary || !output) {
  console.error('usage: target_websocket_schema.cjs <repo> <binary> <output-dir>');
  process.exit(2);
}
const modules = process.env.KIARABOT_API_NODE_MODULES || path.join(repo, 'scripts/ci/node_modules');
for (const dependency of ['ajv', 'ajv-formats', 'js-yaml']) {
  assert.ok(fs.existsSync(path.join(modules, dependency, 'package.json')),
            'run npm ci --prefix scripts/ci first: ' + dependency);
}
const Ajv = require(path.join(modules, 'ajv'));
const addFormats = require(path.join(modules, 'ajv-formats'));
const yaml = require(path.join(modules, 'js-yaml'));

const streamUri = 'https://kiarabot.invalid/api/asyncapi.yaml';
const apiUri = 'https://kiarabot.invalid/api/openapi.yaml';

function load(relativePath) {
  return yaml.load(fs.readFileSync(path.join(repo, relativePath), 'utf8'));
}
// AsyncAPI 2.x and OpenAPI 3.0 are not JSON Schema documents; re-root the frozen schemas the same
// way proto/tests/ws_expansion_schema_test.cjs does and rewrite the cross-document reference.
function normalize(document, uri) {
  const text = JSON.stringify(document).split('./openapi.yaml#').join(apiUri + '#');
  const schema = JSON.parse(text);
  delete schema.id;
  schema.$id = uri;
  return schema;
}

const api = normalize(load('api/openapi.yaml'), apiUri);
const stream = normalize(load('api/asyncapi.yaml'), streamUri);
const ajv = new Ajv({strict: false, allErrors: true, allowUnionTypes: true});
addFormats(ajv);
ajv.addSchema(api, apiUri);
ajv.addSchema(stream, streamUri);

let assertions = 0;
function checkWith(uri, name, value, expected) {
  const validate = ajv.getSchema(uri + '#/components/schemas/' + name);
  assert.ok(validate, 'missing schema ' + name);
  assert.equal(validate(value), expected,
               name + ': ' + JSON.stringify(validate.errors) + ' for ' +
               JSON.stringify(value));
  ++assertions;
}
function check(name, value, expected) {
  checkWith(streamUri, name, value, expected);
}
function checkApi(name, value, expected) {
  checkWith(apiUri, name, value, expected);
}
function read(name) {
  const file = path.join(output, name);
  assert.ok(fs.existsSync(file), 'missing emitted fixture ' + file);
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}
function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

fs.mkdirSync(output, {recursive: true});
execFileSync(binary, ['--emit', output], {stdio: 'inherit'});

// Positive: what the C++ projection really emits must satisfy the frozen stream schemas.
for (const name of ['event_running.json', 'event_blocked.json', 'event_participants.json']) {
  const envelope = read(name);
  check('OperationEnvelope', envelope, true);
  check('StreamEnvelope', envelope, true);
  check('TargetOwnerCursor', envelope.owner_cursor, true);
  checkApi('TargetOperation', envelope.payload, true);
  if (envelope.payload.reason) {
    checkApi('TargetError', envelope.payload.reason, true);
  }
  for (const participant of envelope.payload.participants) {
    checkApi('TargetOperationProgress', participant, true);
    if (participant.reason) {
      checkApi('TargetError', participant.reason, true);
    }
  }
}
// v1.2 producer envelopes: every emitted stream event must satisfy its frozen channel schema and
// the shared StreamEnvelope. The adapter example is the real Runtime AdapterFence shape (no
// execution generation by default); its optional positive form and the plugin strict form are both
// exercised below.
for (const [name, schema] of [
  ['health_healthy.json', 'HealthEnvelope'],
  ['event_adapter.json', 'TargetAdapterEnvelope'],
  ['event_log.json', 'TargetLogEnvelope'],
  ['event_task.json', 'TargetTaskEnvelope'],
  ['event_message.json', 'TargetMessageEnvelope'],
  ['event_plugin.json', 'TargetPluginEnvelope'],
]) {
  const envelope = read(name);
  check(schema, envelope, true);
  check('StreamEnvelope', envelope, true);
}
const adapter = read('event_adapter.json');
assert.equal(adapter.payload.target.online, true);
checkApi('TargetAdapterFence', adapter.payload.target.current_fence, true);
assert.equal(adapter.payload.target.current_fence.execution_generation, undefined);
// Optional positive adapter execution_generation: present-and-positive is valid, zero is rejected;
// the plugin fence stays strict (execution_generation required).
const generated = clone(adapter);
generated.payload.target.current_fence.execution_generation = '1';
check('TargetAdapterEnvelope', generated, true);
checkApi('TargetAdapterFence', generated.payload.target.current_fence, true);
const zeroGeneration = clone(adapter);
zeroGeneration.payload.target.current_fence.execution_generation = '0';
checkApi('TargetAdapterFence', zeroGeneration.payload.target.current_fence, false);
const missingConnection = clone(adapter);
delete missingConnection.payload.target.current_fence.connection_generation;
check('TargetAdapterEnvelope', missingConnection, false);
// Closed v1.2 log metadata: a free-text level or an extra free-text field is rejected.
const badLog = clone(read('event_log.json'));
badLog.payload.level = 'VERBOSE';
check('TargetLogEnvelope', badLog, false);
const extraLog = clone(read('event_log.json'));
extraLog.payload.message = 'free text';
check('TargetLogEnvelope', extraLog, false);
// Target task version is the canonical positive uint64 string.
const badTask = clone(read('event_task.json'));
badTask.payload.version = 2;
check('TargetTaskEnvelope', badTask, false);
// Message metadata never infers sent direction nor carries content-like extras.
const badMessage = clone(read('event_message.json'));
badMessage.payload.direction = 'sent';
check('TargetMessageEnvelope', badMessage, false);
const extraMessage = clone(read('event_message.json'));
extraMessage.payload.body = 'secret';
check('TargetMessageEnvelope', extraMessage, false);
// Plugin state follows the frozen wire-presence schema: resource/version/lifecycle/registered/
// applied_config_version/process_outcome are required, while desired/desired_config_version/
// observed_at/source_service are optional and omitted when the owner had no value.
const plugin = read('event_plugin.json');
const pluginState = plugin.payload.target.state;
assert.equal(pluginState.desired, undefined);
assert.equal(pluginState.desired_config_version, undefined);
assert.equal(pluginState.source_service, undefined);
assert.equal(pluginState.registered, true);
checkApi('TargetVersionedResourceState', pluginState, true);
// Optional observed_at may also be absent from the state (the StreamEnvelope still carries its own
// authoritative occurred_at); the frozen schema must accept that presence form.
const pluginWithoutObserved = clone(plugin);
delete pluginWithoutObserved.payload.target.state.observed_at;
check('TargetPluginEnvelope', pluginWithoutObserved, true);
// Registered instances still require the complete strict plugin fence.
const pluginWithoutFence = clone(plugin);
delete pluginWithoutFence.payload.target.state.fence;
check('TargetPluginEnvelope', pluginWithoutFence, false);
// Required frozen state facts are never synthesized.
const pluginMissingLifecycle = clone(plugin);
delete pluginMissingLifecycle.payload.target.state.lifecycle;
check('TargetPluginEnvelope', pluginMissingLifecycle, false);
const pluginMissingApplied = clone(plugin);
delete pluginMissingApplied.payload.target.state.applied_config_version;
check('TargetPluginEnvelope', pluginMissingApplied, false);
const pluginBadOutcome = clone(plugin);
pluginBadOutcome.payload.target.state.process_outcome = 'EXECUTION_BROKEN';
check('TargetPluginEnvelope', pluginBadOutcome, false);
// Health payload keeps the closed state/component shape.
const badHealth = clone(read('health_healthy.json'));
badHealth.payload.state = 'BROKEN';
check('HealthEnvelope', badHealth, false);

const ready = read('session_ready.json');
check('OperationSessionReady', ready, true);
check('SessionReady', ready, true);
check('TargetOwnerCursor', ready.owner_cursor, true);
check('ResyncRequired', read('resync_cursor_expired.json'), true);
check('ResyncRequired', read('resync_invalid_cursor.json'), true);
check('ResyncRequired', read('resync_server_restart.json'), true);

// Negative: the schema must actually constrain the projection, otherwise the positives above
// would be vacuous. Each mutation is a defect class the transport must reject before delivery.
const running = read('event_running.json');
const mutations = [
  (envelope) => { delete envelope.owner_cursor; },
  (envelope) => { envelope.sequence = 9007199254740992; },
  (envelope) => { envelope.owner_cursor.sequence = '007'; },
  (envelope) => { envelope.owner_cursor.scope.resources = []; },
  (envelope) => { envelope.payload.ref.operation_id = ''; },
  (envelope) => { envelope.payload.version = '0'; },
  (envelope) => { envelope.payload.observed_at = 'not-an-rfc3339-time'; },
  (envelope) => { envelope.payload.extra = true; },
  (envelope) => { envelope.event_type = 'health.updated'; },
];
for (const mutate of mutations) {
  const candidate = clone(running);
  mutate(candidate);
  check('OperationEnvelope', candidate, false);
}
const blocked = read('event_blocked.json');
const withoutReason = clone(blocked);
delete withoutReason.payload.reason;
check('OperationEnvelope', withoutReason, false);
const participant = read('event_participants.json');
const partialParticipantReason = clone(participant);
partialParticipantReason.payload.participants[1].reason = {code: 'UPSTREAM_UNAVAILABLE'};
check('OperationEnvelope', partialParticipantReason, false);
const malformedResync = clone(read('resync_invalid_cursor.json'));
malformedResync.reason = 'free-form text';
check('ResyncRequired', malformedResync, false);

console.log('target websocket schema: ' + assertions + ' assertions');
