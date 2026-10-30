#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tool="${KIARABOT_DESCRIPTOR_COMPATIBILITY_TOOL:-$root/build/proto/kiarabot_descriptor_compatibility}"
[[ -x "$tool" ]] || { echo "Missing compatibility tool: $tool" >&2; exit 1; }
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT
mkdir -p "$tmp_dir/proto/kiarabot/v1"
cp "$root"/proto/kiarabot/v1/*.proto "$tmp_dir/proto/kiarabot/v1/"
include_args=(--proto_path="$tmp_dir/proto")
if [[ -n "${PROTOBUF_INCLUDE_DIR:-}" ]]; then
  include_args+=(--proto_path="$PROTOBUF_INCLUDE_DIR")
fi
generate() {
  protoc "${include_args[@]}" --include_imports --descriptor_set_out="$1" \
    "$tmp_dir"/proto/kiarabot/v1/*.proto
}
generate "$tmp_dir/original.pb"
"$tool" "$tmp_dir/original.pb" "$tmp_dir/original.pb"
# Reusing an existing field number for a different field must be rejected.
python3 - "$tmp_dir/proto/kiarabot/v1/common.proto" <<'PY'
import pathlib
import sys
path = pathlib.Path(sys.argv[1])
source = path.read_text()
old = '  string message = 2;'
if source.count(old) != 1:
    raise SystemExit('test fixture field not found exactly once')
path.write_text(source.replace(old, '  string changed_message = 2;', 1))
PY
generate "$tmp_dir/incompatible.pb"
expect_incompatible() {
  local status=0
  "$tool" "$1" "$2" || status=$?
  if [[ "$status" -ne 1 ]]; then
    echo "Expected an incompatibility exit code of 1, got $status" >&2
    exit 1
  fi
}
expect_incompatible "$tmp_dir/incompatible.pb" "$tmp_dir/original.pb"
expect_incompatible "$tmp_dir/original.pb" "$tmp_dir/incompatible.pb"
cp "$root/proto/kiarabot/v1/common.proto" "$tmp_dir/proto/kiarabot/v1/common.proto"
printf '\nmessage CiCompatibilityFixture { string value = 1; }\n' >> "$tmp_dir/proto/kiarabot/v1/common.proto"
generate "$tmp_dir/additive.pb"
"$tool" "$tmp_dir/additive.pb" "$tmp_dir/original.pb"
echo "Descriptor compatibility positive and negative checks passed"
