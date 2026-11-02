# syntax=docker/dockerfile:1.7
# KiaraBot Core production image.
#
# The build stage uses the same pinned vcpkg toolchain as CI and installs C++
# dependencies from the locked P00 architecture manifest
# (dependencies/architecture, see doc/11-toolchain.md) so the image is built
# with the protocol and infrastructure adapters enabled
# (KIARABOT_GENERATE_PROTO=ON, KIARABOT_ENABLE_INFRASTRUCTURE=ON). The runtime
# stage contains kiarabot-core, the standalone migration command
# kiarabot-migrate, and the operations scripts, and runs as UID 10001.
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
ARG VCPKG_COMMIT=ce613c41372b23b1f51333815feb3edd87ef8a8b
ARG VCPKG_ROOT=/opt/vcpkg

# Configure-time probes need ruby and the openssl CLI; the locked libpq port
# needs bison/flex/perl while it builds the PostgreSQL client tools.
RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        autoconf automake bison build-essential ca-certificates cmake curl flex git \
        libtool openssl perl pkg-config python3 ruby unzip zip \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --quiet https://github.com/microsoft/vcpkg "$VCPKG_ROOT" \
    && git -C "$VCPKG_ROOT" checkout --quiet "$VCPKG_COMMIT" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics

# Install exclusively from the locked architecture manifest. A hand-maintained
# classic `vcpkg install <port>` list cannot express the exact override versions
# that dependencies/architecture/smoke requires (OpenSSL 3.5.0 EXACT,
# nlohmann_json 3.11.3 EXACT, libpqxx 7.10.0, ...). --x-install-root keeps the
# classic toolchain below pointed at the tree this command populated.
WORKDIR /src
COPY dependencies/architecture ./dependencies/architecture
RUN "$VCPKG_ROOT/vcpkg" install \
        --x-manifest-root=/src/dependencies/architecture \
        --triplet=x64-linux \
        --x-install-root="$VCPKG_ROOT/installed" \
        --clean-after-build

ENV PATH="$VCPKG_ROOT/installed/x64-linux/tools/protobuf:$VCPKG_ROOT/installed/x64-linux/tools/grpc:${PATH}"

COPY . .
# kiarabot-core is the OUTPUT_NAME of the kiarabot-reference-core target, whose
# binary directory is pinned by core/CMakeLists.txt to build/reference/core.
# kiarabot-migrate lives in build/reference/infrastructure because the root
# CMakeLists.txt pins that binary directory as well.
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DKIARABOT_GENERATE_PROTO=ON \
        -DKIARABOT_ENABLE_INFRASTRUCTURE=ON \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_MANIFEST_MODE=OFF \
        -DVCPKG_TARGET_TRIPLET=x64-linux \
    && cmake --build build --target kiarabot-reference-core kiarabot-migrate --parallel

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --no-install-recommends -y ca-certificates curl sqlite3 tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 kiarabot \
    && useradd --uid 10001 --gid 10001 --create-home --shell /usr/sbin/nologin kiarabot \
    && install -d -o 10001 -g 10001 /var/lib/kiarabot /var/lib/kiarabot/backups

# vcpkg builds most C++ dependencies as static libraries; copy any shared
# libraries it did produce so a dynamic link still resolves at runtime.
RUN --mount=type=bind,from=build,source=/opt/vcpkg/installed/x64-linux/lib,target=/tmp/vcpkg-lib \
    find /tmp/vcpkg-lib -maxdepth 1 -name 'lib*.so*' -exec cp -a {} /usr/local/lib/ \; \
    && ldconfig

COPY --from=build /src/build/reference/core/kiarabot-core /usr/local/bin/kiarabot-core
COPY --from=build /src/build/reference/infrastructure/kiarabot-migrate /usr/local/bin/kiarabot-migrate
COPY scripts/deploy/ /usr/local/bin/
RUN chmod 0755 /usr/local/bin/*.sh /usr/local/bin/kiarabot-core /usr/local/bin/kiarabot-migrate \
    && chown root:root /usr/local/bin/*.sh /usr/local/bin/kiarabot-core /usr/local/bin/kiarabot-migrate

USER 10001:10001
WORKDIR /var/lib/kiarabot
CMD ["/usr/local/bin/kiarabot-core"]
