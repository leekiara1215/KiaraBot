#!/usr/bin/env bash
set -euo pipefail

missing=0
check() {
  local name="$1"
  local command_name="$2"
  if command -v "$command_name" >/dev/null 2>&1; then
    printf '%-20s %s\n' "$name" "$($command_name --version 2>&1 | head -n 1)"
  else
    printf '%-20s MISSING\n' "$name"
    missing=1
  fi
}

check "cmake" cmake
check "c++ compiler" c++
check "make" make
check "protoc" protoc
if command -v grpc_cpp_plugin >/dev/null 2>&1; then
  printf '%-20s %s\n' "grpc_cpp_plugin" "available (version follows installed gRPC package)"
else
  printf '%-20s MISSING\n' "grpc_cpp_plugin"
  missing=1
fi
check "node" node
check "npm" npm

if [[ "$missing" -ne 0 ]]; then
  echo "One or more required tools are missing; see doc/11-toolchain.md." >&2
  exit 1
fi
