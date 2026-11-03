#!/usr/bin/env bash
# P14c target backup: four private PostgreSQL databases + Host/Adapter SQLite + deployment
# material (CA/certificates/root key/config/credential directory). Logical dumps are taken
# only after an explicit offline check: every writer must be stopped (P13b drain complete).
# This script never migrates, cleans or restores; it only creates a complete backup set.
set -euo pipefail
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib.sh
. "$script_dir/lib.sh"

usage() {
  cat >&2 <<'EOF'
usage: target-backup.sh --root DIR --env FILE --output DIR [--retain N] [--offline]

  --root DIR      deployment material root (config/, tls/, secrets/, credentials/, data/)
  --env FILE      trusted operator env file (sourced) defining:
                  PG_HOST PG_PORT PG_CA_FILE PG_BIN
                  IDENTITY_DB IDENTITY_MIGRATION_ROLE IDENTITY_MIGRATION_PASSWORD_FILE
                  RUNTIME_DB  RUNTIME_MIGRATION_ROLE  RUNTIME_MIGRATION_PASSWORD_FILE
                  SCHEDULER_DB SCHEDULER_MIGRATION_ROLE SCHEDULER_MIGRATION_PASSWORD_FILE
                  MODEL_DB    MODEL_MIGRATION_ROLE    MODEL_MIGRATION_PASSWORD_FILE
                  HOST_SQLITE_PATH ADAPTER_OUTBOX_PATH [ADAPTER_ACTIONS_PATH]
                  [HOST_CREDENTIAL_DIRECTORY]
  --output DIR    backup root; a new target-<UTC>-<rand> set is created below it
  --retain N      keep newest N complete sets (default 7)
  --offline       required confirmation that services/writers are stopped

The backup is quiesced: RPO is zero for committed data if no writer ran after --offline.
Missing material, other database connections, a failed pg_dump/SQLite checkpoint, a
checksum failure or an incomplete set aborts without publishing manifest.txt. No secrets
are printed. Restore is explicit and never automatic.
EOF
}

root="" env_file="" output="" retain=7 offline=0
while (($# > 0)); do
  case "$1" in
    --root) [[ $# -ge 2 ]] || { usage; exit 2; }; root="$2"; shift 2 ;;
    --env) [[ $# -ge 2 ]] || { usage; exit 2; }; env_file="$2"; shift 2 ;;
    --output) [[ $# -ge 2 ]] || { usage; exit 2; }; output="$2"; shift 2 ;;
    --retain) [[ $# -ge 2 ]] || { usage; exit 2; }; retain="$2"; shift 2 ;;
    --offline) offline=1; shift ;;
    --help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done
[[ -n "$root" && -n "$env_file" && -n "$output" ]] || { usage; exit 2; }
((offline == 1)) || fail "refusing backup: pass --offline only after services are stopped (P13b drain)" 2
[[ -f "$env_file" && ! -L "$env_file" ]] || fail "env file missing or symlink" 1
[[ -d "$root" && ! -L "$root" ]] || fail "material root missing or symlink" 1
# Trusted operator-provided file; no untrusted input reaches this script.
# shellcheck disable=SC1090
. "$env_file"
: "${PG_HOST:?PG_HOST missing}" "${PG_PORT:?PG_PORT missing}" "${PG_CA_FILE:?PG_CA_FILE missing}"
PG_BIN="${PG_BIN:-$(dirname "$(command -v psql)")}"
export PG_BIN
[[ -d "$PG_BIN" ]] || fail "PG_BIN directory missing" 1
for cmd in psql pg_dump pg_restore; do [[ -x "$PG_BIN/$cmd" ]] || fail "missing $PG_BIN/$cmd" 1; done
for cmd in sqlite3 tar; do need_cmd "$cmd"; done
for svc in identity runtime scheduler model; do
  db_var=$(service_db_var "$svc"); role_var=$(service_role_var "$svc"); pass_var=$(service_pass_var "$svc")
  : "${!db_var:?$db_var missing}" "${!role_var:?$role_var missing}" "${!pass_var:?$pass_var missing}"
  [[ -f "${!pass_var}" && ! -L "${!pass_var}" ]] || fail "password file missing for $svc" 1
done
: "${HOST_SQLITE_PATH:?HOST_SQLITE_PATH missing}" "${ADAPTER_OUTBOX_PATH:?ADAPTER_OUTBOX_PATH missing}"
[[ -f "$PG_CA_FILE" && ! -L "$PG_CA_FILE" ]] || fail "PG_CA_FILE missing" 1
for required in "$root/config" "$root/tls" "$root/secrets" "$root/tls/ca.pem" \
                "$root/secrets/model-root-key.hex"; do
  [[ -e "$required" ]] || fail "required deployment material missing: ${required#"$root/"}" 1
done
[[ -d "$root/config" && -d "$root/tls" && -d "$root/secrets" ]] || fail "material directories invalid" 1
for file in "$HOST_SQLITE_PATH" "$ADAPTER_OUTBOX_PATH"; do
  [[ -f "$file" && ! -L "$file" ]] || fail "SQLite database missing or symlink: $(basename "$file")" 1
done
if [[ -n "${ADAPTER_ACTIONS_PATH:-}" ]]; then
  [[ -f "$ADAPTER_ACTIONS_PATH" && ! -L "$ADAPTER_ACTIONS_PATH" ]] || fail "adapter actions SQLite missing" 1
fi
if [[ -n "${HOST_CREDENTIAL_DIRECTORY:-}" ]]; then
  [[ -d "$HOST_CREDENTIAL_DIRECTORY" && ! -L "$HOST_CREDENTIAL_DIRECTORY" ]] || fail "host credential directory missing" 1
fi
if ! [[ "$retain" =~ ^[0-9]+$ ]] || ((retain < 1)); then fail "retain must be a positive integer" 2; fi

umask 077
mkdir -p "$output"
stamp=$(date -u +%Y%m%dT%H%M%SZ)
set_dir=$(mktemp -d "$output/target-$stamp-XXXXXXXX")
cleanup_on_fail() { rm -rf -- "$set_dir"; }
trap cleanup_on_fail EXIT
mkdir -p "$set_dir/postgres" "$set_dir/sqlite" "$set_dir/material"
created_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
log "backup set $set_dir starting (offline/quiesced)"

# Offline guard: a backup is only valid when no client connection is still writing. We must not
# silently take a cross-database backup while one service still accepts writes.
for svc in identity runtime scheduler model; do
  db_var=$(service_db_var "$svc"); db="${!db_var}"
  active=$(other_connections "$svc" "$db" 2>/dev/null) \
    || fail "cannot inspect $svc database; is it available?" 1
  [[ "$active" == "0" ]] || fail "$svc database still has $active client connection(s); stop writers and retry" 1
done

# PostgreSQL: custom-format logical dumps, sequential because cross-database snapshots do not exist.
# The offline guard is what makes the four dumps collectively consistent.
for svc in identity runtime scheduler model; do
  db_var=$(service_db_var "$svc"); db="${!db_var}"
  version=$(psql_run "$svc" "$db" "SELECT version FROM kiarabot.schema_versions WHERE singleton;") \
    || fail "cannot read $svc schema version" 1
  [[ "$version" =~ ^[1-9][0-9]*$ ]] || fail "$svc schema_versions is missing or invalid" 1
  dump="$set_dir/postgres/$svc.dump"
  dump_env "$svc" --format=custom --no-owner --file="$dump" "$db" \
    || fail "$svc pg_dump failed" 1
  [[ -s "$dump" ]] || fail "$svc pg_dump produced an empty file" 1
  digest=$(checksum "$dump")
  cat > "$set_dir/postgres/$svc.metadata" <<META
service=$svc
database=$db
schema_version=$version
sha256=$digest
created_at=$created_at
META
  log "pg_dump $svc schema=$version sha256=$digest"
done

# SQLite: checkpoint WAL first, then VACUUM INTO a private snapshot. Copying a live DB file or
# omitting -wal/-shm would lose committed transactions.
sqlite_snapshot() {
  local src="$1" dst="$2" label="$3" version
  [[ -f "$src" ]] || fail "$label SQLite missing" 1
  sqlite3 "$src" 'PRAGMA wal_checkpoint(TRUNCATE);' >/dev/null || fail "$label WAL checkpoint failed" 1
  rm -f -- "$dst"
  sqlite3 "$src" "VACUUM INTO '$dst';" || fail "$label SQLite snapshot failed" 1
  [[ "$(sqlite3 "$dst" 'PRAGMA integrity_check;')" == "ok" ]] || fail "$label SQLite integrity check failed" 1
  version=$(sqlite3 "$dst" 'PRAGMA user_version;')
  printf '%s\n' "$(checksum "$dst") $version" > "$dst.metadata"
  log "$label snapshot user_version=$version sha256=$(awk '{print $1}' "$dst.metadata")"
}
sqlite_snapshot "$HOST_SQLITE_PATH" "$set_dir/sqlite/host.sqlite" "host journal"
sqlite_snapshot "$ADAPTER_OUTBOX_PATH" "$set_dir/sqlite/adapter-outbox.sqlite" "adapter outbox"
if [[ -n "${ADAPTER_ACTIONS_PATH:-}" ]]; then
  sqlite_snapshot "$ADAPTER_ACTIONS_PATH" "$set_dir/sqlite/adapter-actions.sqlite" "adapter actions"
fi

# Deployment material: everything needed to reopen the encrypted/authenticated services.
# tar preserves modes; no secret content is printed.
cp -a -- "$root/config" "$set_dir/material/config"
cp -a -- "$root/tls" "$set_dir/material/tls"
cp -a -- "$root/secrets" "$set_dir/material/secrets"
if [[ -n "${HOST_CREDENTIAL_DIRECTORY:-}" ]]; then
  cp -a -- "$HOST_CREDENTIAL_DIRECTORY" "$set_dir/material/host-credentials"
fi
protect_private_material "$set_dir/material"
tar -czf "$set_dir/material.tar.gz" -C "$set_dir/material" . || fail "material archive failed" 1
material_sha=$(checksum "$set_dir/material.tar.gz")
rm -rf -- "$set_dir/material"

manifest="$set_dir/manifest.txt"
{
  echo "format=p14c-target-backup-v1"
  echo "created_at=$created_at"
  echo "host=$PG_HOST"
  echo "port=$PG_PORT"
  echo "services=identity,runtime,scheduler,model"
  for svc in identity runtime scheduler model; do
    db_var=$(service_db_var "$svc"); db="${!db_var}"
    version=$(awk -F= '$1=="schema_version"{print $2}' "$set_dir/postgres/$svc.metadata")
    digest=$(awk -F= '$1=="sha256"{print $2}' "$set_dir/postgres/$svc.metadata")
    echo "pg.$svc.database=$db"
    echo "pg.$svc.schema_version=$version"
    echo "pg.$svc.dump=postgres/$svc.dump"
    echo "pg.$svc.sha256=$digest"
  done
  echo "sqlite.host=sqlite/host.sqlite"
  echo "sqlite.host.user_version=$(awk '{print $2}' "$set_dir/sqlite/host.sqlite.metadata")"
  echo "sqlite.adapter_outbox=sqlite/adapter-outbox.sqlite"
  echo "sqlite.adapter_outbox.user_version=$(awk '{print $2}' "$set_dir/sqlite/adapter-outbox.sqlite.metadata")"
  if [[ -f "$set_dir/sqlite/adapter-actions.sqlite" ]]; then
    echo "sqlite.adapter_actions=sqlite/adapter-actions.sqlite"
    echo "sqlite.adapter_actions.user_version=$(awk '{print $2}' "$set_dir/sqlite/adapter-actions.sqlite.metadata")"
  fi
  echo "material=material.tar.gz"
  echo "material.sha256=$material_sha"
} > "$manifest"
[[ -s "$manifest" ]] || fail "manifest write failed" 1
trap - EXIT

# Retain only complete sets; an interrupted/failed set has no manifest and is never counted.
sets=()
while IFS= read -r -d '' entry; do sets+=("$entry"); done < <(find "$output" -maxdepth 1 -type d -name 'target-*' -print0 | sort -z)
complete=()
for entry in "${sets[@]}"; do [[ -f "$entry/manifest.txt" ]] && complete+=("$entry"); done
total=${#complete[@]}
if ((total > retain)); then
  for ((i=0; i<total-retain; ++i)); do rm -rf -- "${complete[i]}"; done
fi
log "backup complete: $set_dir"
printf '%s\n' "$set_dir"
