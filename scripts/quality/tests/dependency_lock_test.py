#!/usr/bin/env python3
"""Mutation tests for dependency lock consistency."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('lock', ROOT / 'scripts/quality/check-dependency-lock.py')
LOCK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LOCK)


class DependencyLockTest(unittest.TestCase):
    def setUp(self):
        directory = ROOT / 'dependencies/architecture'
        self.lock = json.loads((directory / 'dependencies.lock.json').read_text())
        self.manifest = json.loads((directory / 'vcpkg.json').read_text())

    def test_committed_manifest_matches(self):
        self.assertEqual(LOCK.validate(self.lock, self.manifest), [])

    def test_drift_missing_override_and_invalid_hash_fail(self):
        for mutation in ('baseline', 'override', 'hash', 'server'):
            lock, manifest = copy.deepcopy(self.lock), copy.deepcopy(self.manifest)
            if mutation == 'baseline':
                manifest['builtin-baseline'] = '0' * 40
            elif mutation == 'override':
                manifest['overrides'].pop()
            elif mutation == 'hash':
                lock['vcpkg']['ports']['jwt-cpp']['git-tree'] = 'invented'
            else:
                lock['postgresql']['version'] = '0.0'
            with self.subTest(mutation=mutation):
                self.assertTrue(LOCK.validate(lock, manifest))

    def test_missing_and_mismatched_installed_metadata_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.assertTrue(LOCK.verify_installed(self.lock, directory, 'test-triplet'))
            for name, entry in self.lock['vcpkg']['ports'].items():
                path = directory / 'test-triplet/share' / name / 'vcpkg.spdx.json'
                path.parent.mkdir(parents=True)
                version = entry['version'] + (f"#{entry['port-version']}" if entry['port-version'] else '')
                path.write_text(json.dumps({'packages': [
                    {'SPDXID': 'SPDXRef-port', 'versionInfo': version}]}))
            self.assertEqual(LOCK.verify_installed(self.lock, directory, 'test-triplet'), [])
            path.write_text(json.dumps({'packages': [
                {'SPDXID': 'SPDXRef-port', 'versionInfo': 'wrong'}]}))
            self.assertTrue(LOCK.verify_installed(self.lock, directory, 'test-triplet'))


if __name__ == '__main__':
    unittest.main()
