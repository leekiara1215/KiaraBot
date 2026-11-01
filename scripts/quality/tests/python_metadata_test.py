#!/usr/bin/env python3
"""Reject version, source and checksum drift in the pinned uv Python catalog mapping."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('python_metadata', ROOT / 'scripts/quality/verify-python-metadata.py')
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


class PythonMetadataTest(unittest.TestCase):
    def setUp(self):
        self.lock = json.loads((ROOT / 'dependencies/architecture/dependencies.lock.json').read_text())
        self.catalog = {}
        for artifact in self.lock['python']['artifacts'].values():
            self.catalog[artifact['metadata_key']] = {
                'major': 3, 'minor': 11, 'patch': 18, 'sha256': artifact['sha256'],
                'url': artifact['source_url'], 'build': artifact['build']}

    def test_selected_metadata_matches(self):
        self.assertEqual(VERIFY.metadata_errors(self.lock, self.catalog), [])

    def test_missing_or_drifted_metadata_fails(self):
        self.assertTrue(VERIFY.metadata_errors(self.lock, {}))
        for field, value in [('patch', 17), ('sha256', '0' * 64), ('url', 'https://wrong.invalid/'),
                             ('build', 'wrong-build')]:
            catalog = copy.deepcopy(self.catalog)
            catalog[next(iter(catalog))][field] = value
            with self.subTest(field=field):
                self.assertTrue(VERIFY.metadata_errors(self.lock, catalog))

    def test_binary_tampering_is_rejected_before_loading_decoder(self):
        with self.assertRaisesRegex(ValueError, 'binary digest'):
            VERIFY.catalog_from_binary(b'tampered', hashlib.sha256(b'original').hexdigest(),
                                       '0' * 64, Path('must-not-load'))


if __name__ == '__main__':
    unittest.main()
