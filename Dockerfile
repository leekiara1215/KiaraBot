# syntax=docker/dockerfile:1.7
# KiaraBot Core production image.
#
# The build stage uses the same pinned vcpkg toolchain as CI so the image is
# built with the protocol and infrastructure adapters enabled
# (KIARABOT_GENERATE_PROTO=ON, KIARABOT_ENABLE_INFRASTRUCTURE=ON). The runtime
# stage contains kiarabot-core, the standalone migration command
# kiarabot-migrate, and the operations scripts, and runs as UID 10001.
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
ARG VCPKG_COMMIT=ce613c41372b23b1f51333815feb3edd87ef8a8b
ARG VCPKG_ROOT=/opt/vcpkg

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        autoconf automake build-essential ca-certificates cmake curl git libtool \
        pkg-config python3 unzip zip \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --quiet https://github.com/microsoft/vcpkg "$VCPKG_ROOT" \
    && git -C "$VCPKG_ROOT" checkout --quiet "$VCPKG_COMMIT" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics \
    && "$VCPKG_ROOT/vcpkg" install --clean-after-build \
        protobuf grpc curl sqlite3 abseil boost-beast boost-asio libsodium

ENV PATH="$VCPKG_ROOT/installed/x64-linux/tools/protobuf:$VCPKG_ROOT/installed/x64-linux/tools/grpc:${PATH}"

WORKDIR /src
COPY . .
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DKIARABOT_GENERATE_PROTO=ON \
        -DKIARABOT_ENABLE_INFRASTRUCTURE=ON \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_MANIFEST_MODE=OFF \
        -DVCPKG_TARGET_TRIPLET=x64-linux \
    && cmake --build build --target kiarabot-core kiarabot-migrate --parallel

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

COPY --from=build /src/build/core/kiarabot-core /usr/local/bin/kiarabot-core
COPY --from=build /src/build/infrastructure/kiarabot-migrate /usr/local/bin/kiarabot-migrate
COPY scripts/deploy/ /usr/local/bin/
RUN chmod 0755 /usr/local/bin/*.sh /usr/local/bin/kiarabot-core /usr/local/bin/kiarabot-migrate \
    && chown root:root /usr/local/bin/*.sh /usr/local/bin/kiarabot-core /usr/local/bin/kiarabot-migrate

USER 10001:10001
WORKDIR /var/lib/kiarabot
CMD ["/usr/local/bin/kiarabot-core"]
