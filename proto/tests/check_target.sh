#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
python_bin="${KIARABOT_TARGET_PYTHON:-python3}"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/kiarabot-target.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
sources=("$root"/proto/kiarabot/v1/*.proto)
protoc -I "$root/proto" --cpp_out="$build_dir" --grpc_out="$build_dir" \
  --plugin=protoc-gen-grpc="$(command -v grpc_cpp_plugin)" \
  --include_imports --descriptor_set_out="$build_dir/target.pb" "${sources[@]}"
# grpc_tools owns a matching Python generator; no runtime version-check bypass.
"$python_bin" -m grpc_tools.protoc -I "$root/proto" --python_out="$build_dir" \
  --grpc_python_out="$build_dir" "${sources[@]}"
# pkg-config flags are intentionally word-split as compiler arguments.
# shellcheck disable=SC2046
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Wpedantic \
  $(pkg-config --cflags protobuf grpc++ | sed 's/-I/-isystem /g') -I "$build_dir" \
  "$root/proto/tests/target_roundtrip.cc" "$build_dir"/kiarabot/v1/*.pb.cc $(pkg-config --libs grpc++) \
  $(pkg-config --libs protobuf) -o "$build_dir/roundtrip"
PYTHONPATH="$build_dir" "$python_bin" "$root/proto/tests/target_contract_test.py" \
  "$build_dir/target.pb" "$build_dir/roundtrip"
