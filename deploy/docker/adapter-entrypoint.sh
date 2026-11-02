#!/usr/bin/env sh
# Read secret files into environment without persisting them in the image or
# process argv. Used by compose.exec.yml so bootstrap/OneBot tokens stay
# read-only deployment mounts.
set -eu
if [ -n "${ONEBOT_ACCESS_TOKEN_FILE:-}" ] && [ -f "$ONEBOT_ACCESS_TOKEN_FILE" ]; then
  ONEBOT_ACCESS_TOKEN="$(cat "$ONEBOT_ACCESS_TOKEN_FILE")"
  export ONEBOT_ACCESS_TOKEN
fi
exec python -m kiarabot_adapter_nonebot
