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

    def test_header_filter_covers_new_handwritten_directories_not_ignored_downloads(self):
        import re
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / 'new_module').mkdir()
            (root / 'build').mkdir()
            (root / '.ignore').write_text('build/\n')
            own = root / 'new_module/value.hpp'
            downloaded = root / 'build/third_party.hpp'
            own.write_text('#pragma once\n')
            downloaded.write_text('downloaded\n')
            pattern = GATE.header_filter(root)
            self.assertIsNotNone(re.search(pattern, str(own)))
            self.assertIsNone(re.search(pattern, str(downloaded)))

    def test_inner_rejects_protocol_json_driver_and_sql(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            inner = root / 'core/src/application/identity'
            inner.mkdir(parents=True)
            source = inner / 'service.cpp'
            for include in ('kiarabot/v1/identity.pb.h', 'google/protobuf/struct.pb.h',
                            'grpcpp/grpcpp.h', 'nlohmann/json.hpp', 'pqxx/pqxx', 'libpq-fe.h',
                            'openssl/sha.h', 'openssl/evp.h'):
                source.write_text(f'#include <{include}>\n')
                with self.subTest(include=include):
                    self.assertTrue(list(GATE.inner_errors(root)))
            source.write_text('auto query = "SELECT id FROM accounts";\n')
            self.assertTrue(list(GATE.inner_errors(root)))
            source.write_text('#include <string>\n')
            self.assertEqual(list(GATE.inner_errors(root)), [])

    def test_control_application_is_checked_and_can_include_its_own_header(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            inner = root / 'control/api/src/application/target'
            inner.mkdir(parents=True)
            source = inner / 'application.cpp'
            source.write_text('#include "kiarabot/control/application/target/application.hpp"\n'
                              '#include "kiarabot/control/application/target/readiness.hpp"\n')
            self.assertEqual(list(GATE.inner_errors(root)), [])
            for include in ('nlohmann/json.hpp', 'kiarabot/v1/runtime.pb.h', 'pqxx/pqxx',
                            'kiarabot/control/target/https_endpoint.hpp',
                            'kiarabot/control/target/readiness.hpp'):
                source.write_text(f'#include <{include}>\n')
                self.assertTrue(list(GATE.inner_errors(root)))

    def test_missing_service_cannot_pass(self):
        model = {'configurations': [{'targets': [{'name': f'kiarabot-{s}'}
                                                for s in GATE.SERVICES]}]}
        self.assertEqual(GATE.target_errors(model), [])
        model['configurations'][0]['targets'].pop()
        self.assertEqual(len(GATE.target_errors(model)), 1)

    def test_transitive_private_outer_link_is_rejected(self):
        graph = {}
        for name in ('control', 'identity', 'runtime', 'plugin_host', 'scheduler', 'model'):
            graph[f'kiarabot_{name}_application'] = {
                'TYPE': ['STATIC_LIBRARY'], 'KIARABOT_LAYER': ['INNER']}
        self.assertEqual(GATE.link_errors(graph, require_services=False), [])
        graph['kiarabot_identity_application']['LINK_LIBRARIES'] = ['$<LINK_ONLY:helper>']
        graph['helper'] = {'LINK_LIBRARIES': ['protobuf::libprotobuf']}
        self.assertTrue(GATE.link_errors(graph, require_services=False))
        graph['helper'] = {'KIARABOT_REFERENCE': ['TRUE']}
        self.assertTrue(GATE.link_errors(graph, require_services=False))

    def test_arbitrary_business_target_must_register_its_layer(self):
        for source, directory in (
                ('src/application/target/new_use_case.cpp', '/repo/core'),
                ('/repo/core/src/domain/target/rules.cpp', '/repo'),
                ('/repo/core/include/kiarabot/core/ports/target/new.hpp', '/repo'),
                ('src/application/target/new.cpp', '/repo/control/api')):
            with self.subTest(source=source):
                graph = {'new_business_name': {'SOURCE_DIR': [directory], 'SOURCES': [source]}}
                errors = GATE.link_errors(graph, require_services=False)
                self.assertTrue(any('missing INNER layer' in error for error in errors))
                # Reference marking cannot conceal target business sources.
                graph['new_business_name']['KIARABOT_REFERENCE'] = ['TRUE']
                self.assertTrue(any('missing INNER layer' in error for error in
                                    GATE.link_errors(graph, require_services=False)))
                graph['new_business_name'].pop('KIARABOT_REFERENCE')
                graph['new_business_name']['KIARABOT_LAYER'] = ['INNER']
                self.assertEqual(GATE.link_errors(graph, require_services=False), [])
                graph['new_business_name']['LINK_LIBRARIES'] = ['$<LINK_ONLY:helper>']
                graph['helper'] = {'INTERFACE_LINK_LIBRARIES': ['OpenSSL::Crypto']}
                self.assertTrue(any('crypto' in error for error in
                                    GATE.link_errors(graph, require_services=False)))

    def test_inner_library_name_may_contain_hyphens(self):
        graph = {'kiarabot-new-business': {'TYPE': ['STATIC_LIBRARY'],
                                           'KIARABOT_LAYER': ['INNER']}}
        self.assertEqual(GATE.link_errors(graph, require_services=False), [])

    def test_registered_inner_targets_cannot_leak_interface_include_paths(self):
        graph = {'rules': {'KIARABOT_LAYER': ['INNER'],
                           'INTERFACE_INCLUDE_DIRECTORIES': ['/repo/generated']}}
        self.assertTrue(GATE.link_errors(graph, require_services=False))

    def test_production_infrastructure_allowed_but_reference_still_rejected(self):
        graph = {f'kiarabot-{service}': {'TYPE': ['EXECUTABLE']} for service in GATE.SERVICES}
        graph['rules'] = {'KIARABOT_LAYER': ['INNER']}
        graph['kiarabot-plugin-host']['LINK_LIBRARIES'] = ['kiarabot_host_journal']
        graph['kiarabot-model']['LINK_LIBRARIES'] = ['kiarabot_model_http_provider']
        graph['kiarabot_host_journal'] = {'LINK_LIBRARIES': ['SQLite3::SQLite3']}
        graph['kiarabot_model_http_provider'] = {'LINK_LIBRARIES': ['CURL::libcurl']}
        self.assertEqual(GATE.link_errors(graph), [])
        graph['kiarabot_host_journal']['KIARABOT_REFERENCE'] = ['TRUE']
        self.assertTrue(any('production links reference' in error for error in
                            GATE.link_errors(graph)))
        graph['kiarabot_host_journal'].pop('KIARABOT_REFERENCE')
        graph['kiarabot-model']['LINK_LIBRARIES'].append('historical')
        graph['historical'] = {'SOURCES': ['/repo/core/reference/src/old.cpp']}
        self.assertTrue(any('production links reference' in error for error in
                            GATE.link_errors(graph)))

    def test_empty_or_unclassified_stage_fails(self):
        self.assertTrue(GATE.stage_errors({'tests': []}, 'p03'))
        tests = {'tests': [{'name': 'repository', 'properties': [
            {'name': 'LABELS', 'value': ['p03']}]}]}
        self.assertTrue(GATE.stage_errors(tests, 'p03'))
        tests['tests'][0]['properties'][0]['value'].append('integration')
        self.assertEqual(GATE.stage_errors(tests, 'p03'), [])


if __name__ == '__main__':
    unittest.main()
