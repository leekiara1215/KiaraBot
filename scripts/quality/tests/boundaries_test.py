#!/usr/bin/env python3
"""Check that the boundary gate detects direct outer-layer includes."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    'boundaries', Path(__file__).resolve().parents[1] / 'check-boundaries.py')
BOUNDARIES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BOUNDARIES)


class BoundariesTest(unittest.TestCase):
    def test_outer_includes_are_rejected_only_in_business_directories(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ('core/src', 'core/include', 'core/app'):
                (root / name).mkdir(parents=True)
            (root / 'core/app/main.cpp').write_text('#include "kiarabot/infrastructure/store.hpp"\n')
            (root / 'core/src/good.cpp').write_text('#include "kiarabot/core/ports.hpp"\n')
            self.assertEqual(list(BOUNDARIES.violations(root)), [])
            (root / 'core/include/bad.hpp').write_text('#include <sqlite3.h>\n#include <curl/curl.h>\n')
            errors = list(BOUNDARIES.violations(root))
            self.assertEqual(len(errors), 2)
            self.assertIn('sqlite3.h', errors[0])
            self.assertIn('curl/curl.h', errors[1])

    def test_domain_and_ports_reject_outer_core_layers(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            domain = root / 'core/include/kiarabot/core/domain'
            ports = root / 'core/include/kiarabot/core/ports'
            domain.mkdir(parents=True)
            ports.mkdir(parents=True)
            (domain / 'bad.hpp').write_text('#include "kiarabot/core/application/runtime.h"\n')
            (ports / 'bad.hpp').write_text('#include "kiarabot/core/runtime/bridge_server.hpp"\n')
            self.assertEqual(len(list(BOUNDARIES.violations(root))), 2)

    def test_outer_build_files_cannot_compile_core_sources(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            directory = root / 'infrastructure'
            directory.mkdir()
            (directory / 'CMakeLists.txt').write_text(
                'add_library(example ../core/src/runtime/runtime_log.cpp)\n')
            self.assertEqual(len(list(BOUNDARIES.violations(root))), 1)


    def test_one_hop_indirect_include_is_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'core/include/kiarabot/core/domain').mkdir(parents=True)
            (root / 'core/src').mkdir()
            (root / 'core/include/kiarabot/core/domain/dep.hpp').write_text('#include <sqlite3.h>\n')
            (root / 'core/src/uses.cpp').write_text('#include "kiarabot/core/domain/dep.hpp"\n')
            errors = list(BOUNDARIES.violations(root))
            self.assertTrue(
                any('indirect forbidden include' in error and 'uses.cpp' in error for error in errors),
                errors)

    def test_python_imports_are_checked_but_tests_are_exempt(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'sdk/python/src').mkdir(parents=True)
            (root / 'sdk/python/src/bad.py').write_text('from kiarabot_adapter_nonebot import main\n')
            (root / 'sdk/python/src/good.py').write_text('from kiarabot.v1 import events_pb2\n')
            self.assertEqual(len(list(BOUNDARIES.violations(root))), 1)
            (root / 'sdk/python/tests').mkdir(parents=True)
            (root / 'sdk/python/tests/test_fixture.py').write_text('import kiarabot_adapter_nonebot\n')
            self.assertEqual(len(list(BOUNDARIES.violations(root))), 1)

    def test_panel_must_not_import_repository_modules(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'panel/src').mkdir(parents=True)
            (root / 'core').mkdir()
            (root / 'panel/src/App.ts').write_text("import x from '../../core/x';\n")
            errors = list(BOUNDARIES.violations(root))
            self.assertEqual(len(errors), 1)
            self.assertIn('outside panel/', errors[0])

    def test_infrastructure_cannot_reference_control(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'infrastructure').mkdir()
            (root / 'infrastructure/CMakeLists.txt').write_text(
                'target_link_libraries(x PRIVATE kiarabot_control)\n')
            errors = list(BOUNDARIES.violations(root))
            self.assertEqual(len(errors), 1)
            self.assertIn('references control', errors[0])


if __name__ == '__main__':
    unittest.main()
