#!/usr/bin/env bash
# Shared helpers for the P14c target backup/restore scripts. Sourced by operators;
# intentionally no dependency on the services, migrations or a live deployment.
set -euo pipefail
export LC_ALL=C

log() { printf '%s p14c-ops %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
fail() { log "ERROR $*"; exit "${2:-1}"; }
need_cmd() { command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1" 1; }
checksum() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}';
  else need_cmd shasum; shasum -a 256 "$1" | awk '{print $1}'; fi
}
mode_private() { chmod 600 "$1"; }
service_db_var() { printf '%s_DB' "$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]')"; }
service_role_var() { printf '%s_MIGRATION_ROLE' "$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]')"; }
service_pass_var() { printf '%s_MIGRATION_PASSWORD_FILE' "$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]')"; }
psql_run() {
  local service="$1" db="$2"; shift 2
  local role_var pass_var role pass
  role_var=$(service_role_var "$service"); pass_var=$(service_pass_var "$service")
  role="${!role_var:-}"; pass="${!pass_var:-}"
  PGPASSWORD="$(cat "$pass")" PGSSLMODE=verify-full PGSSLROOTCERT="$PG_CA_FILE" \
    "$PG_BIN/psql" -X -qAt -v ON_ERROR_STOP=1 -h "$PG_HOST" -p "$PG_PORT" -U "$role" -d "$db" -c "$*"
}
# A migration role cannot see backend_type for sessions owned by the runtime role.
# numbackends counts connections across roles; subtract only our own connection.
other_connections() {
  psql_run "$1" "$2" "SELECT numbackends - 1 FROM pg_stat_database WHERE datname=current_database();"
}
# The CA/leaf certificates may be shared by multiple service UIDs (notably Panel).
# Preserve their deployed read mode. Tighten private material before publishing it.
protect_private_material() {
  find "$1/secrets" -type f -exec chmod 600 {} +
  find "$1/tls" -type f \( -name '*.key' -o -exec grep -qE '^-----BEGIN ([[:alnum:]]+ )*PRIVATE KEY-----' {} \; \) -exec chmod 600 {} +
}
dump_env() {
  local service="$1"; shift
  local role_var pass_var role pass
  role_var=$(service_role_var "$service"); pass_var=$(service_pass_var "$service")
  role="${!role_var:-}"; pass="${!pass_var:-}"
  PGPASSWORD="$(cat "$pass")" PGSSLMODE=verify-full PGSSLROOTCERT="$PG_CA_FILE" \
    "$PG_BIN/pg_dump" -h "$PG_HOST" -p "$PG_PORT" -U "$role" "$@"
}
restore_env() {
  local service="$1"; shift
  local role_var pass_var role pass
  role_var=$(service_role_var "$service"); pass_var=$(service_pass_var "$service")
  role="${!role_var:-}"; pass="${!pass_var:-}"
  PGPASSWORD="$(cat "$pass")" PGSSLMODE=verify-full PGSSLROOTCERT="$PG_CA_FILE" \
    "$PG_BIN/pg_restore" -h "$PG_HOST" -p "$PG_PORT" -U "$role" "$@"
}
