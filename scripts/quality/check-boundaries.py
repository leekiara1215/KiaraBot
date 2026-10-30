#!/usr/bin/env python3
"""Check direct outer-layer dependencies in Core business sources."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
FORBIDDEN = re.compile(
    r'^(?:kiarabot/(?:infrastructure|control)/|sqlite3(?:\.h|/)|curl/|'
    r'boost/(?:beast|asio)(?:/|\.hpp)|Python\.h|nonebot|napcat|openai)', re.IGNORECASE
)


def violations(root: Path):
    for directory in (root / 'core/src', root / 'core/include'):
        for path in sorted(directory.rglob('*')):
            if path.suffix not in {'.h', '.hpp', '.cpp', '.cc'}:
                continue
            source = path.read_text(encoding='utf-8')
            for match in INCLUDE.finditer(source):
                included = match.group(1)
                relative = path.relative_to(root).as_posix()
                inner = '/kiarabot/core/domain/' in relative or '/core/src/domain/' in relative
                port = '/kiarabot/core/ports/' in relative or '/core/src/ports/' in relative
                backwards = (inner and re.match(r'^kiarabot/core/(application|ports|runtime)/', included)) or (
                    port and re.match(r'^kiarabot/core/(application|runtime)/', included))
                if FORBIDDEN.match(included) or backwards:
                    line = source.count('\n', 0, match.start()) + 1
                    yield f'{path.relative_to(root)}:{line}: forbidden include {match.group(1)}'
    for cmake in (root / 'control/api/CMakeLists.txt', root / 'infrastructure/CMakeLists.txt'):
        if not cmake.exists():
            continue
        for line_number, line in enumerate(cmake.read_text(encoding='utf-8').splitlines(), 1):
            if re.search(r'(?:\.\./|\$\{CMAKE_SOURCE_DIR\}/)core/src/', line):
                yield f'{cmake.relative_to(root)}:{line_number}: outer target references Core source'


if __name__ == '__main__':
    errors = list(violations(ROOT))
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        sys.exit(1)
    print('Core direct include boundaries passed')
