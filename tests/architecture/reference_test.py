"""Manifest resolution before/after migration; no production/reference files are moved."""
import copy
import json
import tempfile
import unittest
from pathlib import Path

from check_references import ReferenceFailure, blob_id, check_inventory


class ReferenceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='kiarabot-reference-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.logical = 'core/tests/example_test.cpp'
        self.target = 'core/reference/tests/example_test.cpp'
        self.content = b'void preserved_assertion() { check(true); }\n'
        self.inventory = {'schema_version': 1, 'status': 'reference_only_not_target_pass', 'entries': [
            {'id': 'preserved', 'source': self.logical, 'anchor': 'void preserved_assertion()',
             'status': 'pending_target_rebuild', 'invariant': 'retain assertion', 'target_phase': 'p06'}]}
        self.manifest = {'base': 'd20b54bcc4ea01df023acb54cd528b4a0764b3db', 'files': [
            {'source': self.logical, 'reference': self.target, 'blob': blob_id(self.content)}]}

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def migrated(self, manifest=None):
        self.write(self.target, self.content)
        self.write('core/reference/manifest.json', json.dumps(manifest or self.manifest).encode())

    def test_before_migration_uses_original_logical_source(self):
        self.write(self.logical, self.content)
        self.assertEqual(check_inventory(self.root, self.inventory), 1)

    def test_after_migration_uses_exact_manifest_with_same_logical_anchor(self):
        original_inventory = copy.deepcopy(self.inventory)
        self.migrated()
        self.assertEqual(check_inventory(self.root, self.inventory), 1)
        self.assertEqual(self.inventory, original_inventory)

    def test_no_manifest_or_missing_mapping_never_guesses_destination(self):
        self.write(self.target, self.content)
        with self.assertRaisesRegex(ReferenceFailure, 'source/anchor missing'):
            check_inventory(self.root, self.inventory)
        manifest = copy.deepcopy(self.manifest)
        manifest['files'][0]['source'] = 'core/tests/other.cpp'
        manifest['files'][0]['reference'] = 'core/reference/tests/other.cpp'
        self.migrated(manifest)
        self.write('core/reference/tests/other.cpp', self.content)
        with self.assertRaisesRegex(ReferenceFailure, 'source/anchor missing'):
            check_inventory(self.root, self.inventory)

    def test_missing_destination_changed_bytes_or_anchor_are_rejected(self):
        self.migrated()
        target = self.root / self.target
        target.unlink()
        with self.assertRaisesRegex(ReferenceFailure, 'reference missing'):
            check_inventory(self.root, self.inventory)
        target.write_bytes(b'different assertion')
        with self.assertRaisesRegex(ReferenceFailure, 'bytes changed'):
            check_inventory(self.root, self.inventory)
        target.write_bytes(self.content)
        self.inventory['entries'][0]['anchor'] = 'deleted_assertion'
        with self.assertRaisesRegex(ReferenceFailure, 'source/anchor missing'):
            check_inventory(self.root, self.inventory)

    def test_unknown_incorrect_duplicate_and_escaping_mappings_are_rejected(self):
        mutations = [
            lambda m: m['files'][0].update(reference='core/reference/tests/wrong.cpp'),
            lambda m: m['files'][0].update(source='sdk/python/tests/example.py'),
            lambda m: m['files'][0].update(reference='../outside.cpp'),
            lambda m: m['files'][0].update(extra='unknown'),
            lambda m: m['files'].append(copy.deepcopy(m['files'][0])),
            lambda m: m.update(extra='unknown'),
        ]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                manifest = copy.deepcopy(self.manifest)
                mutate(manifest)
                self.migrated(manifest)
                with self.assertRaises(ReferenceFailure):
                    check_inventory(self.root, self.inventory)

    def test_reintroduced_original_and_symlink_substitution_are_rejected(self):
        self.migrated()
        original = self.write(self.logical, self.content)
        with self.assertRaisesRegex(ReferenceFailure, 'still exists'):
            check_inventory(self.root, self.inventory)
        original.unlink()
        target = self.root / self.target
        target.unlink()
        other = self.write('elsewhere.cpp', self.content)
        target.symlink_to(other)
        with self.assertRaisesRegex(ReferenceFailure, 'symlink'):
            check_inventory(self.root, self.inventory)


if __name__ == '__main__':
    unittest.main()
