#!/usr/bin/env ruby
# frozen_string_literal: true

# P12k read-only audit: OpenAPI REST x-contract-status vs registry target execution scope.
#
# Classification:
#   A  target override implemented + real evidence, no owner-rpc, REST marker should be
#      implemented but is missing/not_implemented -> drift that must be fixed.
#   B  operation carries x-owner-rpc: target_api requires x-contract-status == not_implemented
#      even when the target forwarding has evidence (documented exception, not drift).
#   C  no target override/evidence: keep not_implemented (or no marker for reference-only paths).
#   D  marker implemented but target override missing/not_implemented -> over-claim.
#   E  x-owner-rpc with marker implemented -> violates the owner-RPC marker rule.
require 'yaml'
require 'json'

root = File.expand_path('../..', __dir__)
spec = YAML.load_file(File.join(root, 'api/openapi.yaml'))
registry = JSON.parse(File.read(File.join(root, 'proto/tests/contract_status.json')))
target = registry.fetch('execution_scopes').fetch('target')
overrides = target.fetch('overrides').fetch('operations', {})
evidence = target.fetch('evidence')

rows = []
spec.fetch('paths').each do |path, methods|
  methods.each do |verb, operation|
    next unless operation.is_a?(Hash) && operation['operationId']

    id = operation['operationId']
    key = 'operations.' + id
    effective = overrides[key] || 'not_implemented'
    marker = operation['x-contract-status']
    owner_rpc = operation.key?('x-owner-rpc')
    ev = evidence["operations:#{key}"]
    rows << {
      path: path, verb: verb.upcase, id: id, marker: marker, owner_rpc: owner_rpc,
      effective: effective, evidence: !(ev.nil? || ev.empty?)
    }
  end
end

classes = rows.group_by do |row|
  if row[:marker] == 'implemented' && row[:owner_rpc]
    'E'
  elsif row[:marker] == 'implemented' && row[:effective] != 'implemented'
    'D'
  elsif row[:marker] != 'implemented' && row[:owner_rpc]
    'B'
  elsif row[:marker] != 'implemented' && row[:effective] == 'implemented' && row[:evidence]
    'A'
  elsif row[:marker] != 'implemented'
    'C'
  else
    'OK'
  end
end

classes.each do |name, list|
  next if name == 'OK'

  puts "#{name}: #{list.size}"
  list.each do |row|
    puts format('  %-8s %-48s %-28s marker=%-16s owner_rpc=%-5s target=%-16s evidence=%s',
                row[:verb], row[:path], row[:id], row[:marker] || '(none)', row[:owner_rpc],
                row[:effective], row[:evidence])
  end
end
puts "OK: #{classes.fetch('OK', []).size}"

drift = classes.fetch('A', []) + classes.fetch('D', []) + classes.fetch('E', [])
if drift.empty?
  puts 'contract status consistency: PASS (no A/D/E drift; B owner-rpc markers are documented)'
else
  warn "contract status consistency: FAIL (#{drift.size} A/D/E drift entries)"
  exit 1
end
