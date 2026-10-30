#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
command -v clang-format >/dev/null 2>&1 || { echo 'clang-format is required' >&2; exit 1; }
find . \( -type d \( -name .git -o -name build -o -name node_modules \) -prune \) -o \
  \( -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' \) \
     ! -name '*.pb.h' ! -name '*.pb.cc' -print0 \) | xargs -0 clang-format --dry-run --Werror
