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

// 2. The panel's known-event whitelist must match the AsyncAPI event types whose message is
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

await openapiTS(openapiPath);
console.log(`OpenAPI generation and AsyncAPI parsing passed; ${operations.length} panel endpoints and ${knownEvents.length} known events match the contracts.`);
