# syntax=docker/dockerfile:1.7
# KiaraBot NoneBot2 adapter image. Built from the repository root context so the
# pinned proto sources are available for generating the Python stubs:
#   docker build -f deploy/docker/adapter.Dockerfile -t kiarabot-adapter-nonebot .
# The adapter binds its health probe to 127.0.0.1 and connects to Core at
# CORE_GRPC_ENDPOINT (also 127.0.0.1 under the host-network deployment model).
FROM python:3.11-slim AS build

ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
ARG UV_VERSION=0.13.6

RUN apt-get update \
    && apt-get install --no-install-recommends -y build-essential ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && pip install --no-cache-dir "uv==${UV_VERSION}"

WORKDIR /app
COPY adapters/nonebot/ /app/
COPY proto/ /proto/
RUN uv sync --frozen --project /app \
    && cd /app \
    && .venv/bin/python tools/generate_proto.py /proto

FROM python:3.11-slim AS runtime

RUN apt-get update \
    && apt-get install --no-install-recommends -y ca-certificates curl tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 kiarabot \
    && useradd --uid 10001 --gid 10001 --create-home --shell /usr/sbin/nologin kiarabot \
    && install -d -o 10001 -g 10001 /var/lib/kiarabot-adapter

WORKDIR /app
COPY --from=build /app /app
ENV PYTHONPATH=/app/src:/app/generated \
    PATH=/app/.venv/bin:$PATH \
    PYTHONUNBUFFERED=1 \
    OUTBOX_PATH=/var/lib/kiarabot-adapter/outbox.db

USER 10001:10001
CMD ["python", "-m", "kiarabot_adapter_nonebot"]
