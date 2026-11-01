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
count = 0
openapi['paths'].each do |path, verbs|
  verbs.each do |verb, operation|
    next unless operation.is_a?(Hash) && operation['x-owner-rpc']
    raise 'missing unavailable marker' unless operation['x-contract-status'] == 'not_implemented'
    raise 'missing permission' unless operation['x-permission']
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
