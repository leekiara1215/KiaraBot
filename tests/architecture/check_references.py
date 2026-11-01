"""Verify logical behavior anchors, following only the explicit byte-preserving Core manifest."""
import hashlib
import json
import re
from pathlib import Path, PurePosixPath


class ReferenceFailure(RuntimeError):
    pass


def blob_id(content):
    return hashlib.sha1(b'blob ' + str(len(content)).encode() + b'\0' + content).hexdigest()


def checked_path(root, relative):
    if not isinstance(relative, str):
        raise ReferenceFailure('reference path must be a string')
    path = PurePosixPath(relative)
    if path.is_absolute() or '..' in path.parts or path.as_posix() != relative or not path.parts:
        raise ReferenceFailure('reference path must be canonical and inside repository')
    candidate = root / relative
    # Do not follow a symlink to substitute an assertion or escape the checkout.
    current = root
    for part in path.parts:
        current = current / part
        if current.is_symlink():
            raise ReferenceFailure('reference path must not contain symlinks: ' + relative)
    return candidate


def manifest_mappings(root):
    manifest_path = checked_path(root, 'core/reference/manifest.json')
    if not manifest_path.exists():
        return {}
    manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict) or set(manifest) != {'base', 'files'}:
        raise ReferenceFailure('unknown reference manifest structure')
    if not isinstance(manifest['base'], str) or not re.fullmatch(r'[0-9a-f]{40}', manifest['base']):
        raise ReferenceFailure('invalid reference manifest base')
    if not isinstance(manifest['files'], list) or not manifest['files']:
        raise ReferenceFailure('empty or invalid reference manifest files')
    mappings = {}
    for entry in manifest['files']:
        if not isinstance(entry, dict) or set(entry) != {'source', 'reference', 'blob'}:
            raise ReferenceFailure('unknown reference mapping structure')
        logical, target = entry['source'], entry['reference']
        source_path = checked_path(root, logical)
        target_path = checked_path(root, target)
        if not logical.startswith(('core/app/', 'core/include/', 'core/src/', 'core/tests/')):
            raise ReferenceFailure('unknown Core logical source: ' + logical)
        if target != 'core/reference/' + logical.removeprefix('core/'):
            raise ReferenceFailure('unexpected reference mapping: ' + logical)
        if logical in mappings:
            raise ReferenceFailure('duplicate reference mapping: ' + logical)
        if not isinstance(entry['blob'], str) or not re.fullmatch(r'[0-9a-f]{40}', entry['blob']):
            raise ReferenceFailure('invalid reference blob: ' + logical)
        if source_path.exists():
            raise ReferenceFailure('mapped logical source still exists: ' + logical)
        if not target_path.is_file():
            raise ReferenceFailure('mapped reference missing: ' + target)
        if blob_id(target_path.read_bytes()) != entry['blob']:
            raise ReferenceFailure('mapped reference bytes changed: ' + target)
        mappings[logical] = target_path
    return mappings


def check_inventory(root, inventory):
    mappings = manifest_mappings(root)
    if inventory.get('schema_version') != 1 or inventory.get('status') != 'reference_only_not_target_pass':
        raise ReferenceFailure('unknown behavior inventory version/status')
    seen = set()
    for entry in inventory['entries']:
        if entry['id'] in seen:
            raise ReferenceFailure('duplicate reference behavior ID')
        seen.add(entry['id'])
        logical = entry['source']
        original = checked_path(root, logical)
        source = mappings.get(logical, original)
        if not source.is_file() or entry['anchor'] not in source.read_text():
            raise ReferenceFailure('reference source/anchor missing: ' + logical)
        if entry['status'] != 'pending_target_rebuild':
            raise ReferenceFailure('reference inventory must not claim target behavior passes')
        if not entry['invariant'] or not entry['target_phase'].startswith('p'):
            raise ReferenceFailure('reference invariant/target phase missing')
    return len(seen)


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[2]
    inventory = json.loads(Path(__file__).with_name('reference_behaviors.json').read_text())
    count = check_inventory(root, inventory)
    print(f'{count} logical reference behavior sources/anchors preserved; target rebuild pending')
