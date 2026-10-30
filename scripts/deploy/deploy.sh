#!/usr/bin/env bash
set -euo pipefail

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

usage() { echo "usage: $0 start|stop|status|upgrade|rollback [image-digest]" >&2; }
require_digest() {
  [[ "$1" =~ ^sha256:[[:xdigit:]]{64}$ ]] || { kiarabot_log error deploy validate failure "image digest must be sha256:<64 hex>" INVALID_ARGUMENT; exit 2; }
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
    "${compose[@]}" up --detach --remove-orphans --wait
    kiarabot_log info deploy start success "services started" ""
    ;;
  stop)
    "${compose[@]}" down
    kiarabot_log info deploy stop success "services stopped" ""
    ;;
  status)
    "${compose[@]}" ps
    ;;
  upgrade)
    new_digest="${KIARABOT_CORE_IMAGE_DIGEST:-}"
    rollback_digest="${ROLLBACK_IMAGE_DIGEST:-}"
    require_digest "$new_digest"
    require_digest "$rollback_digest"
    "${compose[@]}" stop panel adapter core
    if KIARABOT_CORE_IMAGE="${repository}@${new_digest}" "${compose[@]}" run --rm --no-deps migrate \
        && start_in_order "${repository}@${new_digest}"; then
      kiarabot_log info deploy upgrade success "services upgraded" ""
    else
      kiarabot_log error deploy upgrade failure "upgrade failed; attempting image rollback" UPGRADE_FAILED
      "${compose[@]}" stop panel adapter core || true
      if start_in_order "${repository}@${rollback_digest}"; then
        kiarabot_log info deploy rollback success "previous image started; verify database compatibility" ""
      else
        kiarabot_log error deploy rollback failure "image rollback failed" ROLLBACK_FAILED
      fi
      exit 1
    fi
    ;;
  rollback)
    digest="${2:-${ROLLBACK_IMAGE_DIGEST:-}}"
    require_digest "$digest"
    "${compose[@]}" stop panel adapter core
    if start_in_order "${repository}@${digest}"; then
      kiarabot_log info deploy rollback success "specified image started; verify database compatibility" ""
    else
      kiarabot_log error deploy rollback failure "image rollback failed" ROLLBACK_FAILED
      exit 1
    fi
    ;;
  *) usage; exit 2 ;;
esac
