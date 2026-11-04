#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
modules="${KIARABOT_API_NODE_MODULES:-$root/scripts/ci/node_modules}"
test -f "$modules/ajv/package.json" || { echo 'Run npm ci --prefix scripts/ci first' >&2; exit 1; }
scratch="$(mktemp -d "${TMPDIR:-/tmp}/kiarabot-target-api.XXXXXXXX")"
trap 'rm -rf "$scratch"' EXIT
ruby proto/tests/target_api_test.rb
ruby -rjson -ryaml -e 'puts JSON.generate(YAML.load_file(ARGV.fetch(0)))' api/openapi.yaml > "$scratch/openapi.json"
node proto/tests/target_schema_test.cjs "$modules" "$scratch/openapi.json"
ruby -rjson -ryaml -e 'puts JSON.generate(YAML.load_file(ARGV.fetch(0)))' api/asyncapi.yaml > "$scratch/asyncapi.json"
node proto/tests/ws_expansion_schema_test.cjs "$modules" "$scratch/openapi.json" "$scratch/asyncapi.json"
node proto/tests/host_bootstrap_schema_test.cjs "$modules" "$scratch/openapi.json"
