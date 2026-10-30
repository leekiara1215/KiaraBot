#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
baseline_ref="${1:?usage: check-descriptor.sh BASELINE_COMMIT}"
if [[ "$baseline_ref" =~ ^0+$ ]] || ! git -C "$root" cat-file -e "${baseline_ref}^{commit}"; then
  echo "Cannot resolve a baseline commit: $baseline_ref" >&2
  exit 1
fi

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT
git -C "$root" archive "$baseline_ref" proto/kiarabot/v1 | tar -x -C "$tmp_dir"
proto_files=("$tmp_dir"/proto/kiarabot/v1/*.proto)
if [[ ! -f "${proto_files[0]}" ]]; then
  echo "The baseline has no v1 protocol files" >&2
  exit 1
fi
include_args=(--proto_path="$tmp_dir/proto")
if [[ -n "${PROTOBUF_INCLUDE_DIR:-}" ]]; then
  include_args+=(--proto_path="$PROTOBUF_INCLUDE_DIR")
fi
protoc "${include_args[@]}" --include_imports \
  --descriptor_set_out="$tmp_dir/baseline.pb" "${proto_files[@]}"
test -s "$tmp_dir/baseline.pb"
KIARABOT_BASELINE_DESCRIPTOR="$tmp_dir/baseline.pb" \
  "$root/proto/tests/check_contracts.sh"
