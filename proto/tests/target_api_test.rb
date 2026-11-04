#!/usr/bin/env ruby
# Target schema consistency only; no handler or authorization implementation claim.
require 'yaml'
require 'json'
require 'pathname'
root = Pathname.new(__dir__).join('../..').realpath
openapi = YAML.load_file(root.join('api/openapi.yaml'))
asyncapi = YAML.load_file(root.join('api/asyncapi.yaml'))
spec = JSON.parse(File.read(root.join('proto/tests/target_spec.json')))
raise 'error mapping drift' unless spec['error_mapping'].all? do |code, (grpc, http)|
  openapi['x-control-policy']['target_error_mapping'][code] == {'grpc' => grpc, 'http' => http}
end
resolve = lambda do |ref, origin|
  file, pointer = ref.split('#', 2)
  data = file.empty? ? origin : YAML.load_file(root.join('api', file))
  pointer.split('/').drop(1).each {|key| data = data.fetch(key.gsub('~1', '/').gsub('~0', '~'))}
  data
end
walk = lambda do |node, origin|
  case node
  when Hash
    resolve.call(node['$ref'], origin) if node.key?('$ref')
    node.each_value {|value| walk.call(value, origin)}
  when Array
    node.each {|value| walk.call(value, origin)}
  end
end
[openapi, asyncapi].each {|data| walk.call(data, data)}
declared_rpcs = spec['methods'].map {|m| m['service'] + '.' + m['method']}
# Internal dispatcher and HostBridge-only methods are never a client-visible API:
# no REST operation may bind them.
internal_rpcs = spec['methods'].select {|m| %w[internal_dispatcher host_bridge].include?(m['exposure']) }
                              .map {|m| m['service'] + '.' + m['method']}
openapi['paths'].each do |path, methods|
  methods.each do |verb, operation|
    next unless operation.is_a?(Hash) && operation['x-target-owner-rpc']
    raise 'internal dispatcher RPC exposed through REST' if internal_rpcs.include?(operation['x-target-owner-rpc'])
    raise 'undeclared target auth RPC' unless declared_rpcs.include?(operation['x-target-owner-rpc'])
    raise 'auth projection missing scope marker' unless operation['x-target-contract-status'] == 'not_implemented'
    projection = resolve.call(operation.fetch('x-target-response-schema'), openapi)
    raise 'auth projection missing authoritative time' unless projection.fetch('required').include?('checked_at')
  end
end
registry = JSON.parse(File.read(root.join('proto/tests/contract_status.json')))
registry_operations = registry.fetch('categories').fetch('operations')
count = 0
openapi['paths'].each do |path, verbs|
  verbs.each do |verb, operation|
    next unless operation.is_a?(Hash) && operation['x-owner-rpc']
    # Inline availability markers must mirror the single registry, not a blanket "everything is
    # unavailable" rule: a REST route bound to an owner RPC is unavailable exactly when the registry
    # still marks its operationId not_implemented. This keeps the target schema consistent after a
    # reviewed availability flip (HOST-04 host maintenance routes, 2026-11-04) without letting an
    # unlisted route silently claim availability.
    operation_key = "operations.#{operation.fetch('x-operation-id', '')}"
    operation_key = "operations.#{operation['operationId']}" if operation['operationId']
    registry_status = registry_operations[operation_key]
    raise "owner RPC route has no registry status: #{operation_key}" if registry_status.nil?
    if registry_status == 'not_implemented'
      raise 'missing unavailable marker' unless operation['x-contract-status'] == 'not_implemented'
    else
      raise 'unexpected unavailable marker' if operation['x-contract-status'] == 'not_implemented'
    end
    raise 'missing permission policy' unless operation['x-permission'] || operation['x-owner-permissions']
    bindings = operation['x-owner-rpc'].is_a?(Hash) ? operation['x-owner-rpc'].values : [operation['x-owner-rpc']]
    raise 'internal dispatcher RPC exposed through REST' if bindings.any? {|rpc| internal_rpcs.include?(rpc)}
    raise 'owner RPC mapping is undeclared' unless bindings.all? {|rpc| declared_rpcs.include?(rpc)}
    if operation['x-owner-rpc'].is_a?(Hash)
      if operation.key?('x-permission')
        raise 'multi-owner permission must exactly match every branch, never union/widen' unless operation.fetch('x-owner-permissions').values.uniq == [operation['x-permission']]
      end
      raise 'owner permission coverage drift' unless operation.fetch('x-owner-permissions').keys.sort == operation['x-owner-rpc'].keys.sort
      operation['x-owner-rpc'].each do |owner, rpc|
        method = spec['methods'].find {|m| m['service'] + '.' + m['method'] == rpc}
        raise 'owner permission mapping drift' unless operation.fetch('x-owner-permissions').fetch(owner) == method['permission']
      end
    else
      method = spec['methods'].find {|m| m['service'] + '.' + m['method'] == operation['x-owner-rpc']}
      raise 'RPC permission mapping drift' unless operation['x-permission'] == method['permission']
    end
    if verb != 'get'
      raise 'missing idempotency header' unless operation['parameters'].any? {|p| p['$ref'] == '#/components/parameters/IdempotencyKey'}
      raise 'missing body' unless operation.dig('requestBody','required')
    end
    count += 1
  end
end
raise 'missing scoped operation channel' unless asyncapi.dig('channels','operations','x-contract-status') == 'not_implemented'
raise 'missing recovery context' unless openapi.dig('components','schemas','Error','properties','failure_context')
puts "#{count} target HTTP operations, error mappings and all REST/WS references passed"
