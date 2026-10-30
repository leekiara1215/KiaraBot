#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=log.sh
. "$script_dir/log.sh"

usage() {
  echo "usage: $0 backup --database PATH --output DIR | restore --database PATH --input BACKUP_FILE" >&2
}

mode="${1:-}"
shift || true
database_path="${DATABASE_PATH:-/var/lib/kiarabot/kiarabot.db}"
backup_dir="${BACKUP_DIR:-/var/lib/kiarabot/backups}"
input_file=""
while (($# > 0)); do
  case "$1" in
    --database) [[ $# -ge 2 ]] || { usage; exit 2; }; database_path="$2"; shift 2 ;;
    --output) [[ $# -ge 2 ]] || { usage; exit 2; }; backup_dir="$2"; shift 2 ;;
    --input) [[ $# -ge 2 ]] || { usage; exit 2; }; input_file="$2"; shift 2 ;;
    --help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

if command -v sha256sum >/dev/null 2>&1; then
  checksum() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
  checksum() { shasum -a 256 "$1" | awk '{print $1}'; }
else
  kiarabot_log error backup validate failure "SHA-256 utility is required" DEPENDENCY_MISSING
  exit 1
fi

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
    sha=$(checksum "$target")
    version=$(sqlite3 "$target" 'PRAGMA user_version;')
    cat > "${target}.metadata" <<META
schema_version=$version
sha256=$sha
created_at=$stamp
META
    kiarabot_log info backup create success "database backup created" ""
    printf '%s\n' "$target"
    ;;
  restore)
    [[ -n "$input_file" && -f "$input_file" && -f "${input_file}.metadata" ]] || { kiarabot_log error backup restore failure "backup or metadata not found" BACKUP_NOT_FOUND; exit 1; }
    [[ ! -e "${database_path}-wal" && ! -e "${database_path}-shm" ]] || { kiarabot_log error backup restore failure "stop database users and remove SQLite sidecars before restore" DATABASE_BUSY; exit 1; }
    expected=$(awk -F= '$1 == "sha256" { print $2 }' "${input_file}.metadata")
    actual=$(checksum "$input_file")
    [[ -n "$expected" && "$expected" == "$actual" ]] || { kiarabot_log error backup restore failure "backup checksum mismatch" CHECKSUM_MISMATCH; exit 1; }
    umask 077
    mkdir -p "$(dirname -- "$database_path")"
    tmp="${database_path}.restore.$$"
    trap 'rm -f -- "$tmp"' EXIT
    cp -- "$input_file" "$tmp"
    mv -f -- "$tmp" "$database_path"
    trap - EXIT
    kiarabot_log info backup restore success "database backup restored" ""
    ;;
  *) usage; exit 2 ;;
esac
