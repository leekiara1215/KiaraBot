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
python3 "$proto_dir/tests/evidence_scopes_test.py"
python3 "$proto_dir/tests/check_implementation_status.py"
python3 "$proto_dir/tests/check_semantic_rules.py"

grep -q '^security:' "$root/api/openapi.yaml"
grep -q 'bearerAuth:' "$root/api/openapi.yaml"
grep -q 'Idempotency-Key' "$root/api/openapi.yaml"
grep -q 'trace_id:' "$root/api/openapi.yaml"
grep -q 'retryable:' "$root/api/openapi.yaml"
grep -q '^x-contract-status:' "$root/api/openapi.yaml"
grep -q '^x-contract-status:' "$root/api/asyncapi.yaml"
grep -q '^x-control-session-policy:' "$root/api/asyncapi.yaml"
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

include_args=(--proto_path="$proto_dir")
if [[ -n "${PROTOBUF_INCLUDE_DIR:-}" ]]; then
  include_args+=(--proto_path="$PROTOBUF_INCLUDE_DIR")
fi

protoc \
  "${include_args[@]}" \
  --descriptor_set_out="$tmp_dir/kiarabot-v1.pb" \
  --include_imports \
  "$proto_dir"/kiarabot/v1/*.proto
test -s "$tmp_dir/kiarabot-v1.pb"

# The six-case descriptor compatibility fixture and the legacy Python reader test
# were previously written but never invoked. Keep them wired to this entry point so
# incompatibility and old-reader drift cannot pass silently. Both fail closed when
# their build/runtime prerequisites are absent.
fixture_test="${KIARABOT_DESCRIPTOR_FIXTURE_TEST:-$proto_dir/tests/descriptor_compatibility_test.sh}"
if [[ ! -x "$fixture_test" ]]; then
  echo "descriptor compatibility fixture test is missing or not executable: $fixture_test" >&2
  exit 2
fi
"$fixture_test" "${KIARABOT_DESCRIPTOR_COMPATIBILITY_TOOL:-$root/build/proto/kiarabot_descriptor_compatibility}"

# The legacy Python reader test needs the locked SDK virtualenv and the generated
# Python package. Keep it opt-in so protoc-only environments and the unified local
# check (scripts/check-toolchain.sh) still work; CI and acceptance set
# KIARABOT_LEGACY_READER_TEST=1 to enforce it.
if [[ "${KIARABOT_LEGACY_READER_TEST:-0}" == "1" ]]; then
  python_bin="${KIARABOT_PYTHON:-$root/sdk/python/.venv/bin/python}"
  generated_python="${KIARABOT_GENERATED_PYTHON:-$root/sdk/python/build/generated}"
  if [[ ! -x "$python_bin" ]]; then
    echo "python SDK environment is required for the legacy reader test: $python_bin" >&2
    echo "create it with the locked SDK environment or set KIARABOT_PYTHON" >&2
    exit 2
  fi
  if [[ ! -d "$generated_python/kiarabot/v1" ]]; then
    echo "generated Python protocol package is required: $generated_python/kiarabot/v1" >&2
    echo "run sdk/python/tools/generate_proto.py or set KIARABOT_GENERATED_PYTHON" >&2
    exit 2
  fi
  PYTHONPATH="$generated_python" "$python_bin" "$proto_dir/tests/action_contract_test.py"
else
  echo "legacy Python reader test skipped; set KIARABOT_LEGACY_READER_TEST=1 to enforce it" >&2
fi

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
