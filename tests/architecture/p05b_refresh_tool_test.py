#!/usr/bin/env python3
"""F-3 unit tests: refresh tool rejects stale/over-age administrator tokens with re-auth hint."""
from __future__ import annotations
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / 'scripts/deploy/instance-delegation-refresh'


def jwt(exp: int) -> str:
    def segment(value):
        return base64.urlsafe_b64encode(
            json.dumps(value, separators=(',', ':')).encode()).rstrip(b'=').decode()
    return segment({'alg': 'EdDSA', 'typ': 'JWT'}) + '.' + segment(
        {'sub': 'admin', 'exp': exp, 'iat': exp - 60}) + '.sig'


class RefreshTokenTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='kiarabot-p05b13-')
        self.root = Path(self.directory.name)
        self.root.chmod(0o700)
        self.config = self.root / 'config.json'
        self.config.write_text(json.dumps({
            'identity_endpoint': '127.0.0.1:1', 'ca_file': str(self.root / 'ca.pem'),
            'control_certificate_file': str(self.root / 'control.pem'),
            'control_private_key_file': str(self.root / 'control.key'),
            'instance_id': '11111111-1111-4111-8111-111111111111',
            'installation_id': '22222222-2222-4222-8222-222222222222',
            'output_dir': str(self.root)}))
        self.config.chmod(0o600)

    def tearDown(self):
        self.directory.cleanup()

    def run_tool(self, token: str, *extra: str):
        token_path = self.root / 'admin.token'
        token_path.write_text(token + '\n')
        token_path.chmod(0o600)
        return subprocess.run(
            [sys.executable, str(TOOL), '--config', str(self.config),
             '--token-file', str(token_path), '--once', *extra],
            capture_output=True, text=True, timeout=20)

    def test_expired_jwt_stops_with_reauth_hint(self):
        result = self.run_tool(jwt(int(time.time()) - 60))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('token_expired', result.stderr)
        self.assertIn('reauthenticate', result.stderr.lower())

    def test_over_age_jwt_is_rejected(self):
        result = self.run_tool(jwt(int(time.time()) + 10000), '--max-token-age', '300')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('token_max_age_exceeded', result.stderr)

    def test_stale_opaque_token_file_is_rejected(self):
        token_path = self.root / 'admin.token'
        token_path.write_text('opaque-token')
        token_path.chmod(0o600)
        old = time.time() - 100
        os.utime(token_path, (old, old))
        result = subprocess.run(
            [sys.executable, str(TOOL), '--config', str(self.config),
             '--token-file', str(token_path), '--once', '--max-token-age', '10'],
            capture_output=True, text=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('token_age_exceeded', result.stderr)
        self.assertIn('reauthenticate', result.stderr.lower())


if __name__ == '__main__':
    unittest.main(verbosity=2)
