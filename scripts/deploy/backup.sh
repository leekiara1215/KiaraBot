#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=log.sh
. "$script_dir/log.sh"

usage() {
  cat >&2 <<'EOF'
usage: backup.sh backup  --database PATH --output DIR [--retain N]
       backup.sh restore --database PATH --input BACKUP_FILE --confirm
                          [--expected-schema-version N]
EOF
}

mode="${1:-}"
shift || true
database_path="${DATABASE_PATH:-/var/lib/kiarabot/kiarabot.db}"
backup_dir="${BACKUP_DIR:-/var/lib/kiarabot/backups}"
input_file=""
retain="${BACKUP_RETENTION:-7}"
confirm="${RESTORE_CONFIRM:-0}"
expected_schema_version="${EXPECTED_SCHEMA_VERSION:-6}"
while (($# > 0)); do
  case "$1" in
    --database) [[ $# -ge 2 ]] || { usage; exit 2; }; database_path="$2"; shift 2 ;;
    --output) [[ $# -ge 2 ]] || { usage; exit 2; }; backup_dir="$2"; shift 2 ;;
    --input) [[ $# -ge 2 ]] || { usage; exit 2; }; input_file="$2"; shift 2 ;;
    --retain) [[ $# -ge 2 ]] || { usage; exit 2; }; retain="$2"; shift 2 ;;
    --expected-schema-version) [[ $# -ge 2 ]] || { usage; exit 2; }; expected_schema_version="$2"; shift 2 ;;
    --confirm) confirm=1; shift ;;
    --help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

if ! [[ "$retain" =~ ^[0-9]+$ ]] || ((retain < 1)); then
  kiarabot_log error backup validate failure "backup retention must be a positive integer" INVALID_ARGUMENT
  exit 2
fi

if command -v sha256sum >/dev/null 2>&1; then
  checksum() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
  checksum() { shasum -a 256 "$1" | awk '{print $1}'; }
else
  kiarabot_log error backup validate failure "SHA-256 utility is required" DEPENDENCY_MISSING
  exit 1
fi

integrity_check() {
  sqlite3 "$1" 'PRAGMA integrity_check;' 2>/dev/null
}

# Keep only the newest ${retain} backup sets. Names are UTC timestamps, so a
# plain (C-locale) lexicographic glob is chronological order.
prune_backups() {
  local dir="$1" keep="$2" count=0 entry
  local -a sets=()
  for entry in "$dir"/kiarabot-*; do
    [[ -d "$entry" ]] || continue
    sets[${#sets[@]}]="$entry"
  done
  local total=${#sets[@]}
  for entry in "${sets[@]}"; do
    count=$((count + 1))
    if ((count <= total - keep)); then
      rm -rf -- "$entry"
    fi
  done
}

case "$mode" in
  backup)
    [[ -f "$database_path" ]] || { kiarabot_log error backup create failure "database does not exist" DATABASE_NOT_FOUND; exit 1; }
    command -v sqlite3 >/dev/null || { kiarabot_log error backup create failure "sqlite3 is required" DEPENDENCY_MISSING; exit 1; }
    umask 077
    mkdir -p "$backup_dir"
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    backup_set=$(mktemp -d "$backup_dir/kiarabot-${stamp}-XXXXXXXX")
    target="$backup_set/database.db"
    escaped_target=${target//\'/\'\'}
    if ! sqlite3 "$database_path" "VACUUM INTO '$escaped_target';"; then
      rm -rf -- "$backup_set"
      kiarabot_log error backup create failure "SQLite snapshot failed" BACKUP_FAILED
      exit 1
    fi
    if [[ "$(integrity_check "$target")" != "ok" ]]; then
      rm -rf -- "$backup_set"
      kiarabot_log error backup create failure "snapshot failed SQLite integrity check" BACKUP_INTEGRITY
      exit 1
    fi
    sha=$(checksum "$target")
    version=$(sqlite3 "$target" 'PRAGMA user_version;')
    cat > "${target}.metadata" <<META
schema_version=$version
sha256=$sha
created_at=$stamp
META
    prune_backups "$backup_dir" "$retain"
    kiarabot_log info backup create success "database backup created" ""
    printf '%s\n' "$target"
    ;;
  restore)
    # Restoring replaces live data: require an explicit confirmation flag so an
    # accidental invocation cannot overwrite the current database.
    if [[ "$confirm" != "1" ]]; then
      kiarabot_log error backup restore rejected "restore requires --confirm or RESTORE_CONFIRM=1" RESTORE_CONFIRMATION_REQUIRED
      exit 2
    fi
    [[ -n "$input_file" && -f "$input_file" && -f "${input_file}.metadata" ]] || { kiarabot_log error backup restore failure "backup or metadata not found" BACKUP_NOT_FOUND; exit 1; }
    [[ ! -e "${database_path}-wal" && ! -e "${database_path}-shm" ]] || { kiarabot_log error backup restore failure "stop database users and remove SQLite sidecars before restore" DATABASE_BUSY; exit 1; }
    if [[ -e "$(dirname -- "$database_path")/migrate.lock" ]]; then
      kiarabot_log error backup restore failure "stop the migration lock before restore" DATABASE_BUSY
      exit 1
    fi
    expected=$(awk -F= '$1 == "sha256" { print $2 }' "${input_file}.metadata")
    actual=$(checksum "$input_file")
    [[ -n "$expected" && "$expected" == "$actual" ]] || { kiarabot_log error backup restore failure "backup checksum mismatch" CHECKSUM_MISMATCH; exit 1; }
    command -v sqlite3 >/dev/null || { kiarabot_log error backup restore failure "sqlite3 is required" DEPENDENCY_MISSING; exit 1; }
    if [[ "$(integrity_check "$input_file")" != "ok" ]]; then
      kiarabot_log error backup restore failure "backup failed SQLite integrity check" BACKUP_INTEGRITY
      exit 1
    fi
    backup_version=$(sqlite3 "$input_file" 'PRAGMA user_version;')
    if [[ "$backup_version" != "$expected_schema_version" ]]; then
      kiarabot_log error backup restore rejected "backup schema version does not match the deployed binary" SCHEMA_MISMATCH
      exit 1
    fi
    umask 077
    mkdir -p "$(dirname -- "$database_path")"
    tmp="${database_path}.restore.$$"
    trap 'rm -f -- "$tmp"' EXIT
    cp -- "$input_file" "$tmp"
    if [[ "$(integrity_check "$tmp")" != "ok" ]]; then
      kiarabot_log error backup restore failure "copied database failed integrity check" BACKUP_INTEGRITY
      exit 1
    fi
    mv -f -- "$tmp" "$database_path"
    trap - EXIT
    kiarabot_log info backup restore success "database backup restored" ""
    ;;
  *) usage; exit 2 ;;
esac
