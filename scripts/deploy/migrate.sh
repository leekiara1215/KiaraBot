#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=log.sh
. "$script_dir/log.sh"

lock_file="${MIGRATION_LOCK_FILE:-/var/lib/kiarabot/migrate.lock}"
database_path="${DATABASE_PATH:-/var/lib/kiarabot/kiarabot.db}"
backup_dir="${BACKUP_DIR:-/var/lib/kiarabot/backups}"
mkdir -p "$(dirname -- "$lock_file")" "$backup_dir"
if (set -o noclobber; : > "$lock_file") 2>/dev/null; then
  trap 'rm -f -- "$lock_file"' EXIT
else
  kiarabot_log error migrate lock failure "another migration is running" MIGRATION_LOCKED
  exit 1
fi

if [[ -f "$database_path" ]]; then
  "$script_dir/backup.sh" backup --database "$database_path" --output "$backup_dir"
else
  kiarabot_log info migrate backup skipped "database does not exist yet" ""
fi

if [[ -n "${MIGRATION_COMMAND:-}" ]]; then
  if bash -c "$MIGRATION_COMMAND"; then
    kiarabot_log info migrate apply success "database migration completed" ""
  else
    kiarabot_log error migrate apply failure "database migration failed" MIGRATION_FAILED
    exit 1
  fi
else
  kiarabot_log error migrate apply failure "migration command is not configured" MIGRATION_NOT_CONFIGURED
  exit 1
fi
