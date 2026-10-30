#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
command -v clang-tidy >/dev/null 2>&1 || { echo 'clang-tidy is required' >&2; exit 1; }
if [[ ! -f build/proto/compile_commands.json ]]; then
  echo 'Configure and build the proto preset with CMAKE_EXPORT_COMPILE_COMMANDS=ON first' >&2
  exit 1
fi
set -- -p build/proto --warnings-as-errors='*' --header-filter="^${root}/(core|proto)/"
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
find core proto/tests -type f \( -name '*.cpp' -o -name '*.cc' \) \
  -print0 | xargs -0 clang-tidy "$@"
