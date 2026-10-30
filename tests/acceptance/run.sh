#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
build_dir="${KIARABOT_ACCEPTANCE_BUILD:-$root/build/proto}"
python="${KIARABOT_ACCEPTANCE_PYTHON:-$root/sdk/python/.venv/bin/python}"
test -x "$python" || { echo 'Create the locked sdk/python virtual environment first' >&2; exit 2; }
test -x "$build_dir/core/kiarabot-core" || { echo 'Build proto + infrastructure Core first' >&2; exit 2; }
export CORE_TEST_BINARY="$build_dir/core/kiarabot-core"
"$python" sdk/python/tools/generate_proto.py
control_python="${KIARABOT_CONTROL_TEST_PYTHON:-$root/control/api/tests/.venv/bin/python}"
test -x "$control_python" || { echo 'Create the locked control/api/tests environment first' >&2; exit 2; }
"$control_python" -m pytest -q control/api/tests
ctest --test-dir "$build_dir" --output-on-failure --no-tests=error
CONTROL_POLICY_BINARY="$build_dir/kiarabot-control-application-test" ruby proto/tests/api_contract_test.rb
"$python" -m pytest -q sdk/python/tests
if [[ -n "${KIARABOT_ADAPTER_DIR:-}" ]]; then
  adapter_python="$KIARABOT_ADAPTER_DIR/.venv/bin/python"
  test -x "$adapter_python" || { echo 'Adapter locked environment is required' >&2; exit 2; }
  export SDK_SOURCE="$root/sdk/python/src"
  (cd "$KIARABOT_ADAPTER_DIR" && "$adapter_python" -m pytest -q)
fi
