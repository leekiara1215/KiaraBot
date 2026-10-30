#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
proto_dir="$root/proto"
tmp_dir="${TMPDIR:-/tmp}/kiarabot-contracts.$$"
trap 'rm -rf "$tmp_dir"' EXIT
mkdir -p "$tmp_dir"

grep -q 'oneof payload' "$proto_dir/kiarabot/v1/events.proto"
grep -q 'oneof payload' "$proto_dir/kiarabot/v1/actions.proto"
if grep -Eq '^  string type = ' "$proto_dir/kiarabot/v1/events.proto" "$proto_dir/kiarabot/v1/actions.proto"; then
  echo "EventEnvelope and ActionEnvelope must use oneof as their only type discriminator" >&2
  exit 1
fi

python3 "$proto_dir/tests/check_enum_consistency.py"

grep -q '^security:' "$root/api/openapi.yaml"
grep -q 'bearerAuth:' "$root/api/openapi.yaml"
grep -q 'Idempotency-Key' "$root/api/openapi.yaml"
grep -q 'trace_id:' "$root/api/openapi.yaml"
grep -q 'retryable:' "$root/api/openapi.yaml"
grep -q 'bearerAuth' "$root/api/asyncapi.yaml"
if grep -q '^security:' "$root/api/asyncapi.yaml"; then
  echo "AsyncAPI 2.x does not allow a root security field; bind bearer auth to the server" >&2
  exit 1
fi
grep -q "components/messages/Hello" "$root/api/asyncapi.yaml"
grep -q "components/messages/Resume" "$root/api/asyncapi.yaml"
grep -q "components/messages/ResyncRequired" "$root/api/asyncapi.yaml"
grep -q '^  messages:' "$root/api/asyncapi.yaml"
for message_id in hello resume resync-required health-update log-update adapter-update plugin-update task-update message-update health-channel-update log-channel-update adapter-channel-update plugin-channel-update task-channel-update message-channel-update; do
  grep -q "messageId: $message_id" "$root/api/asyncapi.yaml"
done
duplicate_message_ids="$(sed -n 's/^[[:space:]]*messageId: //p' "$root/api/asyncapi.yaml" | sort | uniq -d)"
if [[ -n "$duplicate_message_ids" ]]; then
  echo "AsyncAPI messageId values must be unique: $duplicate_message_ids" >&2
  exit 1
fi

if ! command -v protoc >/dev/null 2>&1; then
  echo "protoc is required for descriptor validation; install it before running this test" >&2
  exit 2
fi

protoc \
  --proto_path="$proto_dir" \
  --descriptor_set_out="$tmp_dir/kiarabot-v1.pb" \
  --include_imports \
  "$proto_dir"/kiarabot/v1/*.proto
test -s "$tmp_dir/kiarabot-v1.pb"
if [[ -n "${KIARABOT_BASELINE_DESCRIPTOR:-}" ]]; then
  compatibility_tool="${KIARABOT_DESCRIPTOR_COMPATIBILITY_TOOL:-$root/build/proto/kiarabot_descriptor_compatibility}"
  if [[ ! -x "$compatibility_tool" ]]; then
    echo "descriptor compatibility tool is missing: $compatibility_tool" >&2
    echo "configure and build the proto preset before baseline validation" >&2
    exit 2
  fi
  "$compatibility_tool" "$tmp_dir/kiarabot-v1.pb" "$KIARABOT_BASELINE_DESCRIPTOR"
fi
echo "KiaraBot protocol contract checks passed"
