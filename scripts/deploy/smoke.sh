#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=log.sh
. "$script_dir/log.sh"

usage() {
  cat >&2 <<'EOF'
usage: smoke.sh [--core-url URL] [--adapter-url URL] [--panel-url URL]
                [--token-file FILE] [--skip adapter,panel]
EOF
}

core_url="${KIARABOT_SMOKE_CORE_URL:-http://127.0.0.1:${KIARABOT_CONTROL_PORT:-8080}}"
adapter_url="${KIARABOT_SMOKE_ADAPTER_URL:-http://127.0.0.1:${KIARABOT_ADAPTER_HEALTH_PORT:-8081}}"
panel_url="${KIARABOT_SMOKE_PANEL_URL:-http://127.0.0.1:${KIARABOT_PANEL_PORT:-8082}}"
token_file="${KIARABOT_SMOKE_TOKEN_FILE:-}"
skip="${KIARABOT_SMOKE_SKIP:-}"
while (($# > 0)); do
  case "$1" in
    --core-url) [[ $# -ge 2 ]] || { usage; exit 2; }; core_url="$2"; shift 2 ;;
    --adapter-url) [[ $# -ge 2 ]] || { usage; exit 2; }; adapter_url="$2"; shift 2 ;;
    --panel-url) [[ $# -ge 2 ]] || { usage; exit 2; }; panel_url="$2"; shift 2 ;;
    --token-file) [[ $# -ge 2 ]] || { usage; exit 2; }; token_file="$2"; shift 2 ;;
    --skip) [[ $# -ge 2 ]] || { usage; exit 2; }; skip="$2"; shift 2 ;;
    --help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

skipped() {
  local item
  local IFS=,
  for item in $skip; do
    [[ "$item" == "$1" ]] && return 0
  done
  return 1
}

check() {
  local name="$1" url="$2" endpoint="$3"
  shift 3
  if ! "$script_dir/healthcheck.sh" --url "$url" --endpoint "$endpoint" "$@"; then
    kiarabot_log error smoke check failure "deployment smoke check failed" UPSTREAM_UNAVAILABLE
    exit 1
  fi
  kiarabot_log info smoke check success "$name probe passed" ""
}

# Core: liveness, readiness (storage write probe) and, when a token file is
# supplied, the authenticated component detail endpoint.
check core-live "$core_url" /health/live
check core-ready "$core_url" /health/ready
if [[ -n "$token_file" ]]; then
  check core-detail "$core_url" /api/v1/health --token-file "$token_file"
fi
if ! skipped adapter; then
  check adapter-ready "$adapter_url" /health/ready
fi
if ! skipped panel; then
  check panel-ready "$panel_url" /health/ready
fi
kiarabot_log info smoke complete success "deployment smoke checks passed" ""
