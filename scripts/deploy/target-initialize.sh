#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
  cat >&2 <<'USAGE'
usage: target-initialize.sh SERVICE   # SERVICE: identity|runtime|scheduler|model
Runs the explicit empty-database initializer for one private PostgreSQL database.
Requires PG_HOST, PG_PORT, PG_CA_FILE, KIARABOT_DB_NAME, KIARABOT_MIGRATION_ROLE,
KIARABOT_RUNTIME_ROLE and KIARABOT_MIGRATION_PASSWORD_FILE.
This command never cleans an existing database, imports snapshots or auto-migrates.
USAGE
}

service="${1:-}"
case "$service" in
  identity|runtime|scheduler|model) ;;
  --help) usage; exit 0 ;;
  *) usage; exit 2 ;;
esac

for variable in PG_HOST PG_PORT PG_CA_FILE KIARABOT_DB_NAME KIARABOT_MIGRATION_ROLE \
                KIARABOT_RUNTIME_ROLE KIARABOT_MIGRATION_PASSWORD_FILE; do
  if [[ -z "${!variable:-}" ]]; then
    echo "target-initialize: missing $variable" >&2
    exit 2
  fi
done
for file in "$PG_CA_FILE" "$KIARABOT_MIGRATION_PASSWORD_FILE"; do
  [[ -f "$file" && ! -L "$file" ]] || { echo "target-initialize: missing regular file" >&2; exit 1; }
done

exec /usr/local/bin/kiarabot-pg-initialize \
  --initialize-empty "$service" "$PG_HOST" "$PG_PORT" "$KIARABOT_DB_NAME" \
  "$KIARABOT_MIGRATION_ROLE" "$KIARABOT_RUNTIME_ROLE" "$PG_CA_FILE" \
  "$KIARABOT_MIGRATION_PASSWORD_FILE"
