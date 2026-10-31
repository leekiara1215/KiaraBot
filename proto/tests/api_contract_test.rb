#!/usr/bin/env ruby
# Contract-shape checks complement the OpenAPI and AsyncAPI validators.
require 'yaml'
require 'open3'

root = File.expand_path('../..', __dir__)
openapi = YAML.safe_load_file(File.join(root, 'api/openapi.yaml'))
asyncapi = YAML.safe_load_file(File.join(root, 'api/asyncapi.yaml'))

def check(condition, message)
  raise message unless condition
end

schemas = asyncapi.fetch('components').fetch('schemas')
messages = asyncapi.fetch('components').fetch('messages')
ids = messages.values.map { |message| message.fetch('messageId') }
check(ids.uniq.size == ids.size, 'messageIds must be unique')

channels = {
  'health' => ['Health', 'health.updated', %w[state components]],
  'logs' => ['Log', 'log.updated', %w[id occurred_at level component operation outcome]],
  'adapters' => ['Adapter', 'adapter.updated', %w[id state implementation]],
  'plugins' => ['Plugin', 'plugin.updated', %w[id version]],
  'tasks' => ['Task', 'task.updated', %w[id name state]],
  'messages' => ['Message', 'message.updated', %w[message_id conversation_id direction]]
}

channels.each do |channel, (name, event_type, required)|
  definition = asyncapi.fetch('channels').fetch(channel)
  ['publish', 'subscribe'].each do |direction|
    check(definition.fetch(direction).fetch('message').fetch('oneOf').size >= 2,
          "#{channel} lacks handshake or session messages")
  end
  %w[Update ChannelUpdate].each do |suffix|
    ref = messages.fetch("#{name}#{suffix}").fetch('payload').fetch('$ref')
    expected = channel == 'logs' ? 'LogStreamEnvelope' : "#{name}Envelope"
    check(ref.end_with?("/#{expected}"), "#{channel} has an incorrect envelope")
  end
  if channel == 'logs'
    envelope = schemas.fetch('LogStreamEnvelope').fetch('allOf').last.fetch('properties')
    check(envelope.fetch('event_type').fetch('enum') == ['log.updated'], 'logs event type')
    check(envelope.fetch('payload').fetch('$ref') == './openapi.yaml#/components/schemas/LogEntry',
          'logs must use the canonical REST LogEntry')
    entry = openapi.fetch('components').fetch('schemas').fetch('LogEntry').fetch('allOf')
    check(entry.first.fetch('$ref').end_with?('/GenericResource'), 'log resource identity')
    check(entry.last.fetch('required') == %w[occurred_at level component message], 'log fields')
    next
  end
  envelope = schemas.fetch("#{name}Envelope").fetch('allOf').last.fetch('properties')
  check(envelope.fetch('event_type').fetch('enum') == [event_type], "#{channel} event type")
  check(envelope.fetch('payload').fetch('$ref').end_with?("/#{name}Payload"),
        "#{channel} payload reference")
  check(schemas.fetch("#{name}Payload").fetch('required') == required,
        "#{channel} required payload fields")
end
check(schemas.fetch('PluginPayload').fetch('anyOf').map { |entry| entry.fetch('required') } ==
      [%w[state], %w[health_state]], 'plugin payload requires a state dimension')
check(schemas.fetch('MessagePayload').fetch('additionalProperties') == false,
      'message payload must exclude unlisted content')
check(messages.fetch('SessionReady').fetch('payload').fetch('$ref').end_with?('/SessionReady'),
      'control session response missing')

operations = openapi.fetch('paths')
policy = openapi.fetch('x-control-policy')
['/actions/{actionId}', '/actions/by-idempotency-key'].each do |path|
  operation = operations.fetch(path).fetch('get')
  responses = operation.fetch('responses')
  %w[200 400 401 403 404 429 500 503 504].each do |status|
    check(responses.key?(status), "#{path} missing #{status}")
  end
  check(responses.fetch('200').fetch('$ref').end_with?('/ActionResult'),
        "#{path} result schema")
end
lookup = operations.fetch('/actions/by-idempotency-key').fetch('get')
check(lookup.fetch('parameters').first.fetch('$ref').end_with?('/ActionLookupKey'),
      'idempotency lookup header missing')
action = openapi.fetch('components').fetch('schemas').fetch('ActionResult')
variants = action.fetch('oneOf')
accepted, failed, terminal = variants
check(accepted.fetch('properties').fetch('status').fetch('enum') == ['ACCEPTED'],
      'accepted status variant')
check(failed.fetch('required').include?('error') && failed.fetch('required').include?('completed_at'),
      'failed result requires error and completion time')
check(terminal.fetch('required').include?('completed_at') &&
      terminal.fetch('properties').fetch('status').fetch('enum') == %w[SUCCEEDED EXPIRED CANCELLED],
      'terminal result requirements')
check(action.fetch('description').include?('604800') &&
      action.fetch('description').include?('EXPIRED') &&
      action.fetch('description').include?('TIMEOUT'),
      'ActionResult must document 7 day retention and the EXPIRED/TIMEOUT transition')
['/actions/{actionId}', '/actions/by-idempotency-key'].each do |path|
  operation = operations.fetch(path).fetch('get')
  check(operation.fetch('x-contract-status') == 'not_implemented',
        "#{path} must be marked not_implemented until the REST query is wired")
end
check(operations.fetch('/groups').fetch('get').fetch('x-contract-status') == 'not_implemented',
      '/groups must be marked not_implemented')

# Contract status registry and API extension convergence.
openapi_status = openapi.fetch('x-contract-status')
check(openapi_status.fetch('status_registry') == 'proto/tests/contract_status.json',
      'OpenAPI status registry pointer')
check(openapi_status.fetch('not_implemented').sort ==
      %w[getActionByIdempotencyKey getActionResult listGroups],
      'OpenAPI not_implemented operation list')
check(openapi_status.fetch('deprecated') == [], 'OpenAPI deprecated list must be empty')
asyncapi_status = asyncapi.fetch('x-contract-status')
check(asyncapi_status.fetch('not_implemented').sort ==
      %w[health.updated plugin.updated.health_state].sort,
      'AsyncAPI not_implemented surface list')
check(asyncapi_status.fetch('deprecated') == [], 'AsyncAPI deprecated list must be empty')
check(!asyncapi.fetch('channels').fetch('messages').key?('x-contract-status'),
      'messages channel must be implemented')
check(!messages.fetch('MessageUpdate').key?('x-contract-status'),
      'message.updated must be implemented')
check(messages.fetch('HealthUpdate').fetch('x-contract-status') == 'not_implemented',
      'health.updated must be marked not_implemented')
session_policy = asyncapi.fetch('x-control-session-policy')
check(session_policy.fetch('close_codes').fetch('slow_consumer') == 1013,
      'slow consumer close code')
check(session_policy.fetch('messages_default').include?('No default role grants message.read'),
      'messages channel must not imply a default grant')
check(session_policy.fetch('messages_default').include?('users[].permissions'),
      'messages channel must document the explicit per-user grant path')
check(asyncapi.fetch('components').fetch('securitySchemes').fetch('bearerAuth')
              .fetch('description').include?('First-frame'),
      'bearerAuth must document in-band first-frame authentication')

# Explicit roles, no wildcard, no default message.read.
roles = policy.fetch('roles')
roles.each do |role, grants|
  check(!grants.include?('*'), "#{role} must not use wildcard grants")
  check(!grants.include?('message.read'), "#{role} must not grant message.read by default")
end
check(roles.fetch('admin').include?('audit.read') && roles.fetch('admin').include?('user.read'),
      'admin must explicitly list audit.read and user.read')

# PluginManifest must advertise only the implemented runtime/isolation values.
manifest = openapi.fetch('components').fetch('schemas').fetch('PluginManifest')
check(manifest.fetch('properties').fetch('runtime').fetch('enum') == ['python-rpc'],
      'PluginManifest.runtime must match the python-rpc-only implementation')
check(manifest.fetch('properties').fetch('isolation').fetch('enum') == ['process'],
      'PluginManifest.isolation must match the process-only implementation')
%w[entrypoint capabilities dependencies config_schema permissions ordered].each do |field|
  check(manifest.fetch('properties').key?(field), "PluginManifest must mirror the protobuf field #{field}")
end

# Unified identifier syntax used by the Action endpoints.
parameters = openapi.fetch('components').fetch('parameters')
check(parameters.fetch('ActionId').fetch('schema').fetch('maxLength') == 128 &&
      parameters.fetch('ActionId').fetch('schema').fetch('pattern') == '^[A-Za-z0-9._-]+$',
      'ActionId length/charset')
check(parameters.fetch('ActionLookupKey').fetch('schema').fetch('maxLength') == 128,
      'ActionLookupKey length')
check(parameters.fetch('SessionId').fetch('schema').fetch('maxLength') == 256,
      'SessionId length')
check(parameters.fetch('PluginId').fetch('schema').fetch('pattern') == '^[a-z0-9][a-z0-9.-]{1,63}$',
      'PluginId pattern')

# REST Action queries must reuse the bridge retention/EXPIRED rules.
action_results = policy.fetch('action_results')
check(action_results.fetch('terminal_retention_seconds') == 604800, 'Action terminal retention')
check(action_results.fetch('missing_expired_invisible') == 'NOT_FOUND', 'Action lookup NOT_FOUND rule')
check(action_results.fetch('deadline_transition') == 'EXPIRED', 'Action deadline transition')
check(action_results.fetch('expired_error_code') == 'TIMEOUT', 'EXPIRED uses TIMEOUT')
runtime_semantics = File.read(File.join(root, 'doc/01-runtime-semantics.md'))
check(runtime_semantics.include?('168') && runtime_semantics.include?('EXPIRED'),
      'runtime semantics must state the 168 hour retention and EXPIRED transition')
check(runtime_semantics.include?('未知结果'), 'runtime semantics must state the unknown-outcome close rule')
puts 'OpenAPI and AsyncAPI channel, status, role, manifest and Action retention contract checks passed'

check(policy.fetch('application_port') == 'in-process-cpp-v1', 'control port decision')
check(policy.fetch('default_authorization') == 'deny', 'default deny')
check(policy.fetch('idempotency').fetch('retention_seconds') == 86400, 'idempotency retention')
check(policy.fetch('pagination').fetch('retention_seconds') == 600, 'snapshot retention')
check(operations.fetch('/auth/sessions/{sessionId}/revoke').fetch('post').fetch('responses').key?('204'), 'session revoke')
check(operations.fetch('/auth/me').fetch('get').fetch('operationId') == 'getCurrentPrincipal', 'current principal')
binary = ENV.fetch('CONTROL_POLICY_BINARY', File.join(root, 'build/proto/kiarabot-control-application-test'))
output, status = Open3.capture2(binary, '--dump-policy')
check(status.success?, 'Core policy inspection failed')
core_policy = output.lines.map { |line| fields = line.split; [fields.first, fields.drop(1)] }.to_h
count = 0
operations.each do |path, verbs|
  verbs.each do |method, operation|
    next unless %w[get post put].include?(method)
    if operation['x-process-probe']
      check(%w[/health/live /health/ready].include?(path), 'Unknown process probe')
      check(method == 'get' && operation['security'] == [] && operation['x-permission'] == 'public', 'Probe authentication contract')
      check(verbs['servers'] == [{ 'url' => '/' }], 'Process probes must use root paths')
      check(operation.fetch('responses').key?('200'), 'Probe response missing')
      next
    end
    count += 1
    name = operation.fetch('operationId')
    permission = operation.fetch('x-permission')
    observed = core_policy.fetch(name)
    check(observed[0] == permission, "Core permission drift: #{name}")
    check(observed[1] == (method == 'get' ? '0' : '1'), "Core mutation drift: #{name}")
    if !%w[public refresh.credential].include?(permission)
      %w[admin operator viewer].each_with_index do |role, index|
        grants = policy.fetch('roles').fetch(role)
        expected = grants.include?(permission)
        check((observed[2 + index] == '1') == expected, "Core role drift: #{role}/#{permission}")
      end
    end
    %w[400 401 429 500 503 504].each do |code|
      check(operation.fetch('responses').key?(code), "Missing #{code}: #{path}")
    end
    if method != 'get'
      check(operation.fetch('responses').key?('409'), "Missing idempotency conflict: #{path}")
    end
  end
end
check(core_policy.size == count, 'Core and REST operation coverage differ')
check(asyncapi.fetch('x-control-session-policy').fetch('retention_seconds') == 600, 'stream retention')
puts 'Control session, role, operation, pagination and error contracts match the compiled Core port'
