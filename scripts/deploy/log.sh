#!/usr/bin/env bash
set -euo pipefail

kiarabot_json_escape() {
  local value="$1"
  value=${value//\\/\\\\}
  value=${value//\"/\\\"}
  value=${value//$'\n'/\\n}
  value=${value//$'\r'/\\r}
  value=${value//$'\t'/\\t}
  printf '%s' "$value"
}

kiarabot_log() {
  local level="$1" component="$2" operation="$3" outcome="$4" message="$5" error_code="${6:-}" request_id="${REQUEST_ID:-}" trace_id="${TRACE_ID:-}" resource_id="${RESOURCE_ID:-}"
  printf '{"timestamp":"%s","level":"%s","component":"%s","operation":"%s","outcome":"%s","request_id":"%s","trace_id":"%s","resource_id":"%s","message":"%s","error_code":"%s"}\n' \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    "$(kiarabot_json_escape "$level")" \
    "$(kiarabot_json_escape "$component")" \
    "$(kiarabot_json_escape "$operation")" \
    "$(kiarabot_json_escape "$outcome")" \
    "$(kiarabot_json_escape "$request_id")" \
    "$(kiarabot_json_escape "$trace_id")" \
    "$(kiarabot_json_escape "$resource_id")" \
    "$(kiarabot_json_escape "$message")" \
    "$(kiarabot_json_escape "$error_code")"
}
