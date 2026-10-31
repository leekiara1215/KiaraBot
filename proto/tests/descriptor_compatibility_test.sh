#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tool="${1:-$root/build/proto/kiarabot_descriptor_compatibility}"
if [[ ! -x "$tool" ]]; then
  echo "descriptor compatibility tool is missing: $tool" >&2
  exit 2
fi
if ! command -v protoc >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then
  echo "protoc and python3 are required for compatibility tests" >&2
  exit 2
fi

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT
mkdir -p "$tmp_dir/baseline"
cp -R "$root/proto/kiarabot" "$tmp_dir/baseline/"

make_descriptor() {
  local source_dir="$1"
  local output="$2"
  local include_args=(--proto_path="$source_dir")
  if [[ -n "${PROTOBUF_INCLUDE_DIR:-}" ]]; then
    include_args+=(--proto_path="$PROTOBUF_INCLUDE_DIR")
  fi
  protoc "${include_args[@]}" --descriptor_set_out="$output" --include_imports \
    "$source_dir"/kiarabot/v1/*.proto
}

make_descriptor "$tmp_dir/baseline" "$tmp_dir/baseline.pb"

for scenario in additions field_type field_removed enum_number oneof_name service_signature; do
  mkdir -p "$tmp_dir/$scenario"
  cp -R "$tmp_dir/baseline/kiarabot" "$tmp_dir/$scenario/"
  python3 - "$tmp_dir/$scenario/kiarabot/v1" "$scenario" <<'PY'
from pathlib import Path
import sys

directory = Path(sys.argv[1])
scenario = sys.argv[2]
file_name, old, new = {
    "additions": ("actions.proto", "  CANCELLED = 5;", "  CANCELLED = 5;\n  DEFERRED = 6;"),
    "field_type": ("actions.proto", "  string action_id = 1;", "  uint64 action_id = 1;"),
    "field_removed": ("actions.proto", "  string idempotency_key = 2;\n", ""),
    "enum_number": ("actions.proto", "  CANCELLED = 5;", "  CANCELLED = 6;"),
    "oneof_name": ("events.proto", "  oneof payload {", "  oneof renamed_payload {"),
    "service_signature": ("services.proto", "returns (stream CoreToAdapter)", "returns (CoreToAdapter)"),
}[scenario]
path = directory / file_name
source = path.read_text()
if source.count(old) == 0:
    raise SystemExit(f"fixture text missing: {scenario}")
source = source.replace(old, new, 1)
if scenario == "additions":
    source = source.replace("  oneof payload {", "  string new_field = 8;\n\n  oneof payload {", 1)
path.write_text(source)
PY
  make_descriptor "$tmp_dir/$scenario" "$tmp_dir/$scenario.pb"
  if [[ "$scenario" == additions ]]; then
    "$tool" "$tmp_dir/$scenario.pb" "$tmp_dir/baseline.pb"
  else
    set +e
    "$tool" "$tmp_dir/$scenario.pb" "$tmp_dir/baseline.pb"
    result=$?
    set -e
    if [[ "$result" -ne 1 ]]; then
      echo "expected incompatibility exit 1 for $scenario, got $result" >&2
      exit 1
    fi
  fi
done

echo "Descriptor compatibility scenarios passed"
