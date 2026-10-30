#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
command -v clang-tidy >/dev/null 2>&1 || { echo 'clang-tidy is required' >&2; exit 1; }
if [[ ! -f build/proto/compile_commands.json ]]; then
  echo 'Configure and build the proto preset with CMAKE_EXPORT_COMPILE_COMMANDS=ON first' >&2
  exit 1
fi
files="$(mktemp "${TMPDIR:-/tmp}/kiarabot-tidy.XXXXXXXX")"
trap 'rm -f "$files"' EXIT
find core proto/tests control/api infrastructure/src infrastructure/tests -type f \( -name '*.cpp' -o -name '*.cc' \) \
  -print0 > "$files"
python3 - "$files" <<'PY'
"""Fail when a file scheduled for analysis has no compile-command entry."""
import json
import os
import sys

database_path = 'build/proto/compile_commands.json'
with open(database_path, encoding='utf-8') as stream:
    database = json.load(stream)
compiled = set()
for entry in database:
    path = entry['file']
    if not os.path.isabs(path):
        path = os.path.join(entry['directory'], path)
    compiled.add(os.path.realpath(path))
with open(sys.argv[1], 'rb') as stream:
    scheduled = [os.path.realpath(os.fsdecode(raw))
                 for raw in stream.read().split(b'\0') if raw]
missing = [path for path in scheduled if path not in compiled]
if missing:
    print(f'{database_path} has no compile command for:', file=sys.stderr)
    for path in missing:
        print('  ' + os.path.relpath(path), file=sys.stderr)
    print('Reconfigure the proto preset so every file above is built.', file=sys.stderr)
    sys.exit(1)
PY
set -- -p build/proto --checks='clang-analyzer-*' --warnings-as-errors='*' --header-filter="^${root}/(core|proto|control|infrastructure)/"
if [[ "$(uname -s)" == Darwin ]]; then
  compiler="$(sed -n 's/^CMAKE_CXX_COMPILER:FILEPATH=//p' build/proto/CMakeCache.txt)"
  if [[ -z "$compiler" ]]; then
    echo 'CMake did not record the C++ compiler' >&2
    exit 1
  fi
  compiler_macros="$("$compiler" -dM -E -x c++ /dev/null)"
  if [[ "$compiler_macros" == *'#define __apple_build_version__'* ]]; then
    sdk="${SDKROOT:-$(xcrun --show-sdk-path)}"
    set -- "$@" --extra-arg=-isysroot --extra-arg="$sdk" \
      --extra-arg=-isystem --extra-arg="$sdk/usr/include/c++/v1"
  fi
fi
xargs -0 clang-tidy "$@" < "$files"
