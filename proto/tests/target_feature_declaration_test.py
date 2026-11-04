"""Target feature rollout declarations cannot silently become production availability."""
import contextlib
import copy
import io
import unittest
from unittest.mock import patch

import check_implementation_status as gate


class TargetFeatureDeclarationTest(unittest.TestCase):
    def reject(self, mutate, expected):
        registry = copy.deepcopy(gate.read_registry())
        declaration = registry['vocabulary']['target_features']['host.model.stream.v1']
        mutate(declaration)
        output = io.StringIO()
        with patch.object(gate, 'read_registry', return_value=registry):
            with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                result = gate.main()
        self.assertEqual(result, 1)
        self.assertIn(expected, output.getvalue())

    def test_declaration_does_not_upgrade_target_implementation(self):
        self.reject(lambda d: d.update(status='implemented'),
                    'status differs from its target semantic evidence')

    def test_new_feature_cannot_invent_permission(self):
        self.reject(lambda d: d.update(permission='model.admin'), 'names an undeclared permission')

    def test_declaration_cannot_enable_advertisement(self):
        self.reject(lambda d: d.update(advertise='always'), 'must not advertise from declaration alone')

    def test_reference_source_is_not_target_declaration(self):
        self.reject(lambda d: d.update(declaration_source='core/reference/manifest.json'),
                    'has an invalid canonical declaration source')

    def test_feature_binds_known_target_semantic_evidence(self):
        self.reject(lambda d: d.update(semantic_rule='semantic.unknown'), 'names an unknown semantic rule')


if __name__ == '__main__':
    unittest.main()
