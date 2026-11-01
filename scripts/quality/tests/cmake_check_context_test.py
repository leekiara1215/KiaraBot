#!/usr/bin/env python3
"""Regress externally reused prefixes and reject ambiguous locked-build contexts."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    'context', Path(__file__).resolve().parents[1] / 'cmake-check-context.py')
CONTEXT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONTEXT)


class CmakeContextTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.build = self.root / 'consumer/build/full-locked'
        self.build.mkdir(parents=True)
        self.prefix = self.root / 'other worktree/installed=prefix'
        self.toolchain = self.root / 'pinned tool/scripts/buildsystems/vcpkg.cmake'

    def write(self, prefix=None, extra='', triplet='arm64-osx'):
        (self.build / 'CMakeCache.txt').write_text(
            f'VCPKG_INSTALLED_DIR:UNINITIALIZED={prefix or self.prefix}\n'
            f'CMAKE_TOOLCHAIN_FILE:FILEPATH={self.toolchain}\n'
            f'VCPKG_TARGET_TRIPLET:STRING={triplet}\n' + extra)

    def test_external_prefix_spaces_and_equals_preserved(self):
        self.write()
        value = CONTEXT.read_context(self.build)
        self.assertEqual(value['installed-dir'], str(self.prefix))
        self.assertEqual(value['registry-root'], str(self.toolchain.parents[2]))
        self.assertEqual(value['triplet'], 'arm64-osx')
        self.assertTrue(Path(value['standalone-build-dir']).is_relative_to(self.build))

    def test_context_change_cannot_reuse_an_old_standalone_cache(self):
        self.write()
        original = CONTEXT.read_context(self.build)
        self.write(self.root / 'different/prefix')
        changed = CONTEXT.read_context(self.build)
        self.assertNotEqual(original['standalone-build-dir'], changed['standalone-build-dir'])

    def test_missing_relative_duplicate_conflicting_and_invalid_triplet_fail(self):
        cases = [('', {'prefix': 'relative/prefix'}),
                 ('VCPKG_INSTALLED_DIR:PATH=/duplicate\n', {}),
                 ('_VCPKG_INSTALLED_DIR:PATH=/wrong-prefix\n', {}),
                 ('', {'triplet': '../outside'}),
                 ('', {'triplet': ''})]
        for extra, arguments in cases:
            self.write(extra=extra, **arguments)
            with self.subTest(extra=extra, arguments=arguments), self.assertRaises(ValueError):
                CONTEXT.read_context(self.build)
        (self.build / 'CMakeCache.txt').write_text('unrelated:STRING=only\n')
        with self.assertRaises(ValueError):
            CONTEXT.read_context(self.build)

    def test_equivalent_internal_prefix_is_accepted(self):
        self.write(extra=f'_VCPKG_INSTALLED_DIR:PATH={self.prefix}/../{self.prefix.name}\n')
        self.assertEqual(CONTEXT.read_context(self.build)['installed-dir'], str(self.prefix))


if __name__ == '__main__':
    unittest.main()
