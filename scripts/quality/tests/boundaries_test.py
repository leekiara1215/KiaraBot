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


if __name__ == '__main__':
    unittest.main()
