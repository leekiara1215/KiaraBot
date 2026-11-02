#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
baseline_ref="${1:?usage: check-descriptor.sh BASELINE_COMMIT}"
if [[ "$baseline_ref" =~ ^0+$ ]] || ! git -C "$root" cat-file -e "${baseline_ref}^{commit}"; then
  echo "Cannot resolve a baseline commit: $baseline_ref" >&2
  exit 1
fi

# A baseline that resolves to the tested commit compares the working tree against
# itself and always passes. That was the false green on main, where the manual
# workflow default (origin/main) equals HEAD: the step looked green while proving
# nothing about cross-version compatibility. Fail closed and require an explicit
# baseline instead. KIARABOT_ALLOW_DESCRIPTOR_SELF_BASELINE=1 is only a local
# sanity switch; it never turns the run into a compatibility result.
head_sha="$(git -C "$root" rev-parse HEAD)"
baseline_sha="$(git -C "$root" rev-parse "${baseline_ref}^{commit}")"
if [[ "$baseline_sha" == "$head_sha" ]]; then
  if [[ "${KIARABOT_ALLOW_DESCRIPTOR_SELF_BASELINE:-0}" != "1" ]]; then
    echo "Refusing a descriptor self-comparison: baseline '$baseline_ref' resolves to HEAD ($head_sha)." >&2
    echo "Such a run only proves the gate itself, not cross-version protocol compatibility." >&2
    echo "Pass an earlier commit or release tag as BASELINE_COMMIT (workflow_dispatch input base_ref)." >&2
    echo "For a deliberate local sanity run only, set KIARABOT_ALLOW_DESCRIPTOR_SELF_BASELINE=1." >&2
    exit 1
  fi
  echo "WARNING: baseline '$baseline_ref' equals HEAD; this run proves the gate only, not compatibility." >&2
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
