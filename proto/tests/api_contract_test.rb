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
puts 'OpenAPI and AsyncAPI channel and Action contract checks passed'

policy = openapi.fetch('x-control-policy')
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
    count += 1
    name = operation.fetch('operationId')
    permission = operation.fetch('x-permission')
    observed = core_policy.fetch(name)
    check(observed[0] == permission, "Core permission drift: #{name}")
    check(observed[1] == (method == 'get' ? '0' : '1'), "Core mutation drift: #{name}")
    if !%w[public refresh.credential].include?(permission)
      %w[admin operator viewer].each_with_index do |role, index|
        grants = policy.fetch('roles').fetch(role)
        expected = grants.include?('*') || grants.include?(permission)
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
