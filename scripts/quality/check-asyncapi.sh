#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
command -v npm >/dev/null 2>&1 || { echo 'npm is required' >&2; exit 1; }
command -v node >/dev/null 2>&1 || { echo 'node is required' >&2; exit 1; }
scratch="$(mktemp -d "${TMPDIR:-/tmp}/kiarabot-asyncapi.XXXXXXXX")"
trap 'rm -rf "$scratch"' EXIT
npm install --prefix "$scratch" --no-save --no-package-lock --ignore-scripts @asyncapi/parser@3.6.6
node scripts/quality/validate-asyncapi.cjs "$scratch/node_modules/@asyncapi/parser" api/asyncapi.yaml
