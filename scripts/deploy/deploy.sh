#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=log.sh
. "$script_dir/log.sh"

environment="${KIARABOT_ENVIRONMENT:-production}"
case "$environment" in
  production) default_compose="$script_dir/../../deploy/compose.prod.yml" ;;
  staging) default_compose="$script_dir/../../deploy/compose.staging.yml" ;;
  *) kiarabot_log error deploy validate failure "unknown environment" INVALID_ARGUMENT; exit 2 ;;
esac
compose_file="${COMPOSE_FILE:-$default_compose}"
project_name="${COMPOSE_PROJECT_NAME:-kiarabot-${environment}}"
compose=(docker compose --project-name "$project_name" --file "$compose_file")
repository="${KIARABOT_CORE_IMAGE_REPOSITORY:-ghcr.io/leekiara1215/kiarabot-core}"
database_path="${DATABASE_PATH:-${KIARABOT_RUNTIME_DB:-/var/lib/kiarabot/kiarabot.db}}"
expected_schema_version="${EXPECTED_SCHEMA_VERSION:-6}"
runtime_uid="${KIARABOT_RUNTIME_UID:-10001}"
deploy_record_file="${DEPLOY_RECORD_FILE:-/var/lib/kiarabot/deploy-record.jsonl}"

usage() { echo "usage: $0 start|stop|status|verify|upgrade|rollback [image-digest]" >&2; }

require_digest() {
  [[ "$1" =~ ^sha256:[[:xdigit:]]{64}$ ]] || { kiarabot_log error deploy validate failure "image digest must be sha256:<64 hex>" INVALID_ARGUMENT; exit 2; }
}

file_mode() {
  stat -c %a "$1" 2>/dev/null || stat -f %Lp "$1" 2>/dev/null || true
}
file_owner() {
  stat -c %u "$1" 2>/dev/null || stat -f %u "$1" 2>/dev/null || true
}

# The control configuration is a 0600 file owned by the container user and its
# "listen" field must match KIARABOT_CONTROL_PORT. Fail before touching Docker
# instead of watching Core crash-loop on an unreadable configuration.
preflight() {
  local control_config="${KIARABOT_CONTROL_CONFIG_FILE:-$script_dir/../../deploy/control/control.json}"
  local mode owner listen expected_listen
  if [[ -e "$control_config" ]]; then
    if [[ ! -f "$control_config" ]]; then
      kiarabot_log error deploy preflight failure "control configuration is not a regular file" INVALID_ARGUMENT
      exit 1
    fi
    mode=$(file_mode "$control_config")
    if [[ "$mode" != "600" ]]; then
      kiarabot_log error deploy preflight failure "control configuration must be mode 0600" INVALID_ARGUMENT
      exit 1
    fi
    owner=$(file_owner "$control_config")
    if [[ "$owner" != "$runtime_uid" ]]; then
      kiarabot_log error deploy preflight failure "control configuration must be owned by the runtime uid" INVALID_ARGUMENT
      exit 1
    fi
    listen=$(sed -n 's/.*"listen"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$control_config" | head -n 1)
    expected_listen="127.0.0.1:${KIARABOT_CONTROL_PORT:-8080}"
    if [[ -n "$listen" && "$listen" != "$expected_listen" ]]; then
      kiarabot_log error deploy preflight failure "control listen must equal KIARABOT_CONTROL_PORT" INVALID_ARGUMENT
      exit 1
    fi
  else
    kiarabot_log info deploy preflight warning "control configuration not found; Core will fail until it is mounted" ""
  fi
}

# Deployments are recorded as append-only JSON lines so a later rollback can
# find the previous successful digest without an operator copying it by hand.
record_deployment() {
  local image="$1" outcome="$2" schema_version="" directory
  if command -v sqlite3 >/dev/null 2>&1 && [[ -f "$database_path" ]]; then
    schema_version=$(sqlite3 "$database_path" 'PRAGMA user_version;' 2>/dev/null || true)
  fi
  directory=$(dirname -- "$deploy_record_file")
  if ! mkdir -p "$directory" 2>/dev/null || ! printf '{"timestamp":"%s","environment":"%s","outcome":"%s","image":"%s","schema_version":"%s","commit":"%s"}\n' \
      "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$environment" "$outcome" "$image" "$schema_version" "${KIARABOT_COMMIT:-}" >> "$deploy_record_file" 2>/dev/null; then
    kiarabot_log info deploy record warning "deployment record is not writable" ""
  fi
}

last_recorded_image() {
  local image
  [[ -f "$deploy_record_file" ]] || return 0
  image=$(tail -n 1 "$deploy_record_file" | sed -n 's/.*"image"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
  # Records store the full image reference; rollback needs the digest part only.
  printf '%s\n' "${image##*@}"
}

# Only a database whose schema matches the target binary may be used with a
# rolled-back image. Operators can override with ROLLBACK_FORCE=1 after
# restoring a migration backup.
check_schema_compatible() {
  local version
  if ! command -v sqlite3 >/dev/null 2>&1; then
    kiarabot_log info deploy schema skipped "sqlite3 unavailable; schema compatibility not verified" ""
    return 0
  fi
  if [[ ! -f "$database_path" ]]; then
    kiarabot_log info deploy schema skipped "database does not exist yet" ""
    return 0
  fi
  version=$(sqlite3 "$database_path" 'PRAGMA user_version;' 2>/dev/null || true)
  if [[ "$version" != "$expected_schema_version" ]]; then
    if [[ "${ROLLBACK_FORCE:-0}" == "1" ]]; then
      kiarabot_log info deploy schema warning "schema mismatch accepted because ROLLBACK_FORCE=1" ""
      return 0
    fi
    kiarabot_log error deploy schema failure "database schema $version does not match expected $expected_schema_version; restore the migration backup or set ROLLBACK_FORCE=1" SCHEMA_MISMATCH
    exit 1
  fi
  kiarabot_log info deploy schema success "database schema verified" ""
}

start_in_order() {
  local image="$1"
  local service
  for service in core adapter panel; do
    KIARABOT_CORE_IMAGE="$image" "${compose[@]}" up --detach --no-deps --wait "$service" || return 1
  done
}

case "${1:-}" in
  start)
    preflight
    build_args=()
    if [[ "${KIARABOT_BUILD_IMAGES:-0}" == "1" ]]; then
      build_args=(--build)
    fi
    if [[ ${#build_args[@]} -gt 0 ]]; then
      "${compose[@]}" up --detach --remove-orphans --wait "${build_args[@]}"
    else
      "${compose[@]}" up --detach --remove-orphans --wait
    fi
    record_deployment "${KIARABOT_CORE_IMAGE:-compose-default}" started
    kiarabot_log info deploy start success "services started" ""
    ;;
  stop)
    "${compose[@]}" down
    kiarabot_log info deploy stop success "services stopped" ""
    ;;
  status)
    "${compose[@]}" ps
    ;;
  verify)
    "$script_dir/smoke.sh"
    ;;
  upgrade)
    new_digest="${KIARABOT_CORE_IMAGE_DIGEST:-}"
    require_digest "$new_digest"
    preflight
    "${compose[@]}" stop panel adapter core
    if KIARABOT_CORE_IMAGE="${repository}@${new_digest}" "${compose[@]}" run --rm --no-deps migrate \
        && start_in_order "${repository}@${new_digest}"; then
      record_deployment "${repository}@${new_digest}" upgraded
      kiarabot_log info deploy upgrade success "services upgraded" ""
    else
      kiarabot_log error deploy upgrade failure "upgrade failed; verify schema and backup before rollback" UPGRADE_FAILED
      "${compose[@]}" stop panel adapter core || true
      exit 1
    fi
    ;;
  rollback)
    digest="${2:-${ROLLBACK_IMAGE_DIGEST:-}}"
    if [[ -z "$digest" ]]; then
      digest=$(last_recorded_image)
    fi
    require_digest "$digest"
    preflight
    check_schema_compatible
    "${compose[@]}" stop panel adapter core
    if start_in_order "${repository}@${digest}"; then
      record_deployment "${repository}@${digest}" rolled-back
      kiarabot_log info deploy rollback success "specified image started; schema was verified before rollback" ""
    else
      kiarabot_log error deploy rollback failure "image rollback failed" ROLLBACK_FAILED
      exit 1
    fi
    ;;
  *) usage; exit 2 ;;
esac
