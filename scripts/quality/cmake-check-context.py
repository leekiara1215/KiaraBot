#!/usr/bin/env python3
"""Read only the configured dependency context needed by locked checks."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

SELECTED = {'VCPKG_INSTALLED_DIR', '_VCPKG_INSTALLED_DIR', 'VCPKG_TARGET_TRIPLET',
            'CMAKE_TOOLCHAIN_FILE'}


def read_context(build_dir):
    build_dir = build_dir.resolve()
    values = {}
    for line in (build_dir / 'CMakeCache.txt').read_text().splitlines():
        if not line or line.startswith(('#', '//')) or '=' not in line:
            continue
        declaration, value = line.split('=', 1)
        if ':' not in declaration:
            continue
        key, _kind = declaration.rsplit(':', 1)
        if key not in SELECTED:
            continue
        if key in values:
            raise ValueError(f'duplicate selected CMake cache key: {key}')
        values[key] = value

    def absolute_path(key):
        value = values.get(key, '')
        if not value or any(ord(character) < 32 for character in value) or ';' in value:
            raise ValueError(f'missing/invalid configured path: {key}')
        path = Path(value)
        if not path.is_absolute():
            raise ValueError(f'configured path is not absolute: {key}')
        return path.resolve()

    installed = absolute_path('VCPKG_INSTALLED_DIR')
    if '_VCPKG_INSTALLED_DIR' in values and absolute_path('_VCPKG_INSTALLED_DIR') != installed:
        raise ValueError('configured vcpkg installation paths disagree')
    toolchain = absolute_path('CMAKE_TOOLCHAIN_FILE')
    if tuple(toolchain.parts[-3:]) != ('scripts', 'buildsystems', 'vcpkg.cmake'):
        raise ValueError('locked checks require the configured vcpkg CMake toolchain')
    triplet = values.get('VCPKG_TARGET_TRIPLET', '')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', triplet):
        raise ValueError('missing/invalid configured vcpkg triplet')
    identity = json.dumps([str(installed), str(toolchain), triplet], separators=(',', ':'))
    context_id = hashlib.sha256(identity.encode()).hexdigest()[:12]
    return {'installed-dir': str(installed), 'triplet': triplet, 'toolchain-file': str(toolchain),
            'registry-root': str(toolchain.parents[2]),
            'standalone-build-dir': str(build_dir / f'standalone-infrastructure-{context_id}')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--get', choices=['installed-dir', 'triplet', 'toolchain-file', 'registry-root',
                                       'standalone-build-dir'])
    args = parser.parse_args()
    context = read_context(args.build_dir)
    print(context[args.get] if args.get else json.dumps(context, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        print(f'locked CMake context failed: {error}', file=sys.stderr)
        sys.exit(1)
