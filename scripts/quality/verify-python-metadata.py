#!/usr/bin/env python3
"""Verify Python archive digests against the catalog embedded in the pinned uv binary.

This validates expected digests, not downloaded Python archive bodies. The native
uv binary and decompressed catalog must match their recorded provenance digests.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
MAGIC = b'\x28\xb5\x2f\xfd'


def catalog_from_binary(binary, expected_binary_digest, expected_catalog_digest, library):
    if hashlib.sha256(binary).hexdigest() != expected_binary_digest:
        raise ValueError('uv native binary digest differs from recorded source')
    decoder = ctypes.CDLL(str(library))
    decoder.ZSTD_findFrameCompressedSize.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    decoder.ZSTD_findFrameCompressedSize.restype = ctypes.c_size_t
    decoder.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                     ctypes.c_void_p, ctypes.c_size_t]
    decoder.ZSTD_decompress.restype = ctypes.c_size_t
    decoder.ZSTD_isError.argtypes = [ctypes.c_size_t]
    decoder.ZSTD_isError.restype = ctypes.c_uint
    offset = 0
    while True:
        offset = binary.find(MAGIC, offset)
        if offset < 0:
            raise ValueError('recorded uv catalog not found')
        source = ctypes.create_string_buffer(binary[offset:])
        size = decoder.ZSTD_findFrameCompressedSize(source, len(binary) - offset)
        offset += len(MAGIC)
        if decoder.ZSTD_isError(size) or size > 4 * 1024 * 1024:
            continue
        capacity = 32 * 1024 * 1024
        output = ctypes.create_string_buffer(capacity)
        length = decoder.ZSTD_decompress(output, capacity, source, size)
        if decoder.ZSTD_isError(length):
            continue
        content = output.raw[:length]
        if hashlib.sha256(content).hexdigest() == expected_catalog_digest:
            return json.loads(content)


def metadata_errors(lock, catalog):
    errors = []
    version = tuple(int(part) for part in lock['python']['version'].split('.'))
    for key, artifact in lock['python']['artifacts'].items():
        source = catalog.get(artifact['metadata_key'])
        if source is None:
            errors.append(f'{key}: source metadata missing')
            continue
        if tuple(source.get(part) for part in ('major', 'minor', 'patch')) != version:
            errors.append(f'{key}: source version differs')
        if source.get('sha256') != artifact['sha256']:
            errors.append(f'{key}: archive digest differs')
        if source.get('url') != artifact['source_url']:
            errors.append(f'{key}: archive source URL differs')
        if source.get('build') != artifact['build']:
            errors.append(f'{key}: standalone build differs')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--uv', required=True, type=Path)
    parser.add_argument('--zstd-library', required=True, type=Path)
    args = parser.parse_args()
    lock = json.loads((ROOT / 'dependencies/architecture/dependencies.lock.json').read_text())
    provenance = lock['python']['digest_provenance']
    catalog = catalog_from_binary(args.uv.read_bytes(), provenance['uv_binary_sha256'],
                                  provenance['catalog_sha256'], args.zstd_library)
    errors = metadata_errors(lock, catalog)
    if errors:
        raise ValueError('; '.join(errors))
    print('Python expected archive hashes match the pinned uv catalog; archive bodies not verified')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print(f'Python metadata verification failed: {error}', file=sys.stderr)
        sys.exit(1)
