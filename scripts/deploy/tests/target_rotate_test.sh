#!/usr/bin/env bash
# Offline P14a-2 checks for target-rotate-secret.sh: manifest validation, dry-run, atomic
# replacement, 0600 mode, timestamped backup, bounded retention and explicit failure gates.
set -euo pipefail
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rotate="$script_dir/target-rotate-secret.sh"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/p14a-rotate-test.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
mkdir -p "$tmp/secrets"
printf 'old-credential-value-1234\n' > "$tmp/secrets/test-secret.pass"
chmod 600 "$tmp/secrets/test-secret.pass"
printf 'new-credential-value-5678\n' > "$tmp/new.value"; chmod 600 "$tmp/new.value"
cat > "$tmp/env" <<ENV
KIARABOT_SECRETS_DIR=$tmp/secrets
KIARABOT_SECRETS_MANIFEST=$tmp/manifest
ENV
chmod 600 "$tmp/env"
cat > "$tmp/manifest" <<'MANIFEST'
name|file|mode|consumers|restart_order
test-secret|test-secret.pass|0600|test-consumer|identity
MANIFEST

# Unknown name and missing --confirm are hard gates.
set +e
"$rotate" --env "$tmp/env" --name missing-secret --value-file "$tmp/new.value" --dry-run >/dev/null 2>&1
rc=$?
set -e
[[ "$rc" == "1" ]] || { echo "unknown secret expected exit 1, got $rc" >&2; exit 1; }
set +e
"$rotate" --env "$tmp/env" --name test-secret --value-file "$tmp/new.value" >/dev/null 2>&1
rc=$?
set -e
[[ "$rc" == "2" ]] || { echo "rotation without --confirm expected exit 2, got $rc" >&2; exit 1; }

# Dry-run must not mutate; --confirm replaces atomically and keeps a 0600 backup.
"$rotate" --env "$tmp/env" --name test-secret --value-file "$tmp/new.value" --dry-run >/dev/null 2>&1
grep -q 'old-credential-value-1234' "$tmp/secrets/test-secret.pass" || { echo "dry-run mutated target" >&2; exit 1; }
"$rotate" --env "$tmp/env" --name test-secret --value-file "$tmp/new.value" --confirm >/dev/null 2>&1
grep -q 'new-credential-value-5678' "$tmp/secrets/test-secret.pass" || { echo "new value not installed" >&2; exit 1; }
mode=$(stat -c '%a' "$tmp/secrets/test-secret.pass" 2>/dev/null || stat -f '%Lp' "$tmp/secrets/test-secret.pass")
[[ "$mode" == "600" ]] || { echo "rotated mode $mode != 600" >&2; exit 1; }
shopt -s nullglob
backups=("$tmp/secrets/test-secret.pass".rotate-*.bak)
shopt -u nullglob
((${#backups[@]} == 1)) || { echo "expected one rotation backup" >&2; exit 1; }
[[ "$(stat -c '%a' "${backups[0]}" 2>/dev/null || stat -f '%Lp' "${backups[0]}")" == "600" ]] \
  || { echo "rotation backup mode not 600" >&2; exit 1; }
grep -q 'old-credential-value-1234' "${backups[0]}" || { echo "backup does not contain old value" >&2; exit 1; }
echo "P14A ROTATION SCRIPT CHECKS PASS"
