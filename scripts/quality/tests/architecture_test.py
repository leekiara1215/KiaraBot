#!/usr/bin/env python3
"""Exercise architecture gates with missing targets and unexpected new directories."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    'architecture', Path(__file__).resolve().parents[1] / 'check-architecture.py')
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class ArchitectureTest(unittest.TestCase):
    def test_compile_inventory_includes_new_module_and_ignores_generated(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / 'new_service').mkdir()
            source = root / 'new_service/worker.cpp'
            source.write_text('int main() {}\n')
            (root / 'new_service/generated.pb.cc').write_text('generated\n')
            scheduled = GATE.sources(root)
            self.assertEqual(scheduled, [source])
            self.assertTrue(GATE.coverage_errors(root, [], scheduled))
            database = [{'directory': str(root), 'file': 'new_service/worker.cpp'}]
            self.assertEqual(GATE.coverage_errors(root, database, scheduled), [])
            self.assertTrue(GATE.coverage_errors(root, [], []))

    def test_inner_rejects_protocol_json_driver_and_sql(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            inner = root / 'core/src/application/identity'
            inner.mkdir(parents=True)
            source = inner / 'service.cpp'
            for include in ('kiarabot/v1/identity.pb.h', 'google/protobuf/struct.pb.h',
                            'grpcpp/grpcpp.h', 'nlohmann/json.hpp', 'pqxx/pqxx', 'libpq-fe.h'):
                source.write_text(f'#include <{include}>\n')
                with self.subTest(include=include):
                    self.assertTrue(list(GATE.inner_errors(root)))
            source.write_text('auto query = "SELECT id FROM accounts";\n')
            self.assertTrue(list(GATE.inner_errors(root)))
            source.write_text('#include <string>\n')
            self.assertEqual(list(GATE.inner_errors(root)), [])

    def test_missing_service_cannot_pass(self):
        model = {'configurations': [{'targets': [{'name': f'kiarabot-{s}'}
                                                for s in GATE.SERVICES]}]}
        self.assertEqual(GATE.target_errors(model), [])
        model['configurations'][0]['targets'].pop()
        self.assertEqual(len(GATE.target_errors(model)), 1)

    def test_transitive_private_outer_link_is_rejected(self):
        graph = {}
        for name in ('control', 'identity', 'runtime', 'host', 'scheduler', 'model'):
            graph[f'kiarabot_{name}_application'] = {'TYPE': ['STATIC_LIBRARY']}
        self.assertEqual(GATE.link_errors(graph, require_services=False), [])
        graph['kiarabot_identity_application']['LINK_LIBRARIES'] = ['$<LINK_ONLY:helper>']
        graph['helper'] = {'LINK_LIBRARIES': ['protobuf::libprotobuf']}
        self.assertTrue(GATE.link_errors(graph, require_services=False))
        graph['helper'] = {'KIARABOT_REFERENCE': ['TRUE']}
        self.assertTrue(GATE.link_errors(graph, require_services=False))

    def test_empty_or_unclassified_stage_fails(self):
        self.assertTrue(GATE.stage_errors({'tests': []}, 'p03'))
        tests = {'tests': [{'name': 'repository', 'properties': [
            {'name': 'LABELS', 'value': ['p03']}]}]}
        self.assertTrue(GATE.stage_errors(tests, 'p03'))
        tests['tests'][0]['properties'][0]['value'].append('integration')
        self.assertEqual(GATE.stage_errors(tests, 'p03'), [])


if __name__ == '__main__':
    unittest.main()
