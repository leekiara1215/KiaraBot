#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
command -v clang-format >/dev/null 2>&1 || { echo 'clang-format is required' >&2; exit 1; }
command -v rg >/dev/null 2>&1 || { echo 'ripgrep (rg) is required by check-format' >&2; exit 1; }
# Respect .gitignore so local build trees and dependency environments are excluded.
rg --files -0 -g '*.h' -g '*.hpp' -g '*.cc' -g '*.cpp' -g '*.cxx' \
  -g '!*.pb.h' -g '!*.pb.cc' | xargs -0 clang-format --dry-run --Werror
