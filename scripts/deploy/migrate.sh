#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=log.sh
. "$script_dir/log.sh"

lock_file="${MIGRATION_LOCK_FILE:-/var/lib/kiarabot/migrate.lock}"
database_path="${DATABASE_PATH:-/var/lib/kiarabot/kiarabot.db}"
backup_dir="${BACKUP_DIR:-/var/lib/kiarabot/backups}"
expected_schema_version="${EXPECTED_SCHEMA_VERSION:-6}"
# The authoritative migration command is the standalone kiarabot-migrate binary
# shipped in the Core image. Operators may override it for staging, but the
# command must accept exactly one database path argument.
migration_command="${MIGRATION_COMMAND:-kiarabot-migrate}"

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

# Replace an explicit {database} placeholder, otherwise append the database
# path so the canonical "kiarabot-migrate <path>" invocation is the default.
if [[ "$migration_command" == *"{database}"* ]]; then
  migration_invocation="${migration_command//\{database\}/$(printf '%q' "$database_path")}"
else
  migration_invocation="$migration_command $(printf '%q' "$database_path")"
fi
if bash -c -- "$migration_invocation"; then
  kiarabot_log info migrate apply success "database migration completed" ""
else
  kiarabot_log error migrate apply failure "database migration failed" MIGRATION_FAILED
  exit 1
fi

# Nothing may reach Core before the on-disk schema matches the expected version.
if command -v sqlite3 >/dev/null 2>&1 && [[ -f "$database_path" ]]; then
  migrated_version=$(sqlite3 "$database_path" 'PRAGMA user_version;' 2>/dev/null || true)
  if [[ "$migrated_version" != "$expected_schema_version" ]]; then
    kiarabot_log error migrate verify failure "unexpected schema version after migration" MIGRATION_SCHEMA_MISMATCH
    exit 1
  fi
  kiarabot_log info migrate verify success "schema version verified" ""
else
  kiarabot_log info migrate verify skipped "sqlite3 or database unavailable; schema not verified" ""
fi
