#!/usr/bin/env python3
"""P05b-6 unit tests for sealed delegation file contract (no network/PG)."""
from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import time
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'scripts/deploy'))
import sealed_delegation as sealed  # noqa: E402


def token(issued_at: int, expires_at: int, instance_principal='11111111-1111-4111-8111-111111111111'):
    payload = {
        'aud': 'runtime', 'iat': issued_at, 'nbf': issued_at, 'exp': expires_at,
        'sub': instance_principal, 'iss': 'test',
        'delegation': {'direct_actor': '22222222-2222-4222-8222-222222222222',
                       'actor_chain': ['22222222-2222-4222-8222-222222222222'],
                       'business_hops': '1'},
    }
    def segment(value):
        return base64.urlsafe_b64encode(
            json.dumps(value, separators=(',', ':')).encode()).rstrip(b'=').decode()
    return segment({'alg': 'EdDSA', 'typ': 'JWT'}) + '.' + segment(payload) + '.signature'


def document(instance_id='aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa',
             installation_id='bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb', now=None,
             ttl=56):
    now = int(time.time()) if now is None else int(now)
    return {
        'schema_version': 1,
        'instance_id': instance_id,
        'installation_id': installation_id,
        'audience': 'runtime',
        'permissions': ['task.schedule', 'task.read'],
        'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': instance_id},
                  {'kind': 'RESOURCE_INSTALLATION', 'id': installation_id}],
        'issued_at': now,
        'expires_at': now + ttl,
        'ttl_seconds': ttl,
        'delegation_proof': token(now, now + ttl),
    }


class SealedDelegationTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='kiarabot-p05b6-')
        self.root = Path(self.directory.name)
        self.root.chmod(0o700)
        self.path = self.root / 'instance-delegation.json'

    def tearDown(self):
        self.directory.cleanup()

    def test_atomic_write_read_and_replacement(self):
        sealed.atomic_write_sealed(str(self.path), document())
        info = os.lstat(self.path)
        self.assertTrue(stat.S_ISREG(info.st_mode))
        self.assertEqual(stat.S_IMODE(info.st_mode), 0o600)
        first = sealed.read_sealed_delegation(str(self.path))
        self.assertEqual(first['instance_id'], 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa')
        replacement = document(now=int(time.time()) + 1)
        sealed.atomic_write_sealed(str(self.path), replacement)
        self.assertEqual(sealed.read_sealed_delegation(str(self.path))['issued_at'],
                         replacement['issued_at'])
        self.assertEqual([entry.name for entry in self.root.iterdir()],
                         ['instance-delegation.json'])

    def test_symlink_target_is_rejected(self):
        victim = self.root / 'victim'
        victim.write_text('{}')
        os.chmod(victim, 0o600)
        self.path.symlink_to(victim)
        with self.assertRaises(sealed.SealedDelegationError):
            sealed.atomic_write_sealed(str(self.path), document())
        with self.assertRaises(sealed.SealedDelegationError):
            sealed.read_sealed_delegation(str(self.path))
        self.assertEqual(victim.read_text(), '{}')
        self.assertTrue(self.path.is_symlink())

    def test_wrong_mode_owner_and_instance_are_rejected(self):
        sealed.atomic_write_sealed(str(self.path), document())
        os.chmod(self.path, 0o644)
        with self.assertRaises(sealed.SealedDelegationError):
            sealed.read_sealed_delegation(str(self.path))
        os.chmod(self.path, 0o600)
        with self.assertRaises(sealed.SealedDelegationError):
            sealed.read_sealed_delegation(str(self.path),
                                          expected_owner=os.geteuid() + 1)
        with self.assertRaises(sealed.SealedDelegationError):
            sealed.read_sealed_delegation(str(self.path), expected_instance='wrong-instance')

    def test_ttl_and_expiry_are_rejected(self):
        with self.assertRaises(sealed.SealedDelegationError):
            sealed.validate_document(document(ttl=57))
        expired = document(now=int(time.time()) - 120)
        with self.assertRaises(sealed.SealedDelegationError):
            sealed.validate_document(expired)
        tampered = document()
        tampered['expires_at'] += 1
        with self.assertRaises(sealed.SealedDelegationError):
            sealed.validate_document(tampered)

    def test_scope_and_permissions_are_whitelisted(self):
        wrong = document()
        wrong['permissions'] = ['account.manage']
        with self.assertRaises(sealed.SealedDelegationError):
            sealed.validate_document(wrong)
        cross = document()
        cross['scope'][0] = {'kind': 'RESOURCE_INSTANCE', 'id': 'other-instance'}
        with self.assertRaises(sealed.SealedDelegationError):
            sealed.validate_document(cross)

    def test_failed_replace_keeps_old_file_and_cleans_temporary(self):
        sealed.atomic_write_sealed(str(self.path), document())
        old = self.path.read_text()
        with mock.patch('os.replace', side_effect=OSError('simulated')):
            with self.assertRaises(OSError):
                sealed.atomic_write_sealed(str(self.path), document(now=int(time.time()) + 1))
        self.assertEqual(self.path.read_text(), old)
        self.assertEqual([entry.name for entry in self.root.iterdir()],
                         ['instance-delegation.json'])

    def test_sealed_document_does_not_contain_plaintext_secret_words(self):
        sealed.atomic_write_sealed(str(self.path), document())
        content = self.path.read_text()
        self.assertNotIn('password', content.lower())
        self.assertNotIn('access_token', content.lower())


if __name__ == '__main__':
    unittest.main(verbosity=2)
