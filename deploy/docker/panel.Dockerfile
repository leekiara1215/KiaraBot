# syntax=docker/dockerfile:1.7
# KiaraBot control panel image: static Vite build served by nginx, which also
# reverse-proxies /api/ and /ws/ to the loopback-bound control API.
#   docker build -f deploy/docker/panel.Dockerfile \
#     --build-arg VITE_API_MODE=live -t kiarabot-panel .
FROM node:22-bookworm-slim AS build

WORKDIR /src
COPY api/ /src/api/
COPY panel/ /src/panel/
WORKDIR /src/panel
RUN npm ci --no-audit --no-fund

ARG VITE_API_MODE=live
ARG VITE_API_BASE=/api/v1
ARG VITE_WS_URL=
ENV VITE_API_MODE=${VITE_API_MODE} \
    VITE_API_BASE=${VITE_API_BASE} \
    VITE_WS_URL=${VITE_WS_URL}
RUN npm run build

FROM nginxinc/nginx-unprivileged:1.27-alpine AS runtime

USER root
RUN apk add --no-cache curl
COPY --from=build /src/panel/dist /usr/share/nginx/html
COPY deploy/docker/panel-nginx.conf.template /etc/nginx/templates/default.conf.template
ENV KIARABOT_CONTROL_HOST=127.0.0.1 \
    KIARABOT_CONTROL_PORT=8080 \
    KIARABOT_PANEL_PORT=8082 \
    NGINX_ENVSUBST_FILTER=KIARABOT_

USER 101
EXPOSE 8082
