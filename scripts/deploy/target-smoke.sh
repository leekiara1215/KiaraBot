#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

# P14a target deployment smoke/validation entrypoint.
#
# This host has no Docker (and CI runs may not either). The default mode performs
# all checks that do not require a Docker daemon:
#   * parses the three target Compose files (YAML + expected services);
#   * validates the suite Dockerfile target graph/static constraints;
#   * validates generated service JSON when present, and reminds about material;
#   * runs shell syntax checks and rejects obvious secret/privilege regressions.
# With Docker available it additionally runs `docker compose config` and, with
# --build, builds the target images. It NEVER runs database initialization,
# migrations or `docker compose up`; those are explicit operator steps.
#
# Exit codes: 0 checks passed (or Docker missing and not required), 1 failed
# check, 2 usage/config error.

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
docker_available=0
require_docker=0
do_build=0
release=0
env_file="${KIARABOT_ENV_FILE:-$repo_root/deploy/target.env}"

usage() {
  cat >&2 <<'USAGE'
usage: target-smoke.sh [--build] [--release] [--require-docker] [--env-file FILE]
  (no flags)     offline structural/config checks; if Docker exists also compose config
  --build        additionally build the target images (requires Docker)
  --release      enforce immutable image/digest requirements for release
  --require-docker fail instead of printing "UNVERIFIED" when Docker is absent
USAGE
}

while (($# > 0)); do
  case "$1" in
    --build) do_build=1 ;;
    --release) release=1 ;;
    --require-docker) require_docker=1 ;;
    --env-file) [[ $# -ge 2 ]] || { usage; exit 2; }; env_file="$2"; shift ;;
    --help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
  shift
done

fail() { echo "target-smoke: FAIL: $*" >&2; exit 1; }
note() { echo "target-smoke: $*"; }
command -v docker >/dev/null 2>&1 && docker_available=1

composes=(deploy/compose.infra.yml deploy/compose.core.yml deploy/compose.exec.yml)
expected_services=(
  "deploy/compose.infra.yml:postgres init-identity init-runtime init-scheduler init-model identity-bootstrap"
  "deploy/compose.core.yml:identity runtime scheduler model control panel"
  "deploy/compose.exec.yml:plugin-host adapter"
)

[[ -f "$repo_root/deploy/docker/suite.Dockerfile" ]] || fail "suite Dockerfile missing"
[[ -f "$repo_root/deploy/docker/adapter-entrypoint.sh" ]] || fail "adapter entrypoint missing"
[[ -f "$repo_root/scripts/deploy/target-initialize.sh" ]] || fail "target initializer missing"

command -v ruby >/dev/null 2>&1 || fail "ruby is required for YAML validation"
note "validating Compose YAML structure"
for spec in "${expected_services[@]}"; do
  file="${spec%%:*}"
  services="${spec#*:}"
  path="$repo_root/$file"
  [[ -f "$path" ]] || fail "missing $file"
  ruby -ryaml -e '
    document = YAML.load_file(ARGV[0], aliases: true)
    abort "no services map" unless document["services"].is_a?(Hash)
    expected = ARGV[1].split
    actual = document["services"].keys
    missing = expected - actual
    abort "missing services: #{missing.join(",")}" unless missing.empty?
    extra = actual - expected
    abort "unexpected services: #{extra.join(",")}" unless extra.empty?
  ' "$path" "$services" || fail "Compose parse/structure failed: $file"
done

note "validating Compose security defaults and no host networking"
if grep -RInE "^[[:space:]]*(network_mode:[[:space:]]*host|privileged:[[:space:]]*true)" \
    "$repo_root/deploy/compose.infra.yml" "$repo_root/deploy/compose.core.yml" \
    "$repo_root/deploy/compose.exec.yml"; then
  fail "target compose must not use host networking or privileged mode"
fi
for file in "${composes[@]}"; do
  grep -q "no-new-privileges:true" "$repo_root/$file" || fail "$file lacks no-new-privileges"
done
grep -q "USER 10001:10001" "$repo_root/deploy/docker/suite.Dockerfile" || fail "suite runtime is not non-root"
grep -q "read_only: true" "$repo_root/deploy/compose.core.yml" || fail "core services must be read-only"
grep -q "target: identity" "$repo_root/deploy/compose.core.yml" || fail "compose core does not build identity target"
for target in identity runtime scheduler model plugin-host control tools; do
  grep -qE "^FROM .* AS ${target}\$" "$repo_root/deploy/docker/suite.Dockerfile" \
    || fail "suite Dockerfile missing target stage: $target"
done
for binary in kiarabot-identity kiarabot-runtime kiarabot-scheduler kiarabot-model \
              kiarabot-plugin-host kiarabot-control kiarabot-pg-initialize \
              kiarabot-identity-bootstrap; do
  grep -q "$binary" "$repo_root/deploy/docker/suite.Dockerfile" \
    || fail "suite Dockerfile does not package $binary"
done

note "validating generated service JSON when present"
shopt -s nullglob
json_files=("$repo_root/deploy/target/config"/*.json)
shopt -u nullglob
if ((${#json_files[@]} > 0)); then
  command -v python3 >/dev/null 2>&1 || fail "python3 required for JSON validation"
  for file in "${json_files[@]}"; do
    python3 -m json.tool "$file" >/dev/null || fail "invalid JSON: ${file#"$repo_root"/}"
  done
  note "validated ${#json_files[@]} target config JSON file(s)"
else
  note "WARN: deploy/target/config/*.json not present; deployment material is operator-owned"
fi

note "running shell syntax checks"
for script in "$repo_root"/scripts/deploy/target-initialize.sh \
              "$repo_root"/scripts/deploy/target-smoke.sh \
              "$repo_root"/deploy/docker/adapter-entrypoint.sh; do
  bash -n "$script" || fail "shell syntax: ${script#"$repo_root"/}"
done

if ((release)); then
  [[ "$env_file" == *example* ]] && fail "--release must use a real env file, not the example"
  grep -qE '^UBUNTU_BASE=ubuntu:24\.04@sha256:[0-9a-f]{64}$' "$env_file" \
    || fail "release requires UBUNTU_BASE pinned by sha256 digest"
  for image_var in KIARABOT_IDENTITY_IMAGE KIARABOT_RUNTIME_IMAGE KIARABOT_SCHEDULER_IMAGE \
                   KIARABOT_MODEL_IMAGE KIARABOT_CONTROL_IMAGE KIARABOT_PLUGIN_HOST_IMAGE \
                   KIARABOT_PANEL_IMAGE KIARABOT_ADAPTER_IMAGE KIARABOT_TOOLS_IMAGE; do
    grep -qE "^${image_var}=.+/[^:]+@sha256:[0-9a-f]{64}\$" "$env_file" \
      || fail "release requires ${image_var} pinned by digest"
  done
  note "release digest checks passed"
fi

if ((docker_available)); then
  note "Docker found; validating merged Compose configuration"
  compose_env=()
  if [[ -f "$env_file" ]]; then
    compose_env=(--env-file "$env_file")
  elif [[ -f "$repo_root/deploy/target.env.example" ]]; then
    note "WARN: $env_file missing; using deploy/target.env.example for validation only"
    compose_env=(--env-file "$repo_root/deploy/target.env.example")
  fi
  docker compose "${compose_env[@]}" \
    -f "$repo_root/deploy/compose.infra.yml" \
    -f "$repo_root/deploy/compose.core.yml" \
    -f "$repo_root/deploy/compose.exec.yml" config -q \
    || fail "docker compose config rejected the target files"
  note "docker compose config passed (services are NOT started)"
  if ((do_build)); then
    for service in identity runtime scheduler model plugin-host control; do
      docker build -f "$repo_root/deploy/docker/suite.Dockerfile" \
        --target "$service" \
        --build-arg "UBUNTU_BASE=${UBUNTU_BASE:-ubuntu:24.04}" \
        -t "kiarabot-${service}:local" "$repo_root" \
        || fail "docker build failed for target $service"
    done
    note "target image build passed for suite services (runtime not exercised)"
  fi
  if command -v hadolint >/dev/null 2>&1; then
    hadolint "$repo_root/deploy/docker/suite.Dockerfile" || fail "hadolint failed"
  else
    note "hadolint not installed; Dockerfile lint only structural"
  fi
else
  note "UNVERIFIED: Docker is not installed on this host."
  note "UNVERIFIED: image build, compose config, healthchecks and runtime were NOT executed."
  note "Commands to run on a Docker-capable Linux host:"
  note "  deploy/target.env.example -> deploy/target.env; fill target config/tls/secrets"
  note "  ./scripts/deploy/target-smoke.sh --require-docker --build"
  note "  docker compose --env-file deploy/target.env -f deploy/compose.infra.yml -f deploy/compose.core.yml -f deploy/compose.exec.yml config"
  note "  (initialization: run scripts/deploy/target-initialize.sh via the init profile; never call up before it)"
  if ((require_docker)); then
    fail "Docker is required by --require-docker but unavailable"
  fi
fi

note "PASS (offline structural checks; deployment behavior remains UNVERIFIED until run on Linux/Docker)"
