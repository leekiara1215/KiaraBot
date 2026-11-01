#!/usr/bin/env python3
"""Verify that the explicit reference migration retains byte-identical source blobs."""
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def errors(root):
    manifest = json.loads((root / 'core/reference/manifest.json').read_text())
    if not manifest['files']:
        yield 'empty reference manifest'
    for entry in manifest['files']:
        source, destination = root / entry['source'], root / entry['reference']
        if source.exists():
            yield f'reference source reintroduced into production tree: {entry["source"]}'
        if not destination.is_file():
            yield f'reference missing: {entry["reference"]}'
            continue
        content = destination.read_bytes()
        digest = hashlib.sha1(b'blob ' + str(len(content)).encode() + b'\0' + content).hexdigest()
        if digest != entry['blob']:
            yield f'reference changed from recorded base: {entry["reference"]}'


if __name__ == '__main__':
    failures = list(errors(ROOT))
    if failures:
        print('\n'.join(failures), file=sys.stderr)
        sys.exit(1)
    print('Reference migration blobs preserved; not target business acceptance')
