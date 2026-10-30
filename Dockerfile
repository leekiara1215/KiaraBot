# syntax=docker/dockerfile:1.7
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --no-install-recommends -y ca-certificates cmake g++ make python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target kiarabot-core --parallel

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --no-install-recommends -y ca-certificates curl sqlite3 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 kiarabot \
    && useradd --uid 10001 --gid 10001 --create-home --shell /usr/sbin/nologin kiarabot \
    && install -d -o 10001 -g 10001 /var/lib/kiarabot /var/lib/kiarabot/backups

COPY --from=build /src/build/core/kiarabot-core /usr/local/bin/kiarabot-core
COPY scripts/deploy/ /usr/local/bin/
RUN chmod 0755 /usr/local/bin/*.sh /usr/local/bin/kiarabot-core \
    && chown root:root /usr/local/bin/*.sh /usr/local/bin/kiarabot-core

USER 10001:10001
WORKDIR /var/lib/kiarabot
CMD ["/usr/local/bin/kiarabot-core"]
