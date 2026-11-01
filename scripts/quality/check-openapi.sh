#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
test -x scripts/ci/node_modules/.bin/redocly || { echo 'Run npm ci --prefix scripts/ci first' >&2; exit 1; }
node -e 'if(require("./scripts/ci/node_modules/@redocly/cli/package.json").version!=="2.72.1") process.exit(1)'
scripts/ci/node_modules/.bin/redocly lint api/openapi.yaml
