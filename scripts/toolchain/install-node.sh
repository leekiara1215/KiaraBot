#!/usr/bin/env bash
# Install the Node.js version locked in dependencies/architecture/dependencies.lock.json.
#
# This script never touches the system Node/npm: it verifies the official archive
# against the lock's SHA-256 and unpacks it under build/ (ignored), printing the PATH
# export to use. It fails closed when the archive is unavailable/offline or the hash
# does not match; it does not fall back to a Homebrew/system Node.
#
# usage:
#   scripts/toolchain/install-node.sh [--dest DIR] [--archive FILE] [--quiet]
#   scripts/toolchain/install-node.sh --check [--dest DIR]
#
#   --dest DIR     install root (default: build/toolchain/node-v<version>)
#   --archive FILE use an already-downloaded official archive (still hash-verified)
#   --check        only verify the installed node/npm versions at --dest
#   --quiet        suppress progress messages
set -euo pipefail
export LC_ALL=C

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
lock_file="$root/dependencies/architecture/dependencies.lock.json"
dest=""
archive=""
check_only=0
quiet=0

usage() {
  sed -n '2,20p' "${BASH_SOURCE[0]}" >&2
  exit 2
}

while (($# > 0)); do
  case "$1" in
    --dest) [[ $# -ge 2 ]] || usage; dest="$2"; shift ;;
    --archive) [[ $# -ge 2 ]] || usage; archive="$2"; shift ;;
    --check) check_only=1 ;;
    --quiet) quiet=1 ;;
    --help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "install-node: unknown argument: $1" >&2; usage ;;
  esac
  shift
done

log() { ((quiet)) || echo "install-node: $*"; }
fail() { echo "install-node: FAIL: $*" >&2; exit 1; }

command -v python3 >/dev/null 2>&1 || fail "python3 is required (lock parsing)"
[[ -f "$lock_file" ]] || fail "missing $lock_file"
command -v curl >/dev/null 2>&1 || fail "curl is required to download the official archive"

read_lock() {
  python3 - "$lock_file" "$1" <<'PY'
import json, sys
lock = json.load(open(sys.argv[1], encoding="utf-8"))
node = lock.get("node", {})
if node.get("version") != "22.14.0":
    raise SystemExit("lock version mismatch: " + repr(node.get("version")))
if sys.argv[2] == "version":
    print(node["version"])
elif sys.argv[2] == "url":
    print(node["url"].rstrip("/"))
elif sys.argv[2] == "sha256":
    print(json.dumps(node.get("sha256", {}), sort_keys=True))
else:
    raise SystemExit("unknown lock field")
PY
}

version="$(read_lock version)"
case "$(uname -s) $(uname -m)" in
  "Darwin arm64") platform="darwin-arm64"; archive_name="node-v${version}-darwin-arm64.tar.gz" ;;
  "Linux x86_64") platform="linux-x64"; archive_name="node-v${version}-linux-x64.tar.xz" ;;
  *) fail "unsupported platform $(uname -s)/$(uname -m); lock only covers darwin-arm64/linux-x64" ;;
esac
expected_sha="$(read_lock sha256 | python3 -c 'import json,sys; print(json.load(sys.stdin)[sys.argv[1]])' "$archive_name")"
[[ -n "$expected_sha" ]] || fail "lock has no sha256 for $archive_name"
base_url="$(read_lock url)"

if [[ -z "$dest" ]]; then
  dest="$root/build/toolchain/node-v${version}"
fi
node_bin="$dest/bin/node"
npm_bin="$dest/bin/npm"

if ((check_only)); then
  [[ -x "$node_bin" ]] || fail "node not installed at $dest"
  actual="$("$node_bin" --version | sed 's/^v//')"
  [[ "$actual" == "$version" ]] || fail "installed node $actual != locked $version"
  npm_version="$("$node_bin" "$dest/lib/node_modules/npm/bin/npm-cli.js" --version 2>/dev/null || "$npm_bin" --version)"
  log "OK node v$actual npm $npm_version at $dest"
  echo "export PATH=\"$dest/bin:\$PATH\""
  exit 0
fi

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/kiarabot-node.XXXXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT
file="$tmpdir/$archive_name"
if [[ -n "$archive" ]]; then
  [[ -f "$archive" ]] || fail "archive not found: $archive"
  cp "$archive" "$file"
else
  url="$base_url/$archive_name"
  log "downloading $url"
  if ! curl --fail --location --silent --show-error --retry 2 --connect-timeout 20 \
      --output "$file" "$url"; then
    fail "download failed (offline?); pass --archive with a previously fetched official archive: $url"
  fi
fi

checksum() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}';
  elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}';
  else fail "no sha256sum/shasum available"; fi
}
actual_sha="$(checksum "$file")"
[[ "$actual_sha" == "$expected_sha" ]] || \
  fail "SHA-256 mismatch for $archive_name (expected $expected_sha, got $actual_sha)"
log "SHA-256 verified ($archive_name)"

mkdir -p "$tmpdir/unpack" "$(dirname "$dest")"
tar -xf "$file" -C "$tmpdir/unpack"
unpacked="$tmpdir/unpack/node-v${version}-${platform}"
[[ -d "$unpacked" ]] || fail "unexpected archive layout (missing $unpacked)"
if [[ -e "$dest" ]]; then
  fail "destination already exists: $dest (remove it or choose --dest; refusing to overwrite)"
fi
mv "$unpacked" "$dest"
"$node_bin" --version >/dev/null || fail "installed node does not run"
installed_npm="$(PATH="$dest/bin:$PATH" "$npm_bin" --version)"
log "installed Node v$version npm $installed_npm"
echo "export PATH=\"$dest/bin:\$PATH\""
