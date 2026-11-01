"""Regression for reference provenance and independent target production claims."""
import copy
import json
import tempfile
import unittest
from pathlib import Path

from evidence_scopes import EvidenceFailure, EvidenceResolver, effective_status, validate_scopes
from check_implementation_status import implementation_text


def git_blob(content):
    import hashlib
    return hashlib.sha1(b'blob ' + str(len(content)).encode() + b'\0' + content).hexdigest()


class ScopeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='kiarabot-contract-evidence-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.logical = 'core/src/application/action_router.cpp'
        self.reference = 'core/reference/src/application/action_router.cpp'
        self.content = b'const char* legacy = "message.send";\n'
        self.manifest = {'base': 'd20b54bcc4ea01df023acb54cd528b4a0764b3db', 'files': [
            {'source': self.logical, 'reference': self.reference, 'blob': git_blob(self.content)}]}
        self.registry = {'categories': {'operations': {'old': 'implemented', 'new': 'not_implemented',
                                                     'removed': 'deprecated'}},
                         'execution_scopes': {
                             'reference': {'status_source': 'categories', 'production': False},
                             'target': {'default_status': 'not_implemented', 'overrides': {}, 'evidence': {}}}}

    def write(self, path, data):
        path = self.root / path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def migrated(self, manifest=None):
        self.write(self.reference, self.content)
        self.write('core/reference/manifest.json', json.dumps(manifest or self.manifest).encode())

    def test_reference_evidence_before_and_after_exact_manifest_migration(self):
        original = self.write(self.logical, self.content)
        self.assertEqual(EvidenceResolver(self.root).resolve(self.logical), original)
        original.unlink()
        self.migrated()
        resolver = EvidenceResolver(self.root)
        self.assertEqual(resolver.resolve(self.logical), self.root / self.reference)
        self.assertFalse(resolver.is_target_source(self.logical))
        self.assertFalse(resolver.is_target_source(self.reference))

    def test_missing_or_wrong_mapping_never_guesses_reference_file(self):
        self.write(self.reference, self.content)
        with self.assertRaisesRegex(EvidenceFailure, 'file missing'):
            EvidenceResolver(self.root).resolve(self.logical)
        wrong = copy.deepcopy(self.manifest)
        wrong['files'][0]['reference'] = 'core/reference/wrong.cpp'
        self.migrated(wrong)
        with self.assertRaisesRegex(EvidenceFailure, 'incorrect reference mapping'):
            EvidenceResolver(self.root)

    def test_manifest_missing_file_changed_blob_duplicate_and_escape_fail(self):
        self.migrated()
        target = self.root / self.reference
        target.unlink()
        with self.assertRaisesRegex(EvidenceFailure, 'destination missing'):
            EvidenceResolver(self.root)
        target.write_bytes(b'changed')
        with self.assertRaisesRegex(EvidenceFailure, 'blob changed'):
            EvidenceResolver(self.root)
        target.write_bytes(self.content)
        for key, value in [('source', '../outside'), ('reference', '../outside'), ('extra', 'unknown')]:
            bad = copy.deepcopy(self.manifest)
            bad['files'][0][key] = value
            self.migrated(bad)
            with self.assertRaises(EvidenceFailure):
                EvidenceResolver(self.root)
        bad = copy.deepcopy(self.manifest)
        bad['files'].append(copy.deepcopy(bad['files'][0]))
        self.migrated(bad)
        with self.assertRaisesRegex(EvidenceFailure, 'duplicate'):
            EvidenceResolver(self.root)

    def test_symlinks_and_reintroduced_source_fail(self):
        self.migrated()
        self.write(self.logical, self.content)
        with self.assertRaisesRegex(EvidenceFailure, 'reintroduced'):
            EvidenceResolver(self.root)
        (self.root / self.logical).unlink()
        (self.root / self.reference).unlink()
        elsewhere = self.write('elsewhere.cpp', self.content)
        (self.root / self.reference).symlink_to(elsewhere)
        with self.assertRaisesRegex(EvidenceFailure, 'symlink'):
            EvidenceResolver(self.root)

    def test_reference_implemented_is_preserved_but_never_implicitly_target(self):
        before = copy.deepcopy(self.registry['categories'])
        validate_scopes(self.registry, EvidenceResolver(self.root))
        self.assertEqual(effective_status(self.registry, 'reference', 'operations', 'old'), 'implemented')
        self.assertEqual(effective_status(self.registry, 'target', 'operations', 'old'), 'not_implemented')
        self.assertEqual(effective_status(self.registry, 'target', 'operations', 'removed'), 'deprecated')
        self.assertEqual(before, self.registry['categories'])

    def target_claim(self, paths):
        target = self.registry['execution_scopes']['target']
        target['overrides'] = {'operations': {'new': 'implemented'}}
        target['evidence'] = {'operations:new': paths}

    def test_target_claim_requires_current_production_source_and_target_test(self):
        self.migrated()
        test = 'tests/architecture/run.py'
        production = 'core/src/runtime/platform/service_process.cpp'
        self.write(test, b'# target integration assertions\n')
        self.write(production, b'// current production boundary\n')
        for paths in ([self.logical, test], [production], [test]):
            self.target_claim(paths)
            with self.assertRaises(EvidenceFailure):
                validate_scopes(self.registry, EvidenceResolver(self.root))
        self.target_claim([production, test])
        validate_scopes(self.registry, EvidenceResolver(self.root))
        self.assertEqual(effective_status(self.registry, 'target', 'operations', 'new'), 'implemented')
        self.target_claim([production, test, self.logical])
        with self.assertRaisesRegex(EvidenceFailure, 'cannot certify target'):
            validate_scopes(self.registry, EvidenceResolver(self.root))

    def test_unknown_scope_item_and_production_defaults_fail_closed(self):
        resolver = EvidenceResolver(self.root)
        for mutate in [lambda r: r['execution_scopes']['reference'].update(production=True),
                       lambda r: r['execution_scopes']['target'].update(default_status='implemented'),
                       lambda r: r['execution_scopes']['target'].update(overrides={'unknown': {}}),
                       lambda r: r['execution_scopes']['target'].update(overrides={'operations': {'unknown': 'implemented'}}),
                       lambda r: r['execution_scopes']['target'].update(overrides={'operations': {'removed': 'implemented'}})]:
            broken = copy.deepcopy(self.registry)
            mutate(broken)
            with self.assertRaises(EvidenceFailure):
                validate_scopes(broken, resolver)
        with self.assertRaises(EvidenceFailure):
            effective_status(self.registry, 'unknown', 'operations', 'old')

    def test_reference_vocabulary_and_test_strings_do_not_certify_target(self):
        self.migrated()
        self.write('core/tests/platform/example.py', b'feature_test_only')
        self.write('core/src/runtime/platform/example.cpp', b'feature_current_boundary')
        self.assertIn('message.send', implementation_text('reference', self.root))
        current = implementation_text('target', self.root)
        self.assertNotIn('message.send', current)
        self.assertNotIn('feature_test_only', current)
        self.assertIn('feature_current_boundary', current)

    def test_real_registry_only_probes_are_currently_available_in_target(self):
        root = Path(__file__).resolve().parents[2]
        registry = json.loads((root / 'proto/tests/contract_status.json').read_text())
        validate_scopes(registry, EvidenceResolver(root))
        available = {(category, key) for category, values in registry['categories'].items() for key in values
                     if effective_status(registry, 'target', category, key) == 'implemented'}
        self.assertEqual(available, {('operations', 'operations.getProcessLiveness'),
                                     ('operations', 'operations.getProcessReadiness')})
        self.assertEqual(effective_status(registry, 'reference', 'action_payloads', 'ActionEnvelope.send_message'), 'implemented')
        self.assertEqual(effective_status(registry, 'target', 'action_payloads', 'ActionEnvelope.send_message'), 'not_implemented')


if __name__ == '__main__':
    unittest.main()
