import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { Parser } from '@asyncapi/parser';
import openapiTS from 'openapi-typescript';
import yaml from 'js-yaml';

const source = fileURLToPath(new URL('../../api/asyncapi.yaml', import.meta.url));
const result = await new Parser().parse(await readFile(source, 'utf8'), { source });
const errors = result.diagnostics.filter((item) => item.severity === 0);
assert.deepEqual(errors, [], 'AsyncAPI errors');
assert.ok(result.document, 'AsyncAPI document');
const spec = result.document.json();
assert.deepEqual(spec.components.schemas.SessionReady.required, ['type', 'session_id', 'last_sequence']);
assert.deepEqual(spec.components.schemas.Resume.required, ['type', 'access_token', 'session_id', 'last_sequence']);
assert.ok(result.document.allMessages().some((message) => message.id() === 'session-ready'));
assert.equal(spec['x-control-session-policy'].close_codes.slow_consumer, 1013, 'slow consumer close code');

const openapiPath = new URL('../../api/openapi.yaml', import.meta.url);
const openapi = yaml.load(await readFile(openapiPath, 'utf8'));
const schemas = openapi.components.schemas;
for (const [name, required] of Object.entries({ Group: ['roles', 'permissions'], User: ['groups', 'roles', 'permissions'], LogEntry: ['occurred_at', 'level', 'component', 'message'] })) {
  for (const field of required) assert.ok(schemas[name].allOf[1].required.includes(field), `${name}.${field} required`);
}
assert.equal(schemas.LogPage.properties.items.items.$ref, '#/components/schemas/LogEntry');
assert.equal(spec.components.schemas.LogStreamEnvelope.allOf[1].properties.event_type.enum[0], 'log.updated');

// 1. Every REST endpoint/method used by the panel must exist in openapi.yaml.
//    `src/api/client.ts` declares descriptors via operation('<path>', '<method>'), and the
//    generated `paths` type makes the same table fail `npm run lint` if a key is renamed.
const clientSource = await readFile(new URL('../src/api/client.ts', import.meta.url), 'utf8');
const operations = [...clientSource.matchAll(/operation\(\s*'([^']+)'\s*,\s*'([a-z]+)'\s*\)/g)].map((match) => ({ path: match[1], method: match[2] }));
assert.ok(operations.length >= 20, `expected at least 20 endpoint descriptors, found ${operations.length}`);
for (const { path, method } of operations) {
  const entry = openapi.paths[path];
  assert.ok(entry, `panel endpoint ${path} is missing from api/openapi.yaml`);
  assert.ok(entry[method], `panel endpoint ${method.toUpperCase()} ${path} is missing from api/openapi.yaml`);
}

// 2. P12 preview pages are contract skeletons. Their descriptors are covered by
//    the loop above; also pin the target contract status so wiring or an
//    annotation change cannot silently make a preview look implemented.
const p12Targets = [
  ['/accounts', 'get', 'not_implemented'],
  ['/installations', 'get', 'not_implemented'],
  ['/installations/{installationId}', 'get', 'not_implemented'],
  ['/instances', 'get', 'not_implemented'],
  ['/instances/{instanceId}', 'get', 'not_implemented'],
  ['/operations/{ownerService}/{operationId}', 'get', 'not_implemented'],
  ['/tasks/{taskId}', 'get', 'not_implemented'],
  ['/models/{modelId}', 'get', 'not_implemented'],
  ['/models/{modelId}/configuration', 'get', 'not_implemented'],
  ['/health/live', 'get', 'implemented'],
  ['/health/ready', 'get', 'implemented'],
  ['/health', 'get', 'reference'],
];
for (const [path, method, expected] of p12Targets) {
  const entry = openapi.paths[path];
  assert.ok(entry, `P12 preview endpoint ${path} is missing from api/openapi.yaml`);
  assert.ok(entry[method], `P12 preview endpoint ${method.toUpperCase()} ${path} is missing from api/openapi.yaml`);
  const status = entry[method]['x-contract-status'];
  if (expected === 'not_implemented') {
    assert.equal(status, 'not_implemented', `P12 preview ${path} must stay not_implemented until wiring`);
  } else {
    assert.notEqual(status, 'not_implemented', `P12 preview ${path} is expected ${expected} but is not implemented`);
  }
}
const declaredEndpoints = new Set([...clientSource.matchAll(/\n  (\w+): operation\(/g)].map((match) => match[1]));
const catalogSource = await readFile(new URL('../src/p12/catalog.ts', import.meta.url), 'utf8');
const referencedEndpoints = [...new Set([...catalogSource.matchAll(/endpoints\.(\w+)/g)].map((match) => match[1]))];
assert.ok(referencedEndpoints.length >= 10, `P12 catalog must reference its target operations (found ${referencedEndpoints.length})`);
for (const key of referencedEndpoints) {
  assert.ok(declaredEndpoints.has(key), `P12 catalog references undeclared endpoint ${key}`);
}
assert.ok((catalogSource.match(/key: 'p12-/g) ?? []).length >= 6, 'P12 catalog pages are missing');
const fixturesSource = await readFile(new URL('../src/p12/fixtures.ts', import.meta.url), 'utf8');
assert.ok((fixturesSource.match(/Schemas\['Target/g) ?? []).length >= 8,
  'P12 fixtures must be typed against target schemas');
const previewSource = await readFile(new URL('../src/views/p12-preview.tsx', import.meta.url), 'utf8');
assert.ok(previewSource.includes('未接线') && previewSource.includes('待 P12'),
  'P12 preview must surface the unwired/待 P12 state');

// 3. The panel's known-event whitelist must match the AsyncAPI event types whose message is
//    not marked x-contract-status: not_implemented (health.updated is not produced in v1 and
//    must not be treated as a live refresh signal).
const streamSource = await readFile(new URL('../src/stream/control-stream.ts', import.meta.url), 'utf8');
const knownMatch = streamSource.match(/KNOWN_EVENTS\s*=\s*\[([^\]]+)\]/);
assert.ok(knownMatch, 'KNOWN_EVENTS declaration is missing from control-stream.ts');
const knownEvents = [...knownMatch[1].matchAll(/'([^']+)'/g)].map((match) => match[1]).sort();
const implementedEvents = new Set();
const collectEventTypes = (node, out) => {
  if (!node || typeof node !== 'object') return;
  if (node.event_type?.enum) for (const value of node.event_type.enum) out.add(value);
  for (const value of Object.values(node)) collectEventTypes(value, out);
};
for (const message of Object.values(spec.components.messages)) {
  // @asyncapi/parser resolves $refs inline; the not_implemented marker stays on the message.
  if (message['x-contract-status'] === 'not_implemented') continue;
  collectEventTypes(message.payload, implementedEvents);
}
const expectedEvents = [...implementedEvents].sort();
assert.deepEqual(knownEvents, expectedEvents, `KNOWN_EVENTS (${knownEvents}) drifts from implemented AsyncAPI events (${expectedEvents})`);

// 4. The panel target scoped stream client owns a frozen copy of wire constants that cannot be
//    imported from YAML at runtime. Keep it aligned with api/asyncapi.yaml (frozen target contract)
//    so a contract change cannot silently drift from the panel's close-code/resync/limit handling.
const targetProtocolSource = await readFile(new URL('../src/stream/target-protocol.ts', import.meta.url), 'utf8');
const targetPolicy = spec['x-control-session-policy'];
const closeCodeBlock = targetProtocolSource.match(/TARGET_CLOSE_CODE\s*=\s*\{([^}]+)\}/);
assert.ok(closeCodeBlock, 'TARGET_CLOSE_CODE declaration is missing from target-protocol.ts');
const panelCloseCodes = Object.fromEntries([...closeCodeBlock[1].matchAll(/(\w+):\s*(\d+)/g)]
  .map((match) => [match[1], Number(match[2])]));
for (const [panelName, contractName] of Object.entries({
  invalidFirstFrame: 'invalid_first_frame', unauthenticated: 'unauthenticated', forbidden: 'forbidden',
  internal: 'internal', slowConsumer: 'slow_consumer', shutdown: 'shutdown',
})) {
  assert.equal(panelCloseCodes[panelName], targetPolicy.close_codes[contractName],
    `target close code ${panelName} drifts from AsyncAPI ${contractName}`);
}
assert.equal(panelCloseCodes.normal, 1000, 'resync/documented normal close is 1000');

const resyncReasons = spec.components.schemas.ResyncRequired.properties.reason.enum;
const panelReasonsBlock = targetProtocolSource.match(/TARGET_RESYNC_REASONS\s*=\s*\[([^\]]+)\]/);
assert.ok(panelReasonsBlock, 'TARGET_RESYNC_REASONS declaration is missing from target-protocol.ts');
const panelReasons = [...panelReasonsBlock[1].matchAll(/'([^']+)'/g)].map((match) => match[1]).sort();
assert.deepEqual(panelReasons, [...resyncReasons].sort(),
  `panel resync reasons (${panelReasons}) drift from AsyncAPI (${resyncReasons})`);

const firstFrame = Number((targetProtocolSource.match(/TARGET_FIRST_FRAME_TIMEOUT_MS\s*=\s*([\d_]+)/) ?? [])[1]?.replace(/_/g, ''));
assert.equal(firstFrame, spec['x-target-contract'].limits.first_frame_timeout_ms,
  'panel first-frame deadline drifts from AsyncAPI limits.first_frame_timeout_ms');

const scopeLimit = Number((targetProtocolSource.match(/TARGET_SCOPE_LIMIT\s*=\s*([\d_]+)/) ?? [])[1]?.replace(/_/g, ''));
const exactScope = spec.components.schemas.TargetExactScope.allOf[1].properties.resources;
assert.equal(scopeLimit, exactScope.maxItems, 'panel scope limit drifts from TargetExactScope.resources.maxItems');
assert.equal(exactScope.minItems, 1, 'target exact scope requires at least one resource');

const safeSequence = Number((targetProtocolSource.match(/TARGET_MAX_SAFE_SEQUENCE\s*=\s*([\d_]+)/) ?? [])[1]?.replace(/_/g, ''));
assert.equal(safeSequence, spec.components.schemas.SessionReady.properties.last_sequence.maximum,
  'panel Control sequence bound drifts from AsyncAPI');

const cursorServices = spec.components.schemas.TargetOwnerCursor.allOf[1].properties.source_service.enum;
const panelCursorServices = [...(targetProtocolSource.match(/TARGET_CURSOR_SERVICES\s*=\s*\[([^\]]+)\]/) ?? [])[1]?.matchAll(/'([^']+)'/g) ?? []]
  .map((match) => match[1]).sort();
assert.deepEqual(panelCursorServices, [...cursorServices].sort(),
  'panel owner cursor services drift from AsyncAPI TargetOwnerCursor');

await openapiTS(openapiPath);
console.log(`OpenAPI generation and AsyncAPI parsing passed; ${operations.length} panel endpoints and ${knownEvents.length} known events match the contracts.`);
