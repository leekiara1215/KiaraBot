#!/usr/bin/env python3
"""P08d Host credential_directory helper: permissions, idempotency, rotation, cleanup."""
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
HELPER = ROOT / 'scripts/deploy/target_host_credential.py'
INSTANCE = '11111111-1111-4111-8111-111111111111'
INSTANCE_TWO = '22222222-2222-4222-8222-222222222222'
CREDENTIAL_ONE = 'a' * 64
CREDENTIAL_TWO = 'b' * 64


class HostCredentialTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name)
        self.credentials = self.path / 'credentials'
        self.credentials.mkdir(mode=0o700)
        self.response = self.path / 'response.json'

    def tearDown(self):
        self.directory.cleanup()

    def run_helper(self, *arguments, stdin=None, response=None):
        if response is not None:
            self.response.write_text(json.dumps(response))
            self.response.chmod(0o600)
            arguments = (*arguments, '--response', str(self.response))
        return subprocess.run(
            [str(HELPER), *arguments],
            input=stdin,
            text=True,
            capture_output=True,
            check=False,
        )

    def payload(self, instance=INSTANCE, credential=CREDENTIAL_ONE):
        return {'instance': {'instance_id': instance}, 'bootstrap_credential': credential}

    def target(self, instance=INSTANCE):
        return self.credentials / f'{instance}.bootstrap'

    def assert_secret_absent(self, *outputs):
        for output in outputs:
            self.assertNotIn(CREDENTIAL_ONE, output)
            self.assertNotIn(CREDENTIAL_TWO, output)

    def test_store_writes_exact_0600_file_and_is_idempotent(self):
        first = self.run_helper('store', '--directory', str(self.credentials),
                                response=self.payload())
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertIn(f'STORED {INSTANCE}', first.stdout)
        self.assert_secret_absent(first.stdout, first.stderr)
        information = self.target().lstat()
        self.assertTrue(stat.S_ISREG(information.st_mode))
        self.assertEqual(stat.S_IMODE(information.st_mode), 0o600)
        self.assertEqual(self.target().read_text(), CREDENTIAL_ONE + '\n')
        second = self.run_helper('store', '--directory', str(self.credentials),
                                 response=self.payload())
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn(f'UNCHANGED {INSTANCE}', second.stdout)
        self.assert_secret_absent(second.stdout, second.stderr)

    def test_rotation_requires_replace_and_never_leaks(self):
        self.assertEqual(self.run_helper('store', '--directory', str(self.credentials),
                                         response=self.payload()).returncode, 0)
        refused = self.run_helper('store', '--directory', str(self.credentials),
                                  response=self.payload(credential=CREDENTIAL_TWO))
        self.assertEqual(refused.returncode, 3)
        self.assertIn('REFUSED_ROTATION', refused.stderr)
        self.assertEqual(self.target().read_text(), CREDENTIAL_ONE + '\n')
        self.assert_secret_absent(refused.stdout, refused.stderr)
        replaced = self.run_helper('store', '--directory', str(self.credentials), '--replace',
                                   response=self.payload(credential=CREDENTIAL_TWO))
        self.assertEqual(replaced.returncode, 0, replaced.stderr)
        self.assertIn(f'ROTATED {INSTANCE}', replaced.stdout)
        self.assertEqual(self.target().read_text(), CREDENTIAL_TWO + '\n')
        self.assertEqual(stat.S_IMODE(self.target().lstat().st_mode), 0o600)
        self.assert_secret_absent(replaced.stdout, replaced.stderr)

    def test_bind_adapter_response_and_explicit_instance(self):
        self.response.write_text(json.dumps({
            'binding': {'adapter_instance_id': INSTANCE_TWO},
            'bootstrap_credential': CREDENTIAL_ONE,
        }))
        self.response.chmod(0o600)
        result = subprocess.run(
            [str(HELPER), 'store', '--directory', str(self.credentials),
             '--response', str(self.response)], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.target(INSTANCE_TWO).read_text(), CREDENTIAL_ONE + '\n')
        override = self.run_helper('store', '--directory', str(self.credentials),
                                   '--instance-id', INSTANCE_TWO, '--replace',
                                   response={'bootstrap_credential': CREDENTIAL_TWO})
        self.assertEqual(override.returncode, 0, override.stderr)

    def test_prune_keeps_active_and_removes_temporary_files(self):
        for instance in (INSTANCE, INSTANCE_TWO):
            result = self.run_helper('store', '--directory', str(self.credentials),
                                     response=self.payload(instance=instance))
            self.assertEqual(result.returncode, 0, result.stderr)
        temporary = self.credentials / '.target-host-credential-orphan'
        temporary.write_text('temporary')
        active = self.path / 'active.json'
        active.write_text(json.dumps([INSTANCE]))
        pruned = self.run_helper('prune', '--directory', str(self.credentials),
                                 '--active', str(active))
        self.assertEqual(pruned.returncode, 0, pruned.stderr)
        self.assertIn(f'REMOVED {INSTANCE_TWO}', pruned.stdout)
        self.assertIn('PRUNED 1', pruned.stdout)
        self.assertTrue(self.target(INSTANCE).is_file())
        self.assertFalse(self.target(INSTANCE_TWO).exists())
        self.assertFalse(temporary.exists())
        dry = self.run_helper('prune', '--directory', str(self.credentials),
                              '--active', str(active), '--dry-run')
        self.assertEqual(dry.returncode, 0, dry.stderr)

    def test_rejects_unsafe_inputs_and_symlink_targets(self):
        missing = self.run_helper('store', '--directory', str(self.credentials),
                                  response={'instance': {'instance_id': INSTANCE}})
        self.assertEqual(missing.returncode, 2)
        self.assertIn('MISSING_CREDENTIAL', missing.stderr)
        self.assertFalse(self.target().exists())
        self.response.write_text(json.dumps(self.payload()))
        self.response.chmod(0o644)
        unsafe_response = subprocess.run(
            [str(HELPER), 'store', '--directory', str(self.credentials),
             '--response', str(self.response)], capture_output=True, text=True, check=False)
        self.assertEqual(unsafe_response.returncode, 2)
        self.assertIn('INVALID_RESPONSE', unsafe_response.stderr)
        self.target().symlink_to(self.path / 'elsewhere')
        linked = self.run_helper('store', '--directory', str(self.credentials),
                                 response=self.payload())
        self.assertEqual(linked.returncode, 2)
        self.assertIn('UNSAFE_TARGET', linked.stderr)
        verify = self.run_helper('verify', '--directory', str(self.credentials))
        self.assertEqual(verify.returncode, 2)
        self.assertIn('UNSAFE_TARGET', verify.stderr)

    def test_verify_reports_healthy_directory(self):
        self.assertEqual(self.run_helper('store', '--directory', str(self.credentials),
                                         response=self.payload()).returncode, 0)
        verified = self.run_helper('verify', '--directory', str(self.credentials))
        self.assertEqual(verified.returncode, 0, verified.stderr)
        self.assertIn('VERIFIED 1', verified.stdout)
        self.assert_secret_absent(verified.stdout, verified.stderr)


if __name__ == '__main__':
    unittest.main()
