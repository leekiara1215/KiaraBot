import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { Parser } from '@asyncapi/parser';
import openapiTS from 'openapi-typescript';
import yaml from 'js-yaml';
const source = resolve('../api/asyncapi.yaml');
const result = await new Parser().parse(await readFile(source, 'utf8'), { source });
const errors = result.diagnostics.filter((item) => item.severity === 0);
assert.deepEqual(errors, [], 'AsyncAPI errors');
assert.ok(result.document, 'AsyncAPI document');
const spec = result.document.json();
assert.deepEqual(spec.components.schemas.SessionReady.required, ['type', 'session_id', 'last_sequence']);
assert.deepEqual(spec.components.schemas.Resume.required, ['type', 'access_token', 'session_id', 'last_sequence']);
assert.ok(result.document.allMessages().some((message) => message.id() === 'session-ready'));
const openapiPath = new URL('../../api/openapi.yaml', import.meta.url);
const openapi = yaml.load(await readFile(openapiPath, 'utf8'));
const schemas = openapi.components.schemas;
for (const [name, required] of Object.entries({ Group: ['roles', 'permissions'], User: ['groups', 'roles', 'permissions'], LogEntry: ['occurred_at', 'level', 'component', 'message'] })) {
  for (const field of required) assert.ok(schemas[name].allOf[1].required.includes(field), `${name}.${field} required`);
}
assert.equal(schemas.LogPage.properties.items.items.$ref, '#/components/schemas/LogEntry');
assert.equal(spec.components.schemas.LogStreamEnvelope.allOf[1].properties.event_type.enum[0], 'log.updated');
await openapiTS(openapiPath);
console.log('OpenAPI generation and AsyncAPI parsing passed; session and resume fields present.');
