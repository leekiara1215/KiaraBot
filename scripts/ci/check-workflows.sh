#!/usr/bin/env bash
# Local GitHub Actions workflow checks (P14b-2). This validates what can be checked without a
# repository token: YAML syntax, action SHA pinning, release safety gates and a gh/remote probe.
# It never triggers a workflow and never prints or persists credentials.
set -euo pipefail
export LC_ALL=C

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

workflows=(.github/workflows/*.yml)
failed=0

echo "== YAML syntax =="
if command -v ruby >/dev/null 2>&1; then
  ruby -ryaml -e 'ARGV.each { |path| YAML.load_file(path); puts "parsed #{path}" }' "${workflows[@]}" || failed=1
elif command -v python3 >/dev/null 2>&1 && python3 -c 'import yaml' 2>/dev/null; then
  python3 - "${workflows[@]}" <<'PY' || failed=1
import sys
import yaml
for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as handle:
        yaml.safe_load(handle)
    print(f"parsed {path}")
PY
else
  echo "no YAML parser available (install ruby or python3-yaml)" >&2
  failed=1
fi

echo "== action pinning =="
if ! python3 - "${workflows[@]}" <<'PY'
import re
import sys

pattern = re.compile(r"^\s*uses:\s*([^\s#]+)", re.MULTILINE)
sha = re.compile(r"@[0-9a-f]{40}$")
failed = False
for path in sys.argv[1:]:
    source = open(path, encoding="utf-8").read()
    for match in pattern.finditer(source):
        value = match.group(1).strip("'\"")
        if value.startswith("./"):
            continue
        if not sha.search(value):
            print(f"{path}: action is not pinned to a 40-hex commit: {value}", file=sys.stderr)
            failed = True
        else:
            print(f"{path}: pinned {value}")
raise SystemExit(1 if failed else 0)
PY
then
  failed=1
fi

echo "== release safety gates =="
if ! python3 - <<'PY'
from pathlib import Path
import re

failed = False
release_path = Path('.github/workflows/release.yml')
if not release_path.is_file():
    print('skip release gates: .github/workflows/release.yml is not present yet')
    release = None
else:
    release = release_path.read_text(encoding='utf-8')
checks = {} if release is None else {
    'manual workflow_dispatch trigger': 'workflow_dispatch:' in release,
    'v* tag trigger': re.search(r'tags:\s*\[?["\']?v\*', release) is not None,
    'protected release-gated environment': 'environment: release-gated' in release,
    'packages write permission': 'packages: write' in release,
    'id-token write for keyless cosign': 'id-token: write' in release,
    'push gated by RELEASE_PUSH_ENABLED': 'RELEASE_PUSH_ENABLED' in release,
    'signature gated by RELEASE_COSIGN_KEYLESS': 'RELEASE_COSIGN_KEYLESS' in release,
    'GHCR registry': 'REGISTRY: ghcr.io' in release,
    'SBOM generation': 'SYFT_IMAGE' in release and '.spdx.json' in release,
    'cosign signing': 'COSIGN_IMAGE' in release and 'sign --yes' in release,
}
for label, ok in checks.items():
    print(('ok   ' if ok else 'FAIL ') + label)
    failed = failed or not ok

ci = Path('.github/workflows/ci.yml').read_text(encoding='utf-8')
for label, ok in {
    'CI has workflow_dispatch': 'workflow_dispatch:' in ci,
    'CI auto-triggers are gated by CI_AUTO_ENABLED': 'vars.CI_AUTO_ENABLED' in ci,
    'CI evidence summary job': 'ci-evidence' in ci,
    'full-locked build job': 'full-locked' in ci,
    'full-locked CTest gate (no filtering)': 'ctest --test-dir build/full-locked' in ci,
}.items():
    print(('ok   ' if ok else 'FAIL ') + label)
    failed = failed or not ok
raise SystemExit(1 if failed else 0)
PY
then
  failed=1
fi

echo "== gh / remote probe (no credentials written) =="
git remote -v | sed -n '1,2p'
if command -v gh >/dev/null 2>&1; then
  if gh auth status >/dev/null 2>&1; then
    echo "gh: authenticated"
  else
    echo "gh: installed but NOT authenticated; workflow_dispatch verification must run after 'gh auth login'."
  fi
else
  echo "gh: not installed"
fi

if (( failed )); then
  echo "workflow checks FAILED" >&2
  exit 1
fi
echo "workflow checks passed (GitHub Actions were NOT triggered by this script)"
