#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=log.sh
. "$script_dir/log.sh"

usage() {
  echo "usage: $0 --url URL [--endpoint /health/live|/health/ready|/health/details]" >&2
}

base_url="${HEALTH_URL:-http://127.0.0.1:8080}"
endpoint="${HEALTH_ENDPOINT:-/health/ready}"
while (($# > 0)); do
  case "$1" in
    --url) [[ $# -ge 2 ]] || { usage; exit 2; }; base_url="$2"; shift 2 ;;
    --endpoint) [[ $# -ge 2 ]] || { usage; exit 2; }; endpoint="$2"; shift 2 ;;
    --help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done
case "$endpoint" in
  /health/live|/health/ready|/health/details) ;;
  *) kiarabot_log error healthcheck check rejected "unsupported health endpoint" INVALID_ARGUMENT; exit 2 ;;
esac

url="${base_url%/}${endpoint}"
if curl --fail --silent --show-error --connect-timeout "${HEALTH_CONNECT_TIMEOUT_SECONDS:-2}" \
    --max-time "${HEALTH_TIMEOUT_SECONDS:-5}" "$url" >/dev/null; then
  kiarabot_log info healthcheck check success "health endpoint responded" ""
  exit 0
fi
kiarabot_log error healthcheck check failure "health endpoint unavailable" UPSTREAM_UNAVAILABLE
exit 1
