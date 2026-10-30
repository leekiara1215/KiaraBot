#!/usr/bin/env ruby
# Contract-shape checks complement the OpenAPI and AsyncAPI validators.
require 'yaml'

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
    check(ref.end_with?("/#{name}Envelope"), "#{channel} has an incorrect envelope")
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
check(messages.fetch('Session').fetch('payload').fetch('$ref').end_with?('/Session'),
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
