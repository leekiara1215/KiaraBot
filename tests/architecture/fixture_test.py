"""Real disposable certificate validation; this is not a six-service acceptance run."""
import argparse
import os
import ssl
import stat
import unittest
from pathlib import Path

from fixtures import TestPki

OPENSSL = None


class PkiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pki = TestPki(OPENSSL).__enter__()

    @classmethod
    def tearDownClass(cls):
        directory = cls.pki.root
        cls.pki.__exit__(None, None, None)
        if directory.exists():
            raise AssertionError('temporary private key directory was not removed')

    def test_trusted_server_has_loopback_san_and_server_purpose(self):
        for certificate in self.pki.servers.values():
            result = self.pki.run('verify', '-CAfile', self.pki.ca.certificate,
                                  '-purpose', 'sslserver', '-verify_ip', '127.0.0.1', certificate.certificate)
            self.assertIn(b': OK', result.stdout)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(certificate.certificate, certificate.key)

    def test_trusted_client_purpose(self):
        result = self.pki.run('verify', '-CAfile', self.pki.ca.certificate,
                              '-purpose', 'sslclient', self.pki.client.certificate)
        self.assertIn(b': OK', result.stdout)

    def test_expired_certificates_fail_for_expiry(self):
        for cert in (self.pki.expired_client, self.pki.expired_server):
            result = self.pki.run('verify', '-CAfile', self.pki.ca.certificate,
                                  cert.certificate, expected=2)
            self.assertIn(b'certificate has expired', result.stderr)

    def test_foreign_client_fails_for_untrusted_issuer(self):
        result = self.pki.run('verify', '-CAfile', self.pki.ca.certificate,
                              self.pki.foreign_client.certificate, expected=2)
        self.assertIn(b'unable to get local issuer certificate', result.stderr)

    def test_private_material_has_owner_only_permissions(self):
        self.assertEqual(stat.S_IMODE(self.pki.root.stat().st_mode), 0o700)
        for key in self.pki.root.glob('*.key'):
            self.assertEqual(stat.S_IMODE(key.stat().st_mode), 0o600)

    def test_wrong_private_key_rejected(self):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        with self.assertRaises(ssl.SSLError) as failure:
            context.load_cert_chain(self.pki.servers['runtime'].certificate, self.pki.client.key)
        self.assertIn('KEY_VALUES_MISMATCH', str(failure.exception))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--openssl', required=True, type=Path)
    options, remaining = parser.parse_known_args()
    OPENSSL = options.openssl
    unittest.main(argv=[__file__, *remaining])
