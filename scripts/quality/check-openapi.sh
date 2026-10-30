#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
command -v npx >/dev/null 2>&1 || { echo 'npx is required' >&2; exit 1; }
npx --yes @redocly/cli@2.72.1 lint api/openapi.yaml
