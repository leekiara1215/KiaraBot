#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=log.sh
. "$script_dir/log.sh"

usage() {
  echo "usage: $0 --url URL [--endpoint /health/live|/health/ready|/api/v1/health] [--token-file FILE]" >&2
}

base_url="${HEALTH_URL:-http://127.0.0.1:8080}"
endpoint="${HEALTH_ENDPOINT:-/health/ready}"
token_file=""
while (($# > 0)); do
  case "$1" in
    --url) [[ $# -ge 2 ]] || { usage; exit 2; }; base_url="$2"; shift 2 ;;
    --endpoint) [[ $# -ge 2 ]] || { usage; exit 2; }; endpoint="$2"; shift 2 ;;
    --token-file) [[ $# -ge 2 ]] || { usage; exit 2; }; token_file="$2"; shift 2 ;;
    --help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done
case "$endpoint" in
  /health/live|/health/ready|/api/v1/health) ;;
  *) kiarabot_log error healthcheck check rejected "unsupported health endpoint" INVALID_ARGUMENT; exit 2 ;;
esac

url="${base_url%/}${endpoint}"
if [[ "$endpoint" == /api/v1/health ]]; then
  if [[ -z "$token_file" || ! -f "$token_file" || ! -r "$token_file" ]]; then
    kiarabot_log error healthcheck check rejected "readable token file required" INVALID_ARGUMENT
    exit 2
  fi
  if ! token=$(cat -- "$token_file"); then
    kiarabot_log error healthcheck check rejected "unable to read token file" INVALID_ARGUMENT
    exit 2
  fi
  if [[ ! "$token" =~ ^[a-zA-Z0-9._~+/-]+={0,}$ ]]; then
    kiarabot_log error healthcheck check rejected "invalid token file" INVALID_ARGUMENT
    exit 2
  fi
elif [[ -n "$token_file" ]]; then
  kiarabot_log error healthcheck check rejected "token file is only valid for authenticated health" INVALID_ARGUMENT
  exit 2
fi
probe() {
  curl "$@" --fail --silent --show-error \
    --connect-timeout "${HEALTH_CONNECT_TIMEOUT_SECONDS:-2}" \
    --max-time "${HEALTH_TIMEOUT_SECONDS:-5}" "$url" >/dev/null
}
if { if [[ "$endpoint" == /api/v1/health ]]; then
       printf 'header = "Authorization: Bearer %s"\n' "$token" | probe --config -
     else
       probe </dev/null
     fi; }; then
  kiarabot_log info healthcheck check success "health endpoint responded" ""
  exit 0
fi
kiarabot_log error healthcheck check failure "health endpoint unavailable" UPSTREAM_UNAVAILABLE
exit 1
