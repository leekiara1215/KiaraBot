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
full_preset="${KIARABOT_CHECK_PRESET:-full}"
case "$full_preset" in
  full|full-locked) ;;
  *) echo 'KIARABOT_CHECK_PRESET must be full or full-locked' >&2; exit 1 ;;
esac
# rg, python3 and ruby are used by the format, boundary and contract checks.
# Require them before any build so a missing tool fails immediately instead of
# surfacing after two full builds.
for tool in cmake "$cxx" make protoc grpc_cpp_plugin node npm clang-format clang-tidy rg python3 ruby; do
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
require_version Python "$(python3 --version | version_from_output)" 3.9
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

if [[ "$full_preset" == full-locked ]]; then
  [[ "$(python3 --version | version_from_output)" == 3.11.18 ]] || { echo 'Locked checks require Python 3.11.18' >&2; exit 1; }
  [[ "$node_version" == 22.14.0 ]] || { echo 'Locked checks require Node 22.14.0' >&2; exit 1; }
  [[ "$(clang-format --version | version_from_output)" == 16.0.6 ]] || { echo 'Locked checks require clang-format 16.0.6' >&2; exit 1; }
  [[ "$(clang-tidy --version | version_from_output)" == 22.1.8 ]] || { echo 'Locked checks require clang-tidy LLVM 22.1.8' >&2; exit 1; }
  protoc_version="$(protoc --version | version_from_output)"
  [[ "$protoc_version" == 29.3 || "$protoc_version" == 29.3.0 ]] || { echo 'Locked checks require protoc 29.3' >&2; exit 1; }
fi

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

# Shell syntax gate over every tracked script, not only scripts/.
while IFS= read -r -d '' script; do
  bash -n "$script"
done < <(git ls-files -z '*.sh')
python3 scripts/quality/check-dependency-lock.py
python3 scripts/quality/tests/dependency_lock_test.py
python3 scripts/quality/tests/architecture_test.py
python3 scripts/quality/check-boundaries.py
python3 scripts/quality/tests/boundaries_test.py
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --no-tests=error
# Full preset = proto + infrastructure + compile database, so one build feeds
# CTest, contract checks and clang-tidy. Keep it in sync with check-tidy.sh.
cmake --preset "$full_preset"
cmake --build --preset "$full_preset"
ctest --preset "$full_preset" --no-tests=error
python3 scripts/quality/check-reference.py
python3 scripts/quality/check-architecture.py --inner \
  --database "build/$full_preset/compile_commands.json" \
  --links "build/$full_preset/architecture-targets.json"
ctest --preset "$full_preset" --show-only=json-v1 > "build/$full_preset/ctest-registration.json"
python3 scripts/quality/check-architecture.py --stage p02 \
  --ctest-json "build/$full_preset/ctest-registration.json"
if [[ "$full_preset" == full-locked ]]; then
  python3 scripts/quality/check-dependency-lock.py --registry "${VCPKG_ROOT:?VCPKG_ROOT required}"
  triplet="$(sed -n 's/^VCPKG_TARGET_TRIPLET:STRING=//p' "build/$full_preset/CMakeCache.txt")"
  python3 scripts/quality/check-dependency-lock.py --installed build/architecture-installed --triplet "$triplet"
  cmake -S infrastructure -B build/infrastructure-locked \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_INSTALLED_DIR="$root/build/architecture-installed"
  cmake --build build/infrastructure-locked
  ctest --test-dir build/infrastructure-locked --output-on-failure --no-tests=error
else
  cmake -S infrastructure -B build/infrastructure
  cmake --build build/infrastructure
  ctest --test-dir build/infrastructure --output-on-failure --no-tests=error
fi
KIARABOT_DESCRIPTOR_COMPATIBILITY_TOOL="$root/build/$full_preset/kiarabot_descriptor_compatibility" \
  KIARABOT_LEGACY_READER_TEST=1 ./proto/tests/check_contracts.sh
./scripts/quality/check-format.sh
KIARABOT_TIDY_BUILD_DIR="build/$full_preset" ./scripts/quality/check-tidy.sh
./scripts/quality/check-target-api.sh
./scripts/quality/check-openapi.sh
./scripts/quality/check-asyncapi.sh
echo "All toolchain and quality checks passed"
