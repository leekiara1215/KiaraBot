# syntax=docker/dockerfile:1.7
#
# KiaraBot target six-service suite (P14a). This is NOT the reference single-Core
# image in the repository-root Dockerfile: it builds the P02 target binaries and
# the offline PostgreSQL/Identity bootstrap tools from the same locked P00
# dependency tree, then ships each service as a thin image on a shared runtime
# base.
#
# Build (from the repository root):
#   docker build -f deploy/docker/suite.Dockerfile \
#     --build-arg UBUNTU_BASE=ubuntu:24.04@sha256:<digest> \
#     --target runtime -t kiarabot-suite-runtime .
#   docker build ... --target identity -t kiarabot-identity .
#   ... same for runtime/scheduler/model/plugin-host/control/tools
#
# Reproducibility requirements (release checklist):
#   - UBUNTU_BASE must be passed as an immutable digest; the default tag is for
#     local builds only and is rejected by scripts/deploy/target-smoke.sh --release.
#   - VCPKG_COMMIT and the dependencies/architecture manifest are the P00 lock.
#   - Python 3.11.18 / uv 0.13.6 are the locked generation/runtime versions.
#   - The image never contains TLS private keys, passwords, root keys, enrollment
#     files or generated service JSON: those are read-only deployment mounts.
ARG UBUNTU_BASE=ubuntu:24.04
FROM ${UBUNTU_BASE} AS build

ENV DEBIAN_FRONTEND=noninteractive
ARG VCPKG_COMMIT=ce613c41372b23b1f51333815feb3edd87ef8a8b
ARG VCPKG_ROOT=/opt/vcpkg
ARG UV_VERSION=0.13.6
ARG PYTHON_VERSION=3.11.18

# Configure-time probes need ruby/openssl; the locked libpq port needs
# bison/flex/perl; proto generation needs a Python interpreter for uv.
RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        autoconf automake bison build-essential ca-certificates cmake curl flex git \
        libtool openssl perl pkg-config python3 python3-pip python3-venv ruby unzip zip \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --quiet https://github.com/microsoft/vcpkg "$VCPKG_ROOT" \
    && git -C "$VCPKG_ROOT" checkout --quiet "$VCPKG_COMMIT" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics

WORKDIR /src
COPY dependencies/architecture ./dependencies/architecture
# Install exactly the locked manifest; a classic port list cannot express
# OpenSSL 3.5.0 EXACT / nlohmann_json 3.11.3 EXACT / libpqxx 7.10.0.
RUN "$VCPKG_ROOT/vcpkg" install \
        --x-manifest-root=/src/dependencies/architecture \
        --triplet=x64-linux \
        --x-install-root="$VCPKG_ROOT/installed" \
        --clean-after-build
ENV PATH="$VCPKG_ROOT/installed/x64-linux/tools/protobuf:$VCPKG_ROOT/installed/x64-linux/tools/grpc:${PATH}"

# Locked Python used by KIARABOT_TARGET_PYTHON and the generated Python proto
# module. uv is pinned so the build does not resolve a floating interpreter.
RUN python3 -m venv /opt/kiarabot-uv \
    && /opt/kiarabot-uv/bin/pip install --no-cache-dir "uv==${UV_VERSION}" \
    && /opt/kiarabot-uv/bin/uv python install "${PYTHON_VERSION}"

COPY . .
RUN /opt/kiarabot-uv/bin/uv sync --project sdk/python --locked \
        --python "${PYTHON_VERSION}"

# Build the six service entrypoints + offline tools. `full` composition (not
# `dev`) is required for the real Host/Control/Runtime/PG adapters; proto
# generation uses the locked SDK venv above.
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DKIARABOT_GENERATE_PROTO=ON \
        -DKIARABOT_ENABLE_INFRASTRUCTURE=ON \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_MANIFEST_MODE=OFF \
        -DVCPKG_TARGET_TRIPLET=x64-linux \
        -DPython3_EXECUTABLE=/src/sdk/python/.venv/bin/python \
        -DKIARABOT_TARGET_PYTHON=/src/sdk/python/.venv/bin/python \
    && cmake --build build --parallel \
        --target kiarabot-control kiarabot-identity kiarabot-runtime \
                 kiarabot-plugin-host kiarabot-scheduler kiarabot-model \
                 kiarabot-pg-initialize kiarabot-identity-bootstrap

# --- Shared runtime base -----------------------------------------------------
FROM ${UBUNTU_BASE} AS runtime-base

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --no-install-recommends -y ca-certificates curl tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 kiarabot \
    && useradd --uid 10001 --gid 10001 --create-home --shell /usr/sbin/nologin kiarabot \
    && install -d -o 10001 -g 10001 /var/lib/kiarabot /var/lib/kiarabot/backups \
    && install -d -o 0 -g 0 -m 0755 /etc/kiarabot

# vcpkg builds most C++ dependencies as static libraries; copy any shared
# libraries so a dynamic link resolves at runtime.
RUN --mount=type=bind,from=build,source=/opt/vcpkg/installed/x64-linux/lib,target=/tmp/vcpkg-lib \
    find /tmp/vcpkg-lib -maxdepth 1 -name 'lib*.so*' -exec cp -a {} /usr/local/lib/ \; \
    && ldconfig

ENV KIARABOT_CONFIG_DIR=/etc/kiarabot \
    KIARABOT_TLS_DIR=/etc/kiarabot/tls \
    KIARABOT_SECRETS_DIR=/etc/kiarabot/secrets
USER 10001:10001
WORKDIR /var/lib/kiarabot

# --- Thin service images -----------------------------------------------------
# Each service only receives its own binary and default probe/health port. The
# compose files add read-only config/TLS/secrets mounts and resource limits.
FROM runtime-base AS identity
ENV KIARABOT_SERVICE=identity PROBE_PORT=9081
COPY --from=build /src/build/bin/kiarabot-identity /usr/local/bin/kiarabot-identity
HEALTHCHECK --interval=10s --timeout=3s --retries=6 --start-period=30s \
  CMD curl -fsS "http://127.0.0.1:${PROBE_PORT}/health/live" >/dev/null || exit 1
ENTRYPOINT ["/usr/local/bin/kiarabot-identity"]

FROM runtime-base AS runtime
ENV KIARABOT_SERVICE=runtime PROBE_PORT=9082
COPY --from=build /src/build/bin/kiarabot-runtime /usr/local/bin/kiarabot-runtime
HEALTHCHECK --interval=10s --timeout=3s --retries=6 --start-period=30s \
  CMD curl -fsS "http://127.0.0.1:${PROBE_PORT}/health/live" >/dev/null || exit 1
ENTRYPOINT ["/usr/local/bin/kiarabot-runtime"]

FROM runtime-base AS scheduler
ENV KIARABOT_SERVICE=scheduler PROBE_PORT=9083
COPY --from=build /src/build/bin/kiarabot-scheduler /usr/local/bin/kiarabot-scheduler
HEALTHCHECK --interval=10s --timeout=3s --retries=6 --start-period=30s \
  CMD curl -fsS "http://127.0.0.1:${PROBE_PORT}/health/live" >/dev/null || exit 1
ENTRYPOINT ["/usr/local/bin/kiarabot-scheduler"]

FROM runtime-base AS model
ENV KIARABOT_SERVICE=model PROBE_PORT=9084
COPY --from=build /src/build/bin/kiarabot-model /usr/local/bin/kiarabot-model
HEALTHCHECK --interval=10s --timeout=3s --retries=6 --start-period=30s \
  CMD curl -fsS "http://127.0.0.1:${PROBE_PORT}/health/live" >/dev/null || exit 1
ENTRYPOINT ["/usr/local/bin/kiarabot-model"]

FROM runtime-base AS plugin-host
ENV KIARABOT_SERVICE=plugin-host PROBE_PORT=9085
COPY --from=build /src/build/bin/kiarabot-plugin-host /usr/local/bin/kiarabot-plugin-host
HEALTHCHECK --interval=10s --timeout=3s --retries=6 --start-period=45s \
  CMD curl -fsS "http://127.0.0.1:${PROBE_PORT}/health/live" >/dev/null || exit 1
ENTRYPOINT ["/usr/local/bin/kiarabot-plugin-host"]

FROM runtime-base AS control
ENV KIARABOT_SERVICE=control PROBE_PORT=9080 CONTROL_PORT=8080
COPY --from=build /src/build/bin/kiarabot-control /usr/local/bin/kiarabot-control
HEALTHCHECK --interval=10s --timeout=3s --retries=6 --start-period=30s \
  CMD curl -fsS "http://127.0.0.1:${CONTROL_PORT}/health/live" >/dev/null || exit 1
ENTRYPOINT ["/usr/local/bin/kiarabot-control"]

# Offline tools image: empty-database initializer + Identity bootstrap. It is
# executed explicitly (one-shot profile / operator command); no daemon runs DDL
# and no container starts these automatically.
FROM runtime-base AS tools
COPY --from=build /src/build/bin/kiarabot-identity-bootstrap /usr/local/bin/kiarabot-identity-bootstrap
COPY --from=build /src/build/infrastructure/postgres/kiarabot-pg-initialize /usr/local/bin/kiarabot-pg-initialize
COPY scripts/deploy/ /usr/local/bin/deploy-scripts/
USER 10001:10001
ENTRYPOINT ["/usr/local/bin/kiarabot-pg-initialize"]
