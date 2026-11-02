#!/usr/bin/env bash
# Fast P14c script checks: shell syntax, help exit codes and mandatory safety gates.
# The real backup/restore/fault matrix runs via target-drill.sh (local PG16.4 + SQLite).
set -euo pipefail
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
for script in "$script_dir/lib.sh" "$script_dir/target-backup.sh" "$script_dir/target-restore.sh" "$script_dir/target-drill.sh"; do
  bash -n "$script"
done
"$script_dir/target-backup.sh" --help >/dev/null 2>&1
"$script_dir/target-restore.sh" --help >/dev/null 2>&1
"$script_dir/target-drill.sh" --help >/dev/null 2>&1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/p14c-ops-test.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
mkdir -p "$tmp/root/config" "$tmp/root/tls" "$tmp/root/secrets"
printf 'host=localhost\nport=1\n' > "$tmp/env"
# Missing --offline is a hard stop even with a plausible-looking root.
set +e
"$script_dir/target-backup.sh" --root "$tmp/root" --env "$tmp/env" --output "$tmp/out" >/dev/null 2>&1
rc=$?
set -e
[[ "$rc" == "2" ]] || { echo "backup without --offline expected exit 2, got $rc" >&2; exit 1; }
# Restore without --confirm is a hard stop and never mutates.
set +e
"$script_dir/target-restore.sh" --root "$tmp/root" --env "$tmp/env" --input "$tmp" >/dev/null 2>&1
rc=$?
set -e
[[ "$rc" == "2" ]] || { echo "restore without --confirm expected exit 2, got $rc" >&2; exit 1; }
echo "P14C OPS SCRIPT CHECKS PASS"
