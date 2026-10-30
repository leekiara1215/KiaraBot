#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Required tool not found: $1" >&2
    exit 1
  fi
}

require_version() {
  local name="$1" actual="$2" minimum="$3"
  if [[ ! "$actual" =~ ^[0-9]+([.][0-9]+)*$ ]] || ! awk -v actual="$actual" -v minimum="$minimum" 'BEGIN {
    split(actual, a, "."); split(minimum, b, ".");
    for (i = 1; i <= 3; i++) {
      if (a[i] + 0 > b[i] + 0) exit 0;
      if (a[i] + 0 < b[i] + 0) exit 1;
    }
    exit 0;
  }'; then
    echo "$name $minimum or newer is required (found: ${actual:-unknown})" >&2
    exit 1
  fi
  printf '%-20s %s (minimum %s)\n' "$name" "$actual" "$minimum"
}

version_from_output() {
  sed -nE '/[0-9]+[.][0-9]+/ { s/[^0-9]*([0-9]+([.][0-9]+)+).*/\1/; p; q; }'
}

cxx="${CXX:-c++}"
for tool in cmake "$cxx" make protoc grpc_cpp_plugin node npm npx clang-format clang-tidy; do
  require_command "$tool"
done

require_version CMake "$(cmake --version | version_from_output)" 3.24
make_version_output="$(make --version)"
if [[ "$make_version_output" != GNU\ Make* ]]; then
  echo 'GNU Make is required by the Unix Makefiles presets' >&2
  exit 1
fi
require_version Make "$(printf '%s\n' "$make_version_output" | version_from_output)" 3.81
compiler_macros="$("$cxx" -dM -E -x c++ /dev/null)"
macro_value() {
  printf '%s\n' "$compiler_macros" | awk -v name="$1" '$2 == name {print $3; exit}'
}
if [[ -n "$(macro_value __clang__)" ]]; then
  compiler_version="$(macro_value __clang_major__).$(macro_value __clang_minor__).$(macro_value __clang_patchlevel__)"
  if [[ -n "$(macro_value __apple_build_version__)" ]]; then
    require_version AppleClang "$compiler_version" 15
  else
    require_version Clang "$compiler_version" 16
  fi
elif [[ -n "$(macro_value __GNUC__)" ]]; then
  compiler_version="$(macro_value __GNUC__).$(macro_value __GNUC_MINOR__).$(macro_value __GNUC_PATCHLEVEL__)"
  require_version GCC "$compiler_version" 13
else
  echo "Unsupported C++ compiler: $cxx (expected AppleClang, Clang or GCC)" >&2
  exit 1
fi
require_version protoc "$(protoc --version | version_from_output)" 25
node_version="$(node --version | version_from_output)"
require_version Node "$node_version" 20.19
node_major="${node_version%%.*}"
if [[ "$node_major" -eq 21 ]]; then
  echo 'Redocly 2.72.1 requires Node 20.19+ below 21, or Node 22.12+' >&2
  exit 1
elif [[ "$node_major" -eq 22 ]]; then
  require_version Node "$node_version" 22.12
fi
require_version npm "$(npm --version | version_from_output)" 10
require_version clang-format "$(clang-format --version | version_from_output)" 16
require_version clang-tidy "$(clang-tidy --version | version_from_output)" 16

scratch="$(mktemp -d "${TMPDIR:-/tmp}/kiarabot-toolchain.XXXXXXXX")"
trap 'rm -rf "$scratch"' EXIT
cat > "$scratch/smoke.cpp" <<'CPP'
#include <concepts>
#include <span>
static_assert(std::same_as<decltype(std::span{static_cast<int*>(nullptr), 0}), std::span<int>>);
int main() { return 0; }
CPP
"$cxx" -std=c++20 -Wall -Wextra -Wpedantic "$scratch/smoke.cpp" -o "$scratch/smoke"
"$scratch/smoke"
cat > "$scratch/smoke.proto" <<'PROTO'
syntax = "proto3";
package kiarabot.toolchain;
message Request { string value = 1; }
service Smoke { rpc Check(Request) returns (Request); }
PROTO
protoc --proto_path="$scratch" --cpp_out="$scratch" --grpc_out="$scratch" \
  --plugin="protoc-gen-grpc=$(command -v grpc_cpp_plugin)" "$scratch/smoke.proto"
for generated in smoke.pb.h smoke.pb.cc smoke.grpc.pb.h smoke.grpc.pb.cc; do
  test -s "$scratch/$generated"
done
echo "C++20 compile and Protobuf/gRPC generation passed"

while IFS= read -r -d '' script; do
  bash -n "$script"
done < <(find scripts -type f -name '*.sh' -print0)
python3 scripts/quality/check-boundaries.py
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
cmake --preset proto -DKIARABOT_ENABLE_INFRASTRUCTURE=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build --preset proto
ctest --preset proto
cmake -S infrastructure -B build/infrastructure
cmake --build build/infrastructure
ctest --test-dir build/infrastructure --output-on-failure
./proto/tests/check_contracts.sh
./scripts/quality/check-format.sh
./scripts/quality/check-tidy.sh
./scripts/quality/check-openapi.sh
./scripts/quality/check-asyncapi.sh
echo "All toolchain and quality checks passed"
