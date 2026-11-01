#!/usr/bin/env python3
"""P02 gates: repository-wide compile coverage, inner dependencies and stage registration.

Target architecture checks are opt-in until P01/P02 replaces the old implementation.
No absent process, empty source inventory or missing stage counts as covered.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SERVICES = ('control', 'identity', 'runtime', 'plugin-host', 'scheduler', 'model')
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]', re.MULTILINE)
FORBIDDEN = re.compile(
    r'(?:\.pb\.(?:h|hpp)$|^google/protobuf/|^grpc|^nlohmann/|^boost/(?:json|beast|asio)/|'
    r'^rapidjson/|^json/|^pqxx|^libpq|^postgresql/|^sqlite3|^openssl/|^sodium|^curl/|'
    r'^kiarabot/(?:infrastructure|control)/|^kiarabot/core/runtime/)')


def sources(root):
    output = subprocess.check_output(['rg', '--files', '-0', '-g', '*.cpp', '-g', '*.cc',
                                      '-g', '*.cxx', '-g', '!*.pb.cc'], cwd=root)
    return sorted((root / name.decode()).resolve() for name in output.split(b'\0') if name)


def coverage_errors(root, database, scheduled):
    compiled = set()
    for entry in database:
        directory = Path(entry['directory'])
        if not directory.is_absolute():
            directory = root / directory
        file = Path(entry['file'])
        compiled.add((directory / file).resolve())
    errors = [f'no compile command: {path.relative_to(root)}' for path in scheduled
              if path.resolve() not in compiled]
    if not scheduled:
        errors.append('empty handwritten C++ inventory')
    return errors


def inner_errors(root):
    for base in ('core/include/kiarabot/core', 'core/src'):
        for layer in ('domain', 'application', 'ports'):
            directory = root / base / layer
            for path in sorted(directory.rglob('*')):
                if path.suffix not in {'.h', '.hpp', '.cpp', '.cc', '.cxx'}:
                    continue
                text = path.read_text()
                for match in INCLUDE.finditer(text):
                    include = match.group(1)
                    if FORBIDDEN.search(include):
                        line = text.count('\n', 0, match.start()) + 1
                        yield f'{path.relative_to(root)}:{line}: inner dependency {include}'
                # A lexical SQL guard complements include checking, not a SQL parser.
                if re.search(r'\b(?:SELECT\s+.+?\s+FROM|INSERT\s+INTO|UPDATE\s+\w+\s+SET|'
                             r'DELETE\s+FROM|CREATE\s+TABLE)\b', text, re.I | re.S):
                    yield f'{path.relative_to(root)}: SQL statement in inner layer'


def target_errors(codemodel):
    errors = []
    configurations = codemodel['configurations']
    if not configurations:
        return ['empty CMake codemodel']
    for config in configurations:
        targets = {target['name'] for target in config.get('targets', [])}
        errors.extend(f'P02 target absent: kiarabot-{service}' for service in SERVICES
                      if f'kiarabot-{service}' not in targets)
    return errors


def stage_errors(tests, stage):
    errors = []
    selected = []
    for test in tests['tests']:
        properties = {p['name']: p['value'] for p in test.get('properties', [])}
        labels = properties.get('LABELS', [])
        if stage in labels:
            selected.append(test['name'])
            if not set(labels) & {'unit', 'integration', 'fault'}:
                errors.append(f"{test['name']}: missing unit/integration/fault label")
    if not selected:
        errors.append(f'no tests registered for {stage}')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--list-sources0', action='store_true')
    parser.add_argument('--database', type=Path)
    parser.add_argument('--inner', action='store_true')
    parser.add_argument('--codemodel', type=Path, help='CMake file API codemodel-v2 JSON')
    parser.add_argument('--ctest-json', type=Path, help='ctest --show-only=json-v1 output')
    parser.add_argument('--stage', choices=[f'p{n:02}' for n in range(2, 15)])
    args = parser.parse_args()
    root = args.root.resolve()
    if args.list_sources0:
        for path in sources(root):
            sys.stdout.buffer.write(str(path).encode() + b'\0')
        return 0
    if not any((args.database, args.inner, args.codemodel, args.stage)):
        parser.error('select a gate; an empty check cannot pass')
    errors = []
    if args.database:
        errors.extend(coverage_errors(root, json.loads(args.database.read_text()), sources(root)))
    if args.inner:
        errors.extend(inner_errors(root))
    if args.codemodel:
        errors.extend(target_errors(json.loads(args.codemodel.read_text())))
    if args.stage:
        if not args.ctest_json:
            parser.error('--stage requires --ctest-json')
        errors.extend(stage_errors(json.loads(args.ctest_json.read_text()), args.stage))
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print('Selected architecture gates passed (not a lifecycle or PG acceptance test)')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f'architecture check failed: {error}', file=sys.stderr)
        sys.exit(1)
