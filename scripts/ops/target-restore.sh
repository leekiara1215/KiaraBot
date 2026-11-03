#!/usr/bin/env bash
# P14c target restore: validate a complete backup set first, then restore deployment material,
# SQLite journals and four private PostgreSQL databases. Requires --confirm and stopped services.
# Never migrates, never drops/creates databases, never starts services and never cascades across
# an incomplete set: every required component is checked before the first mutation.
set -euo pipefail
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib.sh
. "$script_dir/lib.sh"

usage() {
  cat >&2 <<'EOF'
usage: target-restore.sh --root DIR --env FILE --input BACKUP_SET --confirm
                         [--expect SERVICE=VERSION ...] [--verify-sql FILE]

  --root DIR      deployment material root to restore into
  --env FILE      trusted operator env file (same contract as target-backup.sh)
  --input DIR     complete backup set produced by target-backup.sh (contains manifest.txt)
  --confirm       explicit destructive restore confirmation
  --expect S=V    optional expected schema version per service (identity|runtime|scheduler|model);
                  mismatches abort before/without automatic migration
  --verify-sql F  optional SQL executed against every restored database; must return exactly "ok"

The script restores material (config/tls/secrets/credentials), Host/Adapter SQLite snapshots and
the four PG dumps in manifest order. A missing/corrupt component aborts before any mutation.
If a step fails after mutation begins, the script exits non-zero and states that a partial restore
may exist; keep all services stopped and follow doc/08 recovery guidance. It never runs a migration.
EOF
}

root="" env_file="" input="" confirm=0 verify_sql=""
expect_value() { eval "printf '%s' \"\${expect_$1:-}\""; }
backup_value() { eval "printf '%s' \"\${backup_$1:-}\""; }
while (($# > 0)); do
  case "$1" in
    --root) [[ $# -ge 2 ]] || { usage; exit 2; }; root="$2"; shift 2 ;;
    --env) [[ $# -ge 2 ]] || { usage; exit 2; }; env_file="$2"; shift 2 ;;
    --input) [[ $# -ge 2 ]] || { usage; exit 2; }; input="$2"; shift 2 ;;
    --confirm) confirm=1; shift ;;
    --expect) [[ $# -ge 2 ]] || { usage; exit 2; };
      [[ "$2" == *=* ]] || { usage; exit 2; }
      case "${2%%=*}" in identity|runtime|scheduler|model) eval "expect_${2%%=*}=\"${2#*=}\"" ;; *) fail "--expect service must be identity|runtime|scheduler|model" 2 ;; esac
      shift 2 ;;
    --verify-sql) [[ $# -ge 2 ]] || { usage; exit 2; }; verify_sql="$2"; shift 2 ;;
    --help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done
[[ -n "$root" && -n "$env_file" && -n "$input" ]] || { usage; exit 2; }
((confirm == 1)) || fail "restore requires --confirm; run is destructive" 2
[[ -f "$env_file" && ! -L "$env_file" ]] || fail "env file missing or symlink" 1
[[ -d "$input" && ! -L "$input" && -f "$input/manifest.txt" ]] || fail "backup set or manifest missing" 1
# Trusted operator-provided file.
# shellcheck disable=SC1090
. "$env_file"
: "${PG_HOST:?}" "${PG_PORT:?}" "${PG_CA_FILE:?}"
PG_BIN="${PG_BIN:-$(dirname "$(command -v psql)")}"; export PG_BIN
[[ -d "$PG_BIN" ]] || fail "PG_BIN missing" 1
for cmd in psql pg_restore pg_dump; do [[ -x "$PG_BIN/$cmd" ]] || fail "missing $PG_BIN/$cmd" 1; done
for cmd in sqlite3 tar; do need_cmd "$cmd"; done
: "${HOST_SQLITE_PATH:?}" "${ADAPTER_OUTBOX_PATH:?}"
[[ -f "$PG_CA_FILE" ]] || fail "PG_CA_FILE missing" 1
[[ -d "$root" && ! -L "$root" ]] || fail "restore root missing or symlink" 1
if [[ -n "$verify_sql" ]]; then [[ -f "$verify_sql" && ! -L "$verify_sql" ]] || fail "verify SQL missing" 1; fi

value() { awk -F= -v k="$1" '$1==k {print substr($0, length(k)+2); exit}' "$input/manifest.txt"; }
[[ "$(value format)" == "p14c-target-backup-v1" ]] || fail "unsupported backup format" 1
[[ "$(value services)" == "identity,runtime,scheduler,model" ]] || fail "backup service set mismatch" 1

# --- preflight: prove the whole set is present and intact before touching live state -----------
material_rel=$(value material); material_sha=$(value material.sha256)
[[ -n "$material_rel" && -n "$material_sha" ]] || fail "manifest material entry missing" 1
[[ -f "$input/$material_rel" && ! -L "$input/$material_rel" ]] || fail "material archive missing" 1
[[ "$(checksum "$input/$material_rel")" == "$material_sha" ]] || fail "material checksum mismatch" 1
if tar -tzf "$input/$material_rel" | awk 'BEGIN{bad=0} /^\// || /(^|\/)\.\.\//{bad=1} END{exit bad}'; then :; else fail "material archive contains unsafe paths" 1; fi

for svc in identity runtime scheduler model; do
  db=$(value "pg.$svc.database"); version=$(value "pg.$svc.schema_version")
  rel=$(value "pg.$svc.dump"); digest=$(value "pg.$svc.sha256")
  [[ -n "$db" && -n "$version" && -n "$rel" && -n "$digest" ]] || fail "manifest pg.$svc incomplete" 1
  [[ "$version" =~ ^[1-9][0-9]*$ ]] || fail "manifest pg.$svc invalid version" 1
  [[ -f "$input/$rel" && ! -L "$input/$rel" ]] || fail "pg.$svc dump missing" 1
  [[ "$(checksum "$input/$rel")" == "$digest" ]] || fail "pg.$svc dump checksum mismatch" 1
  # Older sets made with --no-privileges cannot restore the runtime role's grants.
  "$PG_BIN/pg_restore" --list "$input/$rel" | awk '/ ACL - SCHEMA kiarabot / {found=1} END {exit !found}' \
    || fail "$svc dump lacks kiarabot grants; use a fresh ACL-preserving backup or recover grants offline with the DBA" 1
  printf -v "backup_$svc" '%s' "$input/$rel"
done
for pair in "host:sqlite.host:${HOST_SQLITE_PATH}" "adapter-outbox:sqlite.adapter_outbox:${ADAPTER_OUTBOX_PATH}" \
            "adapter-actions:sqlite.adapter_actions:${ADAPTER_ACTIONS_PATH:-}"; do
  label=${pair%%:*}; rest=${pair#*:}; key=${rest%%:*}; target=${rest#*:}
  rel=$(value "$key")
  [[ -n "$rel" ]] || continue  # adapter-actions is optional only if absent from the manifest
  [[ -n "$target" ]] || fail "env does not define target path for $label" 1
  [[ -f "$input/$rel" && ! -L "$input/$rel" ]] || fail "$label backup missing" 1
  [[ -f "$input/$rel.metadata" ]] || fail "$label metadata missing" 1
  expected_sha=$(awk '{print $1}' "$input/$rel.metadata")
  [[ "$(checksum "$input/$rel")" == "$expected_sha" ]] || fail "$label checksum mismatch" 1
  [[ "$(sqlite3 "$input/$rel" 'PRAGMA integrity_check;')" == "ok" ]] || fail "$label backup integrity failed" 1
  [[ "$(sqlite3 "$input/$rel" 'PRAGMA user_version;')" == "$(value "$key.user_version")" ]] || fail "$label user_version mismatch" 1
done
for svc in identity runtime scheduler model; do
  wanted=$(expect_value "$svc")
  if [[ -n "$wanted" ]]; then
    [[ "$wanted" == "$(value "pg.$svc.schema_version")" ]] || fail "expected $svc schema $wanted but backup has $(value "pg.$svc.schema_version")" 1
  fi
done

# Offline guard after preflight, before mutation. Any active client means a writer may be alive.
for svc in identity runtime scheduler model; do
  db=$(value "pg.$svc.database")
  active=$(other_connections "$svc" "$db" 2>/dev/null) \
    || fail "cannot inspect $svc database; is it available?" 1
  [[ "$active" == "0" ]] || fail "$svc database still has $active client connection(s); stop writers and retry" 1
done
for target in "$HOST_SQLITE_PATH" "$ADAPTER_OUTBOX_PATH" ${ADAPTER_ACTIONS_PATH:+"$ADAPTER_ACTIONS_PATH"}; do
  [[ ! -e "$target-wal" && ! -e "$target-shm" ]] || fail "SQLite sidecars present for $(basename "$target"); stop its users" 1
done

log "preflight passed; restoring backup set $input"
trap 'log "RESTORE FAILED after mutation began: a partial restore may exist; keep services stopped and follow doc/08 before retrying"; exit 1' ERR

# --- material first (operator-owned files; no service is running) -------------------------------
stage=$(mktemp -d "$root/.p14c-restore.XXXXXX")
trap 'rm -rf -- "$stage"; log "RESTORE FAILED: partial restore may exist; keep services stopped"; exit 1' ERR
tar -xpzf "$input/$material_rel" -C "$stage"
[[ -f "$stage/tls/ca.pem" && -f "$stage/secrets/model-root-key.hex" ]] || fail "material archive incomplete" 1
mkdir -p -- "$root/config" "$root/tls" "$root/secrets"
cp -a -- "$stage/config/." "$root/config/"
cp -a -- "$stage/tls/." "$root/tls/"
cp -a -- "$stage/secrets/." "$root/secrets/"
if [[ -d "$stage/host-credentials" ]]; then
  cred_target="${HOST_CREDENTIAL_DIRECTORY:-$root/credentials/host}"
  mkdir -p -- "$cred_target"
  cp -a -- "$stage/host-credentials/." "$cred_target/"
fi
protect_private_material "$root"
log "material restored (config/tls/secrets/credentials)"

# --- SQLite journals ---------------------------------------------------------------------------
restore_sqlite() {
  local label="$1" rel="$2" target="$3" version
  cp -- "$input/$rel" "$target.p14c-restore.$$"
  version=$(sqlite3 "$target.p14c-restore.$$" 'PRAGMA user_version;')
  [[ "$(sqlite3 "$target.p14c-restore.$$" 'PRAGMA integrity_check;')" == "ok" ]] || fail "$label restore integrity failed" 1
  mv -f -- "$target.p14c-restore.$$" "$target"
  log "$label restored user_version=$version"
}
restore_sqlite "host journal" "$(value sqlite.host)" "$HOST_SQLITE_PATH"
restore_sqlite "adapter outbox" "$(value sqlite.adapter_outbox)" "$ADAPTER_OUTBOX_PATH"
if [[ -n "$(value sqlite.adapter_actions)" ]]; then
  restore_sqlite "adapter actions" "$(value sqlite.adapter_actions)" "$ADAPTER_ACTIONS_PATH"
fi

# --- PostgreSQL: one explicit database at a time; no cross-service cascade ----------------------
for svc in identity runtime scheduler model; do
  db=$(value "pg.$svc.database")
  restore_env "$svc" --clean --if-exists --no-owner --exit-on-error \
    --dbname="$db" "$(backup_value "$svc")" || fail "$svc pg_restore failed" 1
  actual=$(psql_run "$svc" "$db" "SELECT version FROM kiarabot.schema_versions WHERE singleton;") \
    || fail "$svc schema version read failed" 1
  expected=$(value "pg.$svc.schema_version")
  [[ "$actual" == "$expected" ]] || fail "$svc restored schema version $actual != backup $expected; migration is not automatic" 1
  relations=$(psql_run "$svc" "$db" "SELECT count(*) FROM information_schema.tables WHERE table_schema='kiarabot';") \
    || fail "$svc relation check failed" 1
  ((relations > 0)) || fail "$svc restored schema has no relations" 1
  if [[ -n "$verify_sql" ]]; then
    result=$(psql_run "$svc" "$db" "$(cat "$verify_sql")") || fail "$svc verify SQL failed" 1
    [[ "$result" == "ok" ]] || fail "$svc verify SQL returned '$result' (expected ok)" 1
  fi
  log "pg_restore $svc schema=$actual relations=$relations"
done

trap - ERR
rm -rf -- "$stage"
log "restore complete; services remain stopped; start in documented order (doc/08)"
