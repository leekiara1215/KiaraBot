#!/usr/bin/env ruby
# Compare the authoritative API catalog with executed Core rules, not a copied permission table.
require 'yaml'
require 'open3'
require 'set'

abort 'usage: identity_role_catalog_test.rb IDENTITY_RULES_TEST_BINARY [OPENAPI_FILE]' unless (1..2).cover?(ARGV.length)
root = File.expand_path('../..', __dir__)
api_path = ARGV[1] || File.join(root, 'api/openapi.yaml')
expected = YAML.safe_load_file(api_path).fetch('x-control-policy').fetch('target_role_capabilities')
raise 'empty role catalog' if expected.empty?
expected.each do |role, permissions|
  raise "invalid or duplicate API permissions for #{role}" unless permissions.is_a?(Array) &&
    permissions.all? { |permission| permission.is_a?(String) && !permission.empty? } &&
    permissions.uniq.length == permissions.length
end
stdout, stderr, status = Open3.capture3(ARGV.fetch(0), '--role-catalog')
raise "Core role catalog failed: #{stderr}" unless status.success?
actual = Hash.new { |roles, role| roles[role] = Set.new }
stdout.each_line do |line|
  role, permission, extra = line.chomp.split("\t", -1)
  raise "malformed Core role row: #{line.inspect}" if role.nil? || role.empty? ||
    permission.nil? || permission.empty? || !extra.nil?
  raise "duplicate Core role row: #{line.inspect}" unless actual[role].add?(permission)
end
raise "role names differ: API=#{expected.keys.sort} Core=#{actual.keys.sort}" unless
  expected.keys.to_set == actual.keys.to_set
expected.each do |role, permissions|
  missing = permissions.to_set - actual.fetch(role)
  extra = actual.fetch(role) - permissions.to_set
  raise "role #{role} differs: missing=#{missing.to_a.sort} extra=#{extra.to_a.sort}" unless
    missing.empty? && extra.empty?
end
puts "#{expected.length} role catalogs match executed Core rules exactly"
