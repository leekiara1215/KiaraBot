#!/usr/bin/env python3
"""Check module boundaries across C++, Python, TypeScript and build files.

This is the machine-checkable subset of doc/00-boundaries.md:

  * Core business code (core/src, core/include) must not include outer/platform
    code, and domain/ports must not include more outward Core layers;
  * one-hop transitive includes: a Core source that includes a repository header
    which itself includes a forbidden dependency is reported as indirect;
  * control/api must not reference infrastructure or Core sources, and
    infrastructure must not reference the control layer;
  * the Python SDK must not import adapter/panel code, adapters must not import
    SDK/plugin code, plugins must not import adapter/control/infrastructure or a
    database client;
  * the TypeScript panel must not import implementation modules outside panel/.

Only direct imports/includes are parsed (plus one hop for C++ headers); this is a
static gate, not a full transitive build-graph audit.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
PY_IMPORT = re.compile(r'^\s*(?:from\s+([A-Za-z_][\w.]*)|import\s+([A-Za-z_][\w.]*))', re.MULTILINE)
TS_IMPORT = re.compile(r"""(?:from\s+|require\(\s*)['"]([^'"]+)['"]""")
FORBIDDEN = re.compile(
    r'^(?:kiarabot/(?:infrastructure|control)/|sqlite3(?:\.h|/)|curl/|'
    r'boost/(?:beast|asio)(?:/|\.hpp)|Python\.h|nonebot|napcat|openai)', re.IGNORECASE)
CXX_SUFFIXES = {'.h', '.hpp', '.cpp', '.cc'}

# (base directory, forbidden import regex, message)
PYTHON_RULES = (
    ('sdk',
     re.compile(r'^(?:kiarabot_adapter_nonebot|adapters|control|panel|kiarabot_infrastructure)(?:\.|$)'),
     'SDK imports adapter/control/panel/infrastructure code'),
    ('adapters',
     re.compile(r'^(?:kiarabot_sdk|sdk|plugins|kiarabot_control|control|panel)(?:\.|$)'),
     'adapter imports SDK/plugin/control/panel code'),
    ('plugins',
     re.compile(r'^(?:kiarabot_adapter_nonebot|adapters|kiarabot_control|control|panel|'
                r'kiarabot_infrastructure|sqlite3|psycopg2?|redis)(?:\.|$)'),
     'plugin imports adapter/control/panel/DB client code'),
)


def _line_of(source: str, offset: int) -> int:
    return source.count('\n', 0, offset) + 1


def _header_index(root: Path):
    headers = {}
    for base in (root / 'core/include', root / 'core/src'):
        if not base.is_dir():
            continue
        for path in base.rglob('*'):
            if path.is_file() and path.suffix in {'.h', '.hpp'}:
                try:
                    headers[path.resolve()] = path.read_text(encoding='utf-8')
                except OSError:
                    continue
    return headers


def _resolve_local_header(root: Path, included: str, current: Path, headers):
    candidates = [current.parent / included]
    if included.startswith('kiarabot/'):
        candidates.append(root / 'core/include' / included)
    for candidate in candidates:
        try:
            resolved = candidate.resolve()
        except OSError:
            continue
        if resolved in headers:
            return resolved
    return None


def _cpp_violations(root: Path):
    headers = _header_index(root)
    for directory in (root / 'core/src', root / 'core/include'):
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob('*')):
            if path.suffix not in CXX_SUFFIXES or not path.is_file():
                continue
            source = path.read_text(encoding='utf-8')
            relative = path.relative_to(root).as_posix()
            for match in INCLUDE.finditer(source):
                included = match.group(1)
                inner = '/kiarabot/core/domain/' in relative or '/core/src/domain/' in relative
                port = '/kiarabot/core/ports/' in relative or '/core/src/ports/' in relative
                backwards = (inner and re.match(r'^kiarabot/core/(application|ports|runtime)/', included)) or (
                    port and re.match(r'^kiarabot/core/(application|runtime)/', included))
                if FORBIDDEN.match(included) or backwards:
                    yield f'{relative}:{_line_of(source, match.start())}: forbidden include {included}'
                    continue
                if path.suffix not in {'.h', '.hpp'} and not re.search(r'\.(?:c|cc|cpp|cxx|h|hpp)$', included):
                    continue
                header = _resolve_local_header(root, included, path, headers)
                if header is None:
                    continue
                header_source = headers[header]
                for nested in INCLUDE.finditer(header_source):
                    nested_name = nested.group(1)
                    if FORBIDDEN.match(nested_name):
                        yield (f'{relative}:{_line_of(source, match.start())}: indirect forbidden include '
                               f'{nested_name} via {_header_label(root, header)}')


def _header_label(root: Path, header: Path) -> str:
    try:
        return header.relative_to(root).as_posix()
    except ValueError:
        return header.name


def _python_violations(root: Path):
    for base_name, pattern, message in PYTHON_RULES:
        base = root / base_name
        if not base.is_dir():
            continue
        for path in sorted(base.rglob('*.py')):
            if not path.is_file():
                continue
            # Tests may compose several modules on purpose (integration fixtures);
            # the dependency rules apply to shipped module code.
            if 'tests' in path.parts or path.name.startswith('test_'):
                continue
            source = path.read_text(encoding='utf-8')
            relative = path.relative_to(root).as_posix()
            for match in PY_IMPORT.finditer(source):
                module = match.group(1) or match.group(2)
                if pattern.match(module):
                    yield f'{relative}:{_line_of(source, match.start())}: {message}: {module}'


def _typescript_violations(root: Path):
    base = root / 'panel'
    if not base.is_dir():
        return
    root_resolved = root.resolve()
    base_resolved = base.resolve()
    for path in sorted(base.rglob('*')):
        if not path.is_file() or path.suffix not in {'.ts', '.tsx', '.js', '.jsx', '.mjs', '.cjs'}:
            continue
        if 'node_modules' in path.parts or '/dist/' in path.as_posix():
            continue
        source = path.read_text(encoding='utf-8')
        relative = path.relative_to(root).as_posix()
        for match in TS_IMPORT.finditer(source):
            specifier = match.group(1)
            if specifier.startswith('kiarabot'):
                yield f'{relative}:{_line_of(source, match.start())}: panel imports implementation module {specifier}'
                continue
            if not specifier.startswith('.'):
                continue
            try:
                resolved = (path.parent / specifier).resolve()
                is_inside_repo = resolved.is_relative_to(root_resolved)
                is_inside_panel = resolved.is_relative_to(base_resolved)
            except OSError:
                continue
            if is_inside_repo and not is_inside_panel:
                yield (f'{relative}:{_line_of(source, match.start())}: panel imports code outside panel/: '
                       f'{specifier}')


def _cmake_violations(root: Path):
    rules = (
        (root / 'control/api/CMakeLists.txt',
         re.compile(r'(?:^|[^A-Za-z_])(infrastructure|kiarabot_sqlite|kiarabot_model|kiarabot_time_zone)'),
         'control target references infrastructure'),
        (root / 'infrastructure/CMakeLists.txt',
         re.compile(r'(?:^|[^A-Za-z_])(control|kiarabot_control)(?:[^A-Za-z_]|$)'),
         'infrastructure target references control'),
    )
    for cmake, pattern, message in rules:
        if not cmake.exists():
            continue
        for line_number, line in enumerate(cmake.read_text(encoding='utf-8').splitlines(), 1):
            if re.search(r'(?:\.\./|\$\{CMAKE_SOURCE_DIR\}/)core/src/', line):
                yield f'{cmake.relative_to(root)}:{line_number}: outer target references Core source'
            elif pattern.search(line) and 'core/include' not in line:
                yield f'{cmake.relative_to(root)}:{line_number}: {message}'


def violations(root: Path):
    yield from _cpp_violations(root)
    yield from _python_violations(root)
    yield from _typescript_violations(root)
    yield from _cmake_violations(root)


if __name__ == '__main__':
    errors = list(violations(ROOT))
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        sys.exit(1)
    print('Module dependency boundaries passed (C++, Python, panel, build files)')
