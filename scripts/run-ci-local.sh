#!/usr/bin/env bash
# Unified local CI entry (P14b). Mirrors the remote CI gates without copying
# their logic: each mode invokes the existing CMake/CTest, proto/tests,
# scripts/quality, uv/pytest and panel npm entrypoints.
#
# usage:
#   scripts/run-ci-local.sh {toolchain|build|test|contracts|python|panel|all}
#                           [--json PATH] [--self-test] [--dry-run]
#
# Environment:
#   PATH must include the locked toolchain (Node 22.14.0 via
#   scripts/toolchain/install-node.sh; protoc/grpc from the P00 vcpkg tree).
#   Build/test require a configured build/full-locked (see docs/spec/ci-gates.md).
#   KIARABOT_CHECK_PRESET (default full-locked) is forwarded to check-toolchain.
#   KIARABOT_JSON (or --json PATH) writes a machine-readable report.
#
# Exit codes: 0 all executed steps passed; 1 a step failed; 2 usage/environment
# error before running steps. Skipped prerequisites are recorded explicitly and
# do NOT report a green run as fully verified.
set -euo pipefail
export LC_ALL=C

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

mode=""
json_path="${KIARABOT_JSON:-}"
dry_run=0
self_test=0
records_file="$(mktemp "${TMPDIR:-/tmp}/kiarabot-ci-records.XXXXXXXX")"
trap 'rm -f "$records_file"' EXIT
failed=0
started_at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
git_sha="$(git rev-parse HEAD 2>/dev/null || echo unknown)"

usage() {
  sed -n '2,22p' "${BASH_SOURCE[0]}" >&2
  exit 2
}

while (($# > 0)); do
  case "$1" in
    toolchain|build|test|contracts|python|panel|all)
      [[ -z "$mode" ]] || usage
      mode="$1"
      ;;
    --json)
      if [[ $# -ge 2 && "$2" != --* ]]; then json_path="$2"; shift; else
        json_path="$root/build/ci-local/ci-local-${git_sha}.json"
      fi
      ;;
    --self-test) self_test=1 ;;
    --dry-run) dry_run=1 ;;
    --help) sed -n '2,22p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) usage ;;
  esac
  shift
done

record() {
  local status="$1" name="$2" duration="$3" command="$4" note="${5:-}"
  printf '%s\t%s\t%s\t%s\t%s\n' "$status" "$name" "$duration" "$command" "$note" >> "$records_file"
}

write_json() {
  local target="$1" overall="$2"
  [[ -n "$target" ]] || return 0
  mkdir -p "$(dirname "$target")"
  python3 - "$target" "$overall" "$mode" "$git_sha" "$started_at" "$records_file" <<'PY'
import json, sys
target, overall, mode, sha, started, records = sys.argv[1:7]
steps = []
for line in open(records, encoding="utf-8"):
    parts = line.rstrip("\n").split("\t")
    if len(parts) != 5:
        continue
    status, name, duration, command, note = parts
    steps.append({"name": name, "status": status, "duration_seconds": float(duration),
                  "command": command, "note": note})
document = {"schema": "kiarabot.ci-local.v1", "mode": mode, "git_sha": sha,
            "started_at": started, "overall": overall,
            "steps": steps,
            "verification_note": "Skipped steps mean the prerequisite was absent; "
                                 "they are not evidence of a verified gate."}
with open(target, "w", encoding="utf-8") as handle:
    json.dump(document, handle, indent=2, sort_keys=True)
    handle.write("\n")
print(target)
PY
}

run() {
  local name="$1" command="$2"
  shift 2
  local started ended duration
  started="$(date +%s)"
  echo "=== $name"
  if ((dry_run)); then
    echo "DRY-RUN: $command"
    record "dry-run" "$name" 0 "$command"
    return 0
  fi
  if "$@"; then
    ended="$(date +%s)"; duration=$((ended - started))
    record "pass" "$name" "$duration" "$command"
    echo "=== $name: PASS (${duration}s)"
    return 0
  fi
  ended="$(date +%s)"; duration=$((ended - started))
  record "fail" "$name" "$duration" "$command"
  echo "=== $name: FAIL (${duration}s)" >&2
  return 1
}

skip() {
  local name="$1" reason="$2"
  echo "=== $name: SKIP ($reason)"
  record "skip" "$name" 0 "-" "$reason"
}

require_path() {
  if [[ ! -e "$1" ]]; then
    echo "run-ci-local: missing prerequisite: $1" >&2
    echo "$2" >&2
    exit 2
  fi
}

section_toolchain() {
  run "check-toolchain" "check-toolchain.sh (preset ${KIARABOT_CHECK_PRESET:-full-locked})" \
    env "KIARABOT_CHECK_PRESET=${KIARABOT_CHECK_PRESET:-full-locked}" bash scripts/check-toolchain.sh
}

section_build() {
  local build_dir="${KIARABOT_BUILD_DIR:-build/full-locked}"
  if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    if [[ -z "${VCPKG_ROOT:-}" ]]; then
      skip "configure-full-locked" "build cache missing and VCPKG_ROOT unset; follow docs/spec/ci-gates.md P00 shared configure"
      echo "run-ci-local: cannot build without a configured build/full-locked" >&2
      return 1
    fi
    local extra=()
    [[ -n "${VCPKG_INSTALLED_DIR:-}" ]] && extra+=("-DVCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}")
    [[ -n "${KIARABOT_TEST_OPENSSL:-}" ]] && extra+=("-DKIARABOT_TEST_OPENSSL=${KIARABOT_TEST_OPENSSL}")
    [[ -n "${KIARABOT_TEST_PG_BIN:-}" ]] && extra+=("-DKIARABOT_TEST_PG_BIN=${KIARABOT_TEST_PG_BIN}")
    [[ -n "${KIARABOT_TARGET_PYTHON:-}" ]] && extra+=("-DKIARABOT_TARGET_PYTHON=${KIARABOT_TARGET_PYTHON}")
    run "configure-full-locked" "cmake --preset full-locked -B $build_dir (shared P00 dependencies)" \
      cmake --preset full-locked -B "$build_dir" -DVCPKG_MANIFEST_MODE=OFF ${extra[@]+"${extra[@]}"} || return 1
  fi
  run "build-full-locked" "cmake --build --preset full-locked --parallel ${KIARABOT_BUILD_JOBS:-3}" \
    cmake --build --preset full-locked --parallel "${KIARABOT_BUILD_JOBS:-3}"
}

section_test() {
  local build_dir="${KIARABOT_BUILD_DIR:-build/full-locked}"
  if [[ ! -f "$build_dir/CTestTestfile.cmake" ]]; then
    skip "ctest-full-locked" "build/full-locked is not configured; run build first"
    echo "run-ci-local: cannot run CTest without a configured build" >&2
    return 1
  fi
  run "ctest-full-locked" "ctest --preset full-locked --output-on-failure" \
    ctest --preset full-locked --output-on-failure
}

section_contracts() {
  local check
  for check in check_implementation_status check_semantic_rules check_enum_consistency \
               evidence_scopes_test; do
    run "contracts-$check" "python3 proto/tests/$check.py" \
      python3 "proto/tests/$check.py" || return 1
  done
  run "contracts-target-api" "ruby proto/tests/target_api_test.rb" \
    ruby proto/tests/target_api_test.rb || return 1
  local descriptor_tool="${KIARABOT_DESCRIPTOR_COMPATIBILITY_TOOL:-$root/build/proto/kiarabot_descriptor_compatibility}"
  if [[ -x "$descriptor_tool" ]]; then
    run "contracts-descriptor" "proto/tests/check_contracts.sh with descriptor tool" \
      env "KIARABOT_DESCRIPTOR_COMPATIBILITY_TOOL=$descriptor_tool" \
          "PROTOBUF_INCLUDE_DIR=${PROTOBUF_INCLUDE_DIR:-/opt/homebrew/include}" \
          bash proto/tests/check_contracts.sh || return 1
  else
    skip "contracts-descriptor" "descriptor compatibility tool missing at $descriptor_tool (build proto preset)"
  fi
  if [[ -f scripts/quality/check-boundaries.py ]]; then
    run "quality-boundaries" "python3 scripts/quality/check-boundaries.py" \
      python3 scripts/quality/check-boundaries.py || return 1
  else
    skip "quality-boundaries" "scripts/quality/check-boundaries.py missing"
  fi
  if [[ -f scripts/quality/check-format.sh ]]; then
    if command -v clang-format >/dev/null 2>&1; then
      run "quality-format" "bash scripts/quality/check-format.sh" \
        bash scripts/quality/check-format.sh || return 1
    else
      skip "quality-format" "clang-format not on PATH (P00 build/format-venv)"
    fi
  fi
}

section_python() {
  local sdk_python="${KIARABOT_SDK_PYTHON:-$root/sdk/python/.venv/bin/python}"
  local adapter_python="${KIARABOT_ADAPTER_PYTHON:-$root/adapters/nonebot/.venv/bin/python}"
  local control_python="${KIARABOT_CONTROL_TEST_PYTHON:-$root/control/api/tests/.venv/bin/python}"
  if [[ -x "$sdk_python" ]]; then
    run "python-sdk" "sdk/python pytest" \
      bash -c 'cd "$1" && "$2" -m pytest -q' _ "$root/sdk/python" "$sdk_python" || return 1
  else
    skip "python-sdk" "missing $sdk_python (uv sync --project sdk/python)"
  fi
  if [[ -x "$adapter_python" ]]; then
    run "python-adapter" "adapters/nonebot pytest" \
      bash -c 'cd "$1" && PYTHONPATH=src:generated "$2" -m pytest -q' _ \
        "$root/adapters/nonebot" "$adapter_python" || return 1
  else
    skip "python-adapter" "missing $adapter_python (uv sync --project adapters/nonebot)"
  fi
  if [[ -x "$control_python" ]]; then
    run "python-control-tests" "control/api/tests pytest" \
      bash -c 'cd "$1" && "$2" -m pytest -q' _ "$root/control/api/tests" "$control_python" || return 1
  else
    skip "python-control-tests" "missing $control_python (uv sync --project control/api/tests)"
  fi
}

section_panel() {
  local node_bin="${KIARABOT_NODE_BIN:-$root/build/toolchain/node-v22.14.0/bin}"
  if [[ ! -x "$node_bin/node" ]]; then
    skip "panel" "locked Node 22.14.0 missing; run scripts/toolchain/install-node.sh"
    echo "run-ci-local: panel requires locked Node 22.14.0" >&2
    return 1
  fi
  export PATH="$node_bin:$PATH"
  run "panel-check-contracts" "npm run check:contracts --prefix panel" \
    npm run check:contracts --prefix panel || return 1
  run "panel-lint" "npm run lint --prefix panel" \
    npm run lint --prefix panel || return 1
  run "panel-test" "npm test --prefix panel" \
    npm test --prefix panel || return 1
  run "panel-build" "npm run build --prefix panel" \
    npm run build --prefix panel
}

run_mode() {
  local step="$1"
  case "$step" in
    toolchain) section_toolchain ;;
    build) section_build ;;
    test) section_test ;;
    contracts) section_contracts ;;
    python) section_python ;;
    panel) section_panel ;;
    *) echo "run-ci-local: unknown mode $step" >&2; return 1 ;;
  esac
}

if ((self_test)); then
  [[ -n "$mode" ]] || { echo "run-ci-local: --self-test requires a mode" >&2; exit 2; }
  record "pass" "self-test" 0 "argument/json writer" "no gates executed"
  tmp_json="$(mktemp "${TMPDIR:-/tmp}/kiarabot-ci-selftest.XXXXXXXX.json")"
  write_json "$tmp_json" "self-test-pass" >/dev/null
  python3 - "$tmp_json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
assert d["schema"] == "kiarabot.ci-local.v1"
assert d["mode"] and d["git_sha"] and d["steps"][0]["status"] == "pass"
print("ci-local self-test: JSON writer/mode parsing OK")
PY
  rm -f "$tmp_json"
  echo "run-ci-local: self-test PASS"
  exit 0
fi

[[ -n "$mode" ]] || usage

if [[ "$mode" == all ]]; then
  steps=(toolchain contracts python panel build test)
  for step in "${steps[@]}"; do
    echo
    echo "### run-ci-local: $step"
    case "$step" in
      toolchain) section_toolchain ;;
      build) section_build ;;
      test) section_test ;;
      contracts) section_contracts ;;
      python) section_python ;;
      panel) section_panel ;;
    esac || { overall="fail:$step"; write_json "$json_path" "$overall" >/dev/null || true; exit 1; }
  done
  skipped_count="$(awk -F '\t' '$1 == "skip" {n++} END {print n+0}' "$records_file")"
  overall="pass"
  ((skipped_count > 0)) && overall="pass-with-skips"
  write_json "$json_path" "$overall" >/dev/null || true
  echo
  echo "run-ci-local: ALL requested steps passed (${skipped_count} skipped prerequisite(s))"
  exit 0
fi

run_mode "$mode" || { write_json "$json_path" "fail:$mode" >/dev/null || true; exit 1; }
skipped_count="$(awk -F '\t' '$1 == "skip" {n++} END {print n+0}' "$records_file")"
overall="pass"
((skipped_count > 0)) && overall="pass-with-skips"
write_json "$json_path" "$overall" >/dev/null || true
echo
if ((skipped_count > 0)); then
  echo "run-ci-local: $mode PASS WITH SKIPS (${skipped_count} prerequisite(s) missing; see JSON/log)"
else
  echo "run-ci-local: $mode PASS"
fi
