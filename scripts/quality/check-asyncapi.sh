#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
test -f scripts/ci/node_modules/@asyncapi/parser/package.json || { echo 'Run npm ci --prefix scripts/ci first' >&2; exit 1; }
node -e 'if(require("./scripts/ci/node_modules/@asyncapi/parser/package.json").version!=="3.6.6") process.exit(1)'
node scripts/ci/validate-asyncapi.cjs api/asyncapi.yaml
