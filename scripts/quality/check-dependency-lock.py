#!/usr/bin/env python3
"""Validate architecture locks offline; optionally verify an official registry checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def validate(lock, manifest):
    errors = []
    revision = lock['vcpkg']['revision']
    if not re.fullmatch(r'[a-f0-9]{40}', revision):
        errors.append('invalid registry revision')
    if manifest.get('builtin-baseline') != revision:
        errors.append('manifest baseline differs from lock')
    ports = lock['vcpkg']['ports']
    dependencies = {d if isinstance(d, str) else d['name'] for d in manifest['dependencies']}
    overrides = {d['name']: d for d in manifest['overrides']}
    if dependencies != set(ports) or set(overrides) != set(ports):
        errors.append('dependencies/overrides do not cover exactly the locked ports')
    for name, entry in ports.items():
        override = overrides.get(name, {})
        if (override.get('version'), override.get('port-version', 0)) != (
                entry['version'], entry['port-version']):
            errors.append(f'{name}: version override differs')
        for field, length in [('git-tree', 40), ('portfile-sha256', 64)]:
            if not re.fullmatch('[a-f0-9]{%d}' % length, entry[field]):
                errors.append(f'{name}: invalid {field}')
    if lock['postgresql']['version'] != ports['libpq']['version']:
        errors.append('server and libpq versions differ')
    if not re.fullmatch(r'[a-f0-9]{128}', lock['postgresql']['sha512']):
        errors.append('invalid PostgreSQL source hash')
    if len(lock['node']['sha256']) != 2 or any(
            not re.fullmatch(r'[a-f0-9]{64}', value) for value in lock['node']['sha256'].values()):
        errors.append('missing/invalid Node archive hashes')
    return errors


def verify_registry(lock, registry):
    errors = []
    def git(*args):
        return subprocess.check_output(['git', '-C', str(registry), *args], text=True).strip()
    if git('rev-parse', 'HEAD') != lock['vcpkg']['revision']:
        return ['registry HEAD does not match lock']
    baseline = json.loads((registry / 'versions/baseline.json').read_text())['default']
    for name, entry in lock['vcpkg']['ports'].items():
        if baseline[name] != {'baseline': entry['version'], 'port-version': entry['port-version']}:
            errors.append(f'{name}: registry baseline differs')
        if git('rev-parse', f'HEAD:ports/{name}') != entry['git-tree']:
            errors.append(f'{name}: registry tree differs')
        digest = hashlib.sha256((registry / f'ports/{name}/portfile.cmake').read_bytes()).hexdigest()
        if digest != entry['portfile-sha256']:
            errors.append(f'{name}: modified portfile')
    # Reject local changes anywhere: patches and transitive ports matter too.
    if git('status', '--porcelain', '--untracked-files=no'):
        errors.append('registry has tracked modifications')
    return errors


def verify_installed(lock, installed, triplet):
    errors = []
    for name, entry in lock['vcpkg']['ports'].items():
        metadata = installed / triplet / 'share' / name / 'vcpkg.spdx.json'
        if not metadata.is_file():
            errors.append(f'{name}: installed metadata absent for {triplet}')
            continue
        packages = json.loads(metadata.read_text())['packages']
        actual = next(package['versionInfo'] for package in packages
                      if package['SPDXID'] == 'SPDXRef-port')
        expected = entry['version'] + (f"#{entry['port-version']}" if entry['port-version'] else '')
        if actual != expected:
            errors.append(f'{name}: installed version {actual} differs from {expected}')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--registry', type=Path)
    parser.add_argument('--installed', type=Path)
    parser.add_argument('--triplet', help='required with --installed')
    parser.add_argument('--archive', type=Path, help='verify a downloaded PG or Node archive without extracting')
    args = parser.parse_args()
    directory = ROOT / 'dependencies/architecture'
    lock = json.loads((directory / 'dependencies.lock.json').read_text())
    errors = validate(lock, json.loads((directory / 'vcpkg.json').read_text()))
    if args.registry:
        errors.extend(verify_registry(lock, args.registry))
    if args.installed:
        if not args.triplet:
            parser.error('--installed requires --triplet')
        errors.extend(verify_installed(lock, args.installed, args.triplet))
    build_tools = json.loads((directory / 'build-tools.lock.json').read_text())['tools']
    for tool in build_tools:
        if not re.fullmatch(r'[a-f0-9]{64}', tool['sha256']):
            errors.append(f"{tool['name']}: invalid archive hash")
    if lock['openssl-cli']['version'] != lock['vcpkg']['ports']['openssl']['version']:
        errors.append('OpenSSL CLI/library versions differ')
    if args.archive:
        archives = {lock['postgresql']['url'].rsplit('/', 1)[1]: ('sha512', lock['postgresql']['sha512']),
                    lock['openssl-cli']['archive-name']: ('sha512', lock['openssl-cli']['sha512'])}
        archives.update({name: ('sha256', digest) for name, digest in lock['node']['sha256'].items()})
        archives.update({tool['url'].rsplit('/', 1)[1]: ('sha256', tool['sha256']) for tool in build_tools})
        if args.archive.name not in archives:
            raise ValueError('archive is not present in lock')
        algorithm, expected = archives[args.archive.name]
        digest = hashlib.new(algorithm)
        with args.archive.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
        if digest.hexdigest() != expected:
            errors.append('archive checksum mismatch')
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print('Architecture dependency lock consistent' + ('; registry verified' if args.registry else
          ' (offline only; installed dependencies not verified)'))
    if args.installed:
        print('Installed metadata matches lock; runtime/ABI requires dependency smoke')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f'dependency lock check failed: {error}', file=sys.stderr)
        sys.exit(1)
