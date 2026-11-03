#!/usr/bin/env bash
# P14a-2 manual secret rotation helper (file-mount phase; KMS/hot rotation deferred).
# Replaces one deployment secret atomically, keeps a timestamped local backup, enforces
# 0600/non-symlink rules, validates the name against the operator secrets manifest when
# configured, and prints the documented ordered restart plan. It never restarts containers
# automatically and never talks to a secret store.
set -euo pipefail
export LC_ALL=C

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
umask 077

usage() {
  cat >&2 <<'USAGE'
usage: target-rotate-secret.sh --env FILE --name NAME --value-file FILE [options]

  --env FILE          trusted operator deploy/target.env (0600, sourced for paths)
  --name NAME         secret name from KIARABOT_SECRETS_MANIFEST (required)
  --value-file FILE   new secret value file (regular, non-symlink, 8..4096 bytes)
  --secrets-dir DIR   override KIARABOT_SECRETS_DIR
  --manifest FILE     override KIARABOT_SECRETS_MANIFEST
  --retain N          keep newest N rotation backups (default 5)
  --confirm           actually replace; without it only prints the plan
  --dry-run           print plan and exit 0 without confirmation requirement
  --help              show this help

Rotation is manual: run a P14c offline target backup first, rotate, then restart in the
printed order (identity -> runtime/scheduler/model -> plugin-host -> adapter -> control ->
panel), verify the new credential works and the old one is rejected, then keep the backup
until the observation window ends. There is no KMS/hot rotation in this phase.
USAGE
}

env_file="" name="" value_file="" secrets_dir="" manifest="" retain=5 confirm=0 dry_run=0
while (($# > 0)); do
  case "$1" in
    --env) [[ $# -ge 2 ]] || { usage; exit 2; }; env_file="$2"; shift 2 ;;
    --name) [[ $# -ge 2 ]] || { usage; exit 2; }; name="$2"; shift 2 ;;
    --value-file) [[ $# -ge 2 ]] || { usage; exit 2; }; value_file="$2"; shift 2 ;;
    --secrets-dir) [[ $# -ge 2 ]] || { usage; exit 2; }; secrets_dir="$2"; shift 2 ;;
    --manifest) [[ $# -ge 2 ]] || { usage; exit 2; }; manifest="$2"; shift 2 ;;
    --retain) [[ $# -ge 2 ]] || { usage; exit 2; }; retain="$2"; shift 2 ;;
    --confirm) confirm=1; shift ;;
    --dry-run) dry_run=1; shift ;;
    --help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done
[[ -n "$env_file" && -n "$name" && -n "$value_file" ]] || { usage; exit 2; }
[[ -f "$env_file" && ! -L "$env_file" ]] || { echo "target-rotate-secret: env file missing or symlink" >&2; exit 1; }
# Trusted operator file; it exports material paths only.
# shellcheck disable=SC1090
. "$env_file"
secrets_dir="${secrets_dir:-${KIARABOT_SECRETS_DIR:-$repo_root/deploy/target/secrets}}"
manifest="${manifest:-${KIARABOT_SECRETS_MANIFEST:-$repo_root/deploy/secrets.manifest.example}}"
[[ "$name" =~ ^[a-z0-9][a-z0-9-]{0,63}$ ]] || { echo "target-rotate-secret: invalid secret name" >&2; exit 2; }
[[ "$retain" =~ ^[0-9]+$ ]] || { echo "target-rotate-secret: retain must be numeric" >&2; exit 2; }
[[ -f "$value_file" && ! -L "$value_file" ]] || { echo "target-rotate-secret: value file missing or symlink" >&2; exit 1; }
[[ -f "$manifest" && ! -L "$manifest" ]] || { echo "target-rotate-secret: secrets manifest missing or symlink" >&2; exit 1; }
entry=$(awk -F'|' -v name="$name" '$1==name && $1 !~ /^#/ {print; exit}' "$manifest")
[[ -n "$entry" ]] || { echo "target-rotate-secret: secret not listed in manifest" >&2; exit 1; }
relative=$(printf '%s' "$entry" | awk -F'|' '{print $2}')
mode=$(printf '%s' "$entry" | awk -F'|' '{print $3}')
order=$(printf '%s' "$entry" | awk -F'|' '{print $5}')
[[ "$relative" =~ ^[A-Za-z0-9._/-]+$ && "$relative" != /* && "$relative" != *..* ]] \
  || { echo "target-rotate-secret: unsafe manifest path" >&2; exit 1; }
[[ "$mode" == "0600" ]] || { echo "target-rotate-secret: only 0600 secrets are supported" >&2; exit 1; }
target="$secrets_dir/$relative"
[[ -f "$target" && ! -L "$target" ]] || { echo "target-rotate-secret: target secret missing or symlink" >&2; exit 1; }
size=$(wc -c < "$value_file" | tr -d ' ')
((size >= 8 && size <= 4096)) || { echo "target-rotate-secret: value must be 8..4096 bytes" >&2; exit 1; }

cat >&2 <<PLAN
target-rotate-secret: secret=$name target=$(basename "$target") manifest=$(basename "$manifest")
target-rotate-secret: before rotating run an offline P14c backup:
  scripts/ops/target-backup.sh --root <material-root> --env <p14c env> --output <backups> --offline
target-rotate-secret: planned restart order after replacement: ${order:-identity}
  docker compose --env-file deploy/target.env -f deploy/compose.target.yml --profile exec \\
    up -d --force-recreate --no-deps ${order:-identity}   # then recreate affected dependants in identity->runtime/scheduler/model->plugin-host->adapter->control->panel order
target-rotate-secret: verify new credential works and old credential is rejected; keep the
  rotation backup and previous P14c backup until the observation window ends. KMS/hot rotation
  is deferred.
PLAN
if ((dry_run)); then exit 0; fi
((confirm == 1)) || { echo "target-rotate-secret: rerun with --confirm to replace the file" >&2; exit 2; }

stamp=$(date -u +%Y%m%dT%H%M%SZ)
backup="$target.rotate-$stamp.bak"
cp -- "$target" "$backup"
chmod 600 "$backup"
tmp=$(mktemp "$target.rotate.XXXXXXXX")
trap 'rm -f -- "$tmp"' EXIT
# Single-file mounts inherit host ownership; do not silently replace a service-owned
# credential with one owned by the operator running this script.
cp -p -- "$target" "$tmp"
owner() { stat -c '%u:%g' "$1" 2>/dev/null || stat -f '%u:%g' "$1"; }
[[ "$(owner "$tmp")" == "$(owner "$target")" ]] || { echo "target-rotate-secret: could not preserve service owner" >&2; exit 1; }
cat -- "$value_file" > "$tmp"
chmod 600 "$tmp"
mv -f -- "$tmp" "$target"
trap - EXIT
chmod 600 "$target"
[[ "$(stat -c '%a' "$target" 2>/dev/null || stat -f '%Lp' "$target")" == "600" ]] \
  || { echo "target-rotate-secret: rotated secret mode is not 600" >&2; exit 1; }

# Keep rotation backups bounded (0055 suffix glob is chronological in C locale).
shopt -s nullglob
backups=("$target".rotate-*.bak)
shopt -u nullglob
if ((${#backups[@]} > retain)); then
  for ((i=0; i<${#backups[@]}-retain; ++i)); do rm -f -- "${backups[i]}"; done
fi
echo "target-rotate-secret: replaced $name; backup=$(basename "$backup")" >&2
echo "target-rotate-secret: restart in order and verify old/new credentials per doc/08" >&2
