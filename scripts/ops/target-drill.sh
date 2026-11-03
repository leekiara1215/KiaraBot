#!/usr/bin/env bash
# P14c local real drill: owned PostgreSQL 16.4 cluster + four service databases + Host/Adapter
# SQLite + deployment material. Runs backup/restore and failure cases; local single-host only.
# It never touches the repository or an external deployment; all state lives under mktemp.
set -euo pipefail
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib.sh
. "$script_dir/lib.sh"

usage() {
  cat >&2 <<'EOF'
usage: target-drill.sh [--keep]

Requires:
  PG16_BIN        PostgreSQL 16.4 bin/ (default: /Users/betta/Workspace/KiaraBot/build/pg-16.4-install/bin)
  KIARABOT_PG_INITIALIZE  compiled kiarabot-pg-initialize
                  (default: /Users/betta/Workspace/KiaraBot/build/full-locked/infrastructure/postgres/kiarabot-pg-initialize)
  OPENSSL_BIN     openssl binary (default: build/openssl-3.5.0-install/bin/openssl, else PATH)
  sqlite3, tar, python3

Creates a temporary local PG cluster with trust auth, SSL and four initialized databases; seeds
one representative audit row per database plus Host journal and Adapter outbox/actions SQLite;
backs up; destroys data/material; restores; verifies schema versions/relations/rows and material;
then runs failure drills (active writer, missing root key, corrupt Host journal, single-database
loss, partial backup set). Prints RPO/RTO measured locally. No Docker, no remote host.
EOF
}
keep=0
case "${1:-}" in
  --keep) keep=1 ;;
  --help) usage; exit 0 ;;
  "") ;;
  *) usage; exit 2 ;;
esac
PGBIN="${PG16_BIN:-/Users/betta/Workspace/KiaraBot/build/pg-16.4-install/bin}"
INITIALIZER="${KIARABOT_PG_INITIALIZE:-/Users/betta/Workspace/KiaraBot/build/full-locked/infrastructure/postgres/kiarabot-pg-initialize}"
OPENSSL="${OPENSSL_BIN:-/Users/betta/Workspace/KiaraBot/build/openssl-3.5.0-install/bin/openssl}"
[[ -x "$OPENSSL" ]] || OPENSSL=$(command -v openssl)
for cmd in "$PGBIN/initdb" "$PGBIN/pg_ctl" "$PGBIN/psql" "$INITIALIZER" "$OPENSSL" sqlite3 python3 tar; do
  [[ -x "$cmd" ]] || [[ "$cmd" == "sqlite3" && -n "$(command -v sqlite3)" ]] || [[ "$cmd" == "python3" && -n "$(command -v python3)" ]] || [[ "$cmd" == "tar" && -n "$(command -v tar)" ]] || fail "missing drill dependency: $cmd" 1
done
WORK=$(mktemp -d "${TMPDIR:-/tmp}/kiarabot-p14c-drill.XXXXXX")
PGDATA="$WORK/pg/data"
PORT=$(python3 - <<'PY'
import socket
s = socket.socket(); s.bind(('127.0.0.1', 0)); print(s.getsockname()[1]); s.close()
PY
)
ROOT="$WORK/root"; BACKUP_ROOT="$WORK/backups"
mkdir -p "$PGDATA" "$ROOT/config" "$ROOT/tls" "$ROOT/secrets" "$ROOT/credentials/host" "$ROOT/data/host" "$ROOT/data/adapter" "$BACKUP_ROOT"
export PATH="$PGBIN:$PATH"

pg_started=0
cleanup() {
  if ((pg_started)); then "$PGBIN/pg_ctl" -D "$PGDATA" stop -m immediate >/dev/null 2>&1 || true; fi
  if ((keep)); then log "drill workspace kept: $WORK"; else rm -rf -- "$WORK"; fi
}
trap cleanup EXIT
log "drill workspace=$WORK pg_port=$PORT"

ADMIN_PASS=$(python3 -c 'import secrets; print(secrets.token_hex(24))')
printf '%s\n' "$ADMIN_PASS" > "$WORK/admin.pass"; chmod 600 "$WORK/admin.pass"
"$PGBIN/initdb" -D "$PGDATA" -A scram-sha-256 -U p14c_admin --pwfile "$WORK/admin.pass" \
  --no-locale --encoding=UTF8 >/dev/null
"$OPENSSL" req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 \
  -subj /CN=localhost -addext "subjectAltName=DNS:localhost" \
  -keyout "$ROOT/tls/server.key" -out "$ROOT/tls/server.pem" >/dev/null 2>&1
chmod 600 "$ROOT/tls/server.key"
printf "hostssl all all 127.0.0.1/32 scram-sha-256\nhostnossl all all 127.0.0.1/32 reject\n" \
  > "$PGDATA/pg_hba.conf"
cat >> "$PGDATA/postgresql.conf" <<CONF
listen_addresses='127.0.0.1'
port=$PORT
ssl=on
ssl_cert_file='$ROOT/tls/server.pem'
ssl_key_file='$ROOT/tls/server.key'
unix_socket_directories='$ROOT'
CONF
"$PGBIN/pg_ctl" -D "$PGDATA" -l "$WORK/pg.log" -w -t 20 start >/dev/null
pg_started=1
PGPASSWORD="$ADMIN_PASS" PGSSLMODE=verify-full PGSSLROOTCERT="$ROOT/tls/server.pem" \
  "$PGBIN/psql" -X -q -h localhost -p "$PORT" -U p14c_admin -d postgres -v ON_ERROR_STOP=1 <<SQL >/dev/null
CREATE ROLE drill_identity_migration LOGIN PASSWORD 'identity-mig-pass';
CREATE ROLE drill_identity_runtime LOGIN PASSWORD 'identity-run-pass';
CREATE DATABASE drill_identity OWNER drill_identity_migration;
REVOKE ALL ON DATABASE drill_identity FROM PUBLIC;
GRANT CONNECT ON DATABASE drill_identity TO drill_identity_runtime;
CREATE ROLE drill_runtime_migration LOGIN PASSWORD 'runtime-mig-pass';
CREATE ROLE drill_runtime_runtime LOGIN PASSWORD 'runtime-run-pass';
CREATE DATABASE drill_runtime OWNER drill_runtime_migration;
REVOKE ALL ON DATABASE drill_runtime FROM PUBLIC;
GRANT CONNECT ON DATABASE drill_runtime TO drill_runtime_runtime;
CREATE ROLE drill_scheduler_migration LOGIN PASSWORD 'scheduler-mig-pass';
CREATE ROLE drill_scheduler_runtime LOGIN PASSWORD 'scheduler-run-pass';
CREATE DATABASE drill_scheduler OWNER drill_scheduler_migration;
REVOKE ALL ON DATABASE drill_scheduler FROM PUBLIC;
GRANT CONNECT ON DATABASE drill_scheduler TO drill_scheduler_runtime;
CREATE ROLE drill_model_migration LOGIN PASSWORD 'model-mig-pass';
CREATE ROLE drill_model_runtime LOGIN PASSWORD 'model-run-pass';
CREATE DATABASE drill_model OWNER drill_model_migration;
REVOKE ALL ON DATABASE drill_model FROM PUBLIC;
GRANT CONNECT ON DATABASE drill_model TO drill_model_runtime;
SQL

# Deployment material: CA, per-service certs, root key and operator credentials.
cp -- "$ROOT/tls/server.pem" "$ROOT/tls/ca.pem"
# Stable connection CA kept outside the restored material root: a total CA loss on the host must
# not prevent the operator from connecting to PG to restore the material that contains the CA.
cp -- "$ROOT/tls/server.pem" "$WORK/pg-ca.pem"
cp -- "$ROOT/tls/server.key" "$ROOT/tls/ca.key"
for svc in identity runtime scheduler model host; do
  mkdir -p "$ROOT/tls/$svc"
  cp -- "$ROOT/tls/server.pem" "$ROOT/tls/$svc/server.pem"
  cp -- "$ROOT/tls/server.key" "$ROOT/tls/$svc/server.key"
done
printf '{"service":"identity","drill":true}\n' > "$ROOT/config/identity.json"
printf '{"service":"runtime","drill":true}\n' > "$ROOT/config/runtime.json"
printf '{"service":"scheduler","drill":true}\n' > "$ROOT/config/scheduler.json"
printf '{"service":"model","drill":true}\n' > "$ROOT/config/model.json"
printf '%s\n' "$(python3 -c 'import secrets; print(secrets.token_hex(32))')" > "$ROOT/secrets/model-root-key.hex"
printf '%s\n' "$(python3 -c 'import secrets; print(secrets.token_hex(32))')" > "$ROOT/secrets/adapter.bootstrap"
printf '%s\n' "$(python3 -c 'import secrets; print(secrets.token_hex(32))')" > "$ROOT/secrets/identity-verification-key.pem"
for svc in identity runtime scheduler model; do
  printf '%s\n' "${svc}-mig-pass" > "$ROOT/secrets/$svc-migration.pass"
done
printf 'drill-credential\n' > "$ROOT/credentials/host/credential.secret"
chmod 600 "$ROOT/secrets/"* "$ROOT/credentials/host/credential.secret"
export PG_HOST=localhost PG_PORT="$PORT" PG_CA_FILE="$WORK/pg-ca.pem" PG_BIN="$PGBIN"
export HOST_SQLITE_PATH="$ROOT/data/host/host.sqlite"        ADAPTER_OUTBOX_PATH="$ROOT/data/adapter/outbox.db"        ADAPTER_ACTIONS_PATH="$ROOT/data/adapter/actions.db"        HOST_CREDENTIAL_DIRECTORY="$ROOT/credentials/host"
for svc in identity runtime scheduler model; do
  upper=$(printf '%s' "$svc" | tr '[:lower:]' '[:upper:]')
  eval "export ${upper}_DB=drill_${svc}"
  eval "export ${upper}_MIGRATION_ROLE=drill_${svc}_migration"
  eval "export ${upper}_MIGRATION_PASSWORD_FILE=$ROOT/secrets/${svc}-migration.pass"
done

expected_version() {
  case "$1" in identity) echo 15 ;; runtime) echo 17 ;; scheduler) echo 10 ;; model) echo 9 ;; *) echo 0 ;; esac
}
database_name() {
  case "$1" in identity) echo drill_identity ;; runtime) echo drill_runtime ;; scheduler) echo drill_scheduler ;; model) echo drill_model ;; esac
}
for svc in identity runtime scheduler model; do
  "$INITIALIZER" --initialize-empty "$svc" localhost "$PORT" "$(database_name "$svc")" \
    "drill_${svc}_migration" "drill_${svc}_runtime" "$ROOT/tls/ca.pem" \
    "$ROOT/secrets/$svc-migration.pass" >/dev/null || fail "initializer failed for $svc" 1
  version=$(psql_run "$svc" "$(database_name "$svc")" "SELECT version FROM kiarabot.schema_versions WHERE singleton;")
  [[ "$version" == "$(expected_version "$svc")" ]] || fail "$svc initialized at unexpected version $version" 1
  psql_run "$svc" "$(database_name "$svc")" \
    "INSERT INTO kiarabot.audit_entries(actor,resource_kind,resource_id,request_id,trace_id,method,authorization_version,occurred_at) VALUES ('p14c-drill-$svc',0,'drill-resource','drill-request','drill-trace','drill.method','1',(extract(epoch from clock_timestamp())*1000000000)::bigint);" >/dev/null \
    || fail "seed row failed for $svc" 1
done

seed_sqlite() {
  local path="$1" version="$2" table="$3" sql="$4"
  rm -f "$path" "$path-wal" "$path-shm"
  sqlite3 "$path" "PRAGMA journal_mode=WAL; PRAGMA user_version=$version; CREATE TABLE $table(value TEXT NOT NULL); INSERT INTO $table(value) VALUES('drill');" >/dev/null
  [[ "$(sqlite3 "$path" 'PRAGMA integrity_check;')" == "ok" ]] || fail "SQLite seed failed: $path" 1
}
seed_sqlite "$ROOT/data/host/host.sqlite" 3 journal "INSERT INTO journal(value) VALUES('seed-host');"
seed_sqlite "$ROOT/data/adapter/outbox.db" 2 outbox "INSERT INTO outbox(value) VALUES('seed-outbox');"
seed_sqlite "$ROOT/data/adapter/actions.db" 3 actions "INSERT INTO actions(value) VALUES('seed-action');"

cat > "$WORK/target.env" <<ENV
PG_HOST=localhost
PG_PORT=$PORT
PG_CA_FILE=$WORK/pg-ca.pem
PG_BIN=$PGBIN
IDENTITY_DB=drill_identity
IDENTITY_MIGRATION_ROLE=drill_identity_migration
IDENTITY_MIGRATION_PASSWORD_FILE=$ROOT/secrets/identity-migration.pass
RUNTIME_DB=drill_runtime
RUNTIME_MIGRATION_ROLE=drill_runtime_migration
RUNTIME_MIGRATION_PASSWORD_FILE=$ROOT/secrets/runtime-migration.pass
SCHEDULER_DB=drill_scheduler
SCHEDULER_MIGRATION_ROLE=drill_scheduler_migration
SCHEDULER_MIGRATION_PASSWORD_FILE=$ROOT/secrets/scheduler-migration.pass
MODEL_DB=drill_model
MODEL_MIGRATION_ROLE=drill_model_migration
MODEL_MIGRATION_PASSWORD_FILE=$ROOT/secrets/model-migration.pass
HOST_SQLITE_PATH=$ROOT/data/host/host.sqlite
ADAPTER_OUTBOX_PATH=$ROOT/data/adapter/outbox.db
ADAPTER_ACTIONS_PATH=$ROOT/data/adapter/actions.db
HOST_CREDENTIAL_DIRECTORY=$ROOT/credentials/host
ENV
chmod 600 "$WORK/target.env"
# shellcheck disable=SC1090
. "$WORK/target.env"

verify_all() {
  local stage="$1" svc version relations rows
  for svc in identity runtime scheduler model; do
    version=$(psql_run "$svc" "$(database_name "$svc")" "SELECT version FROM kiarabot.schema_versions WHERE singleton;")
    [[ "$version" == "$(expected_version "$svc")" ]] || fail "$stage: $svc schema $version != $(expected_version "$svc")" 1
    relations=$(psql_run "$svc" "$(database_name "$svc")" "SELECT count(*) FROM information_schema.tables WHERE table_schema='kiarabot';")
    ((relations >= 20)) || fail "$stage: $svc relation count $relations too small" 1
    rows=$(PGPASSWORD="${svc}-run-pass" PGSSLMODE=verify-full PGSSLROOTCERT="$PG_CA_FILE" \
      "$PGBIN/psql" -XqAt -v ON_ERROR_STOP=1 -h localhost -p "$PORT" -U "drill_${svc}_runtime" \
      -d "$(database_name "$svc")" -c "SELECT count(*) FROM kiarabot.audit_entries WHERE actor='p14c-drill-$svc';") \
      || fail "$stage: runtime role lost access to $svc data" 1
    [[ "$rows" == "1" ]] || fail "$stage: $svc representative row missing ($rows)" 1
  done
  for item in "host:$ROOT/data/host/host.sqlite:3:journal" "adapter:$ROOT/data/adapter/outbox.db:2:outbox" "actions:$ROOT/data/adapter/actions.db:3:actions"; do
    IFS=: read -r label path version table <<< "$item"
    [[ "$(sqlite3 "$path" 'PRAGMA integrity_check;')" == "ok" ]] || fail "$stage: $label SQLite corrupt" 1
    [[ "$(sqlite3 "$path" 'PRAGMA user_version;')" == "$version" ]] || fail "$stage: $label user_version mismatch" 1
    [[ "$(sqlite3 "$path" "SELECT count(*) FROM $table;")" == "1" ]] || fail "$stage: $label row missing" 1
  done
  [[ -s "$ROOT/secrets/model-root-key.hex" && -s "$ROOT/tls/ca.pem" ]] || fail "$stage: material missing" 1
  [[ "$(stat -c '%a' "$ROOT/secrets/model-root-key.hex" 2>/dev/null || stat -f '%Lp' "$ROOT/secrets/model-root-key.hex")" == "600" ]] || fail "$stage: root key permissions not 600" 1
  log "$stage verification passed"
}

backup() {
  "$script_dir/target-backup.sh" --root "$ROOT" --env "$WORK/target.env" \
    --output "$BACKUP_ROOT" --offline --retain 5
}
restore() {
  local input="$1"
  "$script_dir/target-restore.sh" --root "$ROOT" --env "$WORK/target.env" \
    --input "$input" --confirm --verify-sql "$WORK/verify.sql"
}
printf "SELECT CASE WHEN (SELECT count(*) FROM kiarabot.schema_versions WHERE singleton)=1 AND (SELECT count(*) FROM information_schema.tables WHERE table_schema='kiarabot')>=20 AND (SELECT count(*) FROM kiarabot.audit_entries WHERE actor LIKE 'p14c-drill-%%')=1 THEN 'ok' ELSE 'bad' END;\n" > "$WORK/verify.sql"

t0=$(date +%s%N)
BACKUP_SET=$(backup | tail -1)
backup_ms=$(( ($(date +%s%N) - t0) / 1000000 ))
[[ -d "$BACKUP_SET" && -f "$BACKUP_SET/manifest.txt" ]] || fail "backup set incomplete" 1
verify_all "post-seed/pre-backup"

# Destroy every restore input: four schemas, both SQLite journals and material.
for svc in identity runtime scheduler model; do
  PGPASSWORD="$(cat "$ROOT/secrets/$svc-migration.pass")" PGSSLMODE=verify-full \
    PGSSLROOTCERT="$ROOT/tls/ca.pem" "$PGBIN/psql" -X -q -h localhost -p "$PORT" \
    -U "drill_${svc}_migration" -d "$(database_name "$svc")" -c 'DROP SCHEMA kiarabot CASCADE;' >/dev/null
done
rm -f "$ROOT/data/host/host.sqlite" "$ROOT/data/host/host.sqlite-wal" "$ROOT/data/host/host.sqlite-shm"
rm -f "$ROOT/data/adapter/outbox.db"* "$ROOT/data/adapter/actions.db"*
rm -f "$ROOT/secrets/model-root-key.hex" "$ROOT/tls/ca.pem" "$ROOT/config/identity.json"
rm -rf "$ROOT/credentials/host"
log "destroyed all four schemas, Host/Adapter SQLite and a subset of material"
t0=$(date +%s%N)
restore "$BACKUP_SET" >/dev/null
rto_ms=$(( ($(date +%s%N) - t0) / 1000000 ))
verify_all "post-restore"
log "main restore passed: RPO=0s (quiesced backup; no writer accepted after --offline), RTO=$((rto_ms/1000)).$((rto_ms%1000))s backup=${backup_ms}ms"

# Credential replacement drill (manual rotation phase): rotate one migration credential, verify the
# old password is rejected, verify the new password is accepted by the same operator scripts, then
# take a fresh full backup. Service-process restart order is documented in doc/08; process daemons
# are not part of this local data-layer drill.
new_pass="identity-rotated-$(python3 -c 'import secrets; print(secrets.token_hex(16))')"
old_pass=$(cat "$ROOT/secrets/identity-migration.pass")
PGPASSWORD="$ADMIN_PASS" PGSSLMODE=verify-full PGSSLROOTCERT="$WORK/pg-ca.pem" \
  "$PGBIN/psql" -X -q -h localhost -p "$PORT" -U p14c_admin -d postgres \
  -c "ALTER ROLE drill_identity_migration PASSWORD '$new_pass';" >/dev/null
printf '%s\n' "$new_pass" > "$ROOT/secrets/identity-migration.pass"; chmod 600 "$ROOT/secrets/identity-migration.pass"
if PGPASSWORD="$old_pass" PGSSLMODE=verify-full PGSSLROOTCERT="$WORK/pg-ca.pem" \
    "$PGBIN/psql" -X -q -h localhost -p "$PORT" -U drill_identity_migration -d drill_identity \
    -c 'SELECT 1;' >/dev/null 2>&1; then fail "old credential still accepted after rotation" 1; fi
psql_run identity "$(database_name identity)" "SELECT 1;" >/dev/null || fail "new credential rejected" 1
BACKUP_SET=$(backup | tail -1)
verify_all "post-credential-rotation"
log "fault/rotation: new identity migration credential accepted, old rejected, fresh backup passed (service restart order documented; no daemons in data-layer drill)"

# Fault 1: an active writer must block backup (offline guard), and no set is published.
PGPASSWORD="runtime-run-pass" PGSSLMODE=verify-full \
  PGSSLROOTCERT="$WORK/pg-ca.pem" "$PGBIN/psql" -X -q -h localhost -p "$PORT" \
  -U drill_runtime_runtime -d drill_runtime -c 'SELECT pg_sleep(6);' >/dev/null 2>&1 &
writer=$!
sleep 1
if backup >"$WORK/backup-active.log" 2>&1; then fail "active-writer backup unexpectedly succeeded" 1; fi
kill "$writer" 2>/dev/null || true; wait "$writer" 2>/dev/null || true
# Wait for the backend to disappear so the next fault starts from a truly quiesced cluster.
for _ in 1 2 3 4 5 6 7 8 9 10; do
  active=$(other_connections runtime "$(database_name runtime)") || true
  [[ "$active" == "0" ]] && break
  sleep 0.5
done
grep -q 'client connection' "$WORK/backup-active.log" || fail "active-writer failure lacked explicit offline message" 1
log "fault: active writer blocked backup with explicit error"

# Fault 2: missing model root key must block backup before a manifest is published.
mv "$ROOT/secrets/model-root-key.hex" "$WORK/root-key.saved"
if backup >"$WORK/backup-nokey.log" 2>&1; then fail "missing-root-key backup unexpectedly succeeded" 1; fi
mv "$WORK/root-key.saved" "$ROOT/secrets/model-root-key.hex"; chmod 600 "$ROOT/secrets/model-root-key.hex"
grep -q 'model-root-key.hex' "$WORK/backup-nokey.log" || fail "missing-root-key error not explicit" 1
log "fault: missing model root key blocked backup explicitly"
# Fault 2b: missing deployment CA material must also block backup explicitly.
mv "$ROOT/tls/ca.pem" "$WORK/ca.pem.saved"
if backup >"$WORK/backup-noca.log" 2>&1; then fail "missing-CA backup unexpectedly succeeded" 1; fi
mv "$WORK/ca.pem.saved" "$ROOT/tls/ca.pem"
grep -q 'ca.pem' "$WORK/backup-noca.log" || fail "missing-CA error not explicit" 1
log "fault: missing deployment CA blocked backup explicitly"

# Fault 2c: one service database stopped/unavailable -> backup must fail instead of silently
# taking a three-database set (single-service stop, no cross-service cascade).
PGPASSWORD="$ADMIN_PASS" PGSSLMODE=verify-full PGSSLROOTCERT="$WORK/pg-ca.pem" \
  "$PGBIN/psql" -X -q -h localhost -p "$PORT" -U p14c_admin -d postgres \
  -c 'ALTER DATABASE drill_scheduler ALLOW_CONNECTIONS false;' >/dev/null
if backup >"$WORK/backup-svc-down.log" 2>&1; then fail "single-service-down backup unexpectedly succeeded" 1; fi
PGPASSWORD="$ADMIN_PASS" PGSSLMODE=verify-full PGSSLROOTCERT="$WORK/pg-ca.pem" \
  "$PGBIN/psql" -X -q -h localhost -p "$PORT" -U p14c_admin -d postgres \
  -c 'ALTER DATABASE drill_scheduler ALLOW_CONNECTIONS true;' >/dev/null
grep -qi 'scheduler' "$WORK/backup-svc-down.log" || fail "single-service-down error not explicit" 1
log "fault: single service database unavailable blocked backup explicitly"


# Fault 3: corrupt Host journal must fail SQLite integrity before publication.
cp -- "$ROOT/data/host/host.sqlite" "$WORK/host.sqlite.saved"
printf 'corrupt-not-a-sqlite-database' > "$ROOT/data/host/host.sqlite"
if backup >"$WORK/backup-corrupt.log" 2>&1; then fail "corrupt-host backup unexpectedly succeeded" 1; fi
cp -- "$WORK/host.sqlite.saved" "$ROOT/data/host/host.sqlite"
grep -qi 'host journal\|integrity' "$WORK/backup-corrupt.log" || fail "corrupt-host error not explicit" 1
log "fault: corrupt Host journal blocked backup explicitly"

# Fault 4: single-database loss -> full restore succeeds and all databases reconcile.
psql_run runtime "$(database_name runtime)" "DROP SCHEMA kiarabot CASCADE;" >/dev/null
restore "$BACKUP_SET" >/dev/null
verify_all "single-db-loss-restore"
log "fault: single database loss restored"

# Fault 5: an incomplete backup set must be rejected before any mutation.
cp -a -- "$BACKUP_SET" "$WORK/partial-set"
rm -f -- "$WORK/partial-set/postgres/runtime.dump"
if restore "$WORK/partial-set" >"$WORK/restore-partial.log" 2>&1; then fail "partial backup restore unexpectedly succeeded" 1; fi
grep -qi 'runtime dump missing\|checksum mismatch\|missing' "$WORK/restore-partial.log" || fail "partial restore error not explicit" 1
verify_all "partial-restore-rejected"
log "fault: partial backup set rejected; live state unchanged"

log "P14C LOCAL DRILL PASS"
log "RPO: 0s (offline/quiesced backup)  RTO: $((rto_ms/1000)).$((rto_ms%1000))s  backup_set=$BACKUP_SET"
