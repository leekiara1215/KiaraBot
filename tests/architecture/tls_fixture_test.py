"""Real loopback mTLS handshakes for disposable PKI; not production RPC acceptance."""
import argparse
import socket
import ssl
import threading
import unittest
from pathlib import Path

from fixtures import TestPki, reserved_listener

OPENSSL = None


class TlsFixtureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pki = TestPki(OPENSSL).__enter__()

    @classmethod
    def tearDownClass(cls):
        cls.pki.__exit__(None, None, None)

    def handshake(self, client, trusted_ca=None, hostname='127.0.0.1'):
        server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server_context.load_cert_chain(self.pki.servers['runtime'].certificate, self.pki.servers['runtime'].key)
        server_context.load_verify_locations(cafile=self.pki.ca.certificate)
        server_context.verify_mode = ssl.CERT_REQUIRED
        client_context = ssl.create_default_context(cafile=(trusted_ca or self.pki.ca).certificate)
        if client:
            client_context.load_cert_chain(client.certificate, client.key)
        outcome = {}
        with reserved_listener() as listener:
            listener.settimeout(3)
            endpoint = listener.getsockname()

            def serve():
                try:
                    raw, _ = listener.accept()
                    with raw:
                        raw.settimeout(2)
                        with server_context.wrap_socket(raw, server_side=True) as secure:
                            outcome['peer'] = secure.getpeercert()
                            secure.sendall(b'authenticated')
                except (ssl.SSLError, OSError) as error:
                    outcome['server_error'] = error

            thread = threading.Thread(target=serve)
            thread.start()
            try:
                with socket.create_connection(endpoint, timeout=2) as raw:
                    try:
                        with client_context.wrap_socket(raw, server_hostname=hostname) as secure:
                            outcome['received'] = secure.recv(64)
                    except ssl.SSLError as error:
                        outcome['client_error'] = error
            finally:
                thread.join(timeout=4)
                self.assertFalse(thread.is_alive(), 'TLS fixture thread did not exit')
        return outcome

    def test_trusted_mutual_tls_has_authenticated_peer(self):
        result = self.handshake(self.pki.client)
        self.assertEqual(result['received'], b'authenticated')
        self.assertIn('peer', result)
        self.assertNotIn('server_error', result)

    def test_missing_client_certificate_is_explicitly_rejected(self):
        result = self.handshake(None)
        self.assertNotIn('peer', result)
        self.assertEqual(result['server_error'].reason, 'PEER_DID_NOT_RETURN_A_CERTIFICATE')

    def test_foreign_and_expired_clients_fail_verification(self):
        for cert, codes, alert in ((self.pki.foreign_client, (19, 20), 'TLSV1_ALERT_UNKNOWN_CA'),
                                   (self.pki.expired_client, (10,), 'SSLV3_ALERT_CERTIFICATE_EXPIRED')):
            result = self.handshake(cert)
            self.assertNotIn('peer', result)
            self.assertIsInstance(result['server_error'], ssl.SSLError)
            if isinstance(result['server_error'], ssl.SSLCertVerificationError):
                self.assertIn(result['server_error'].verify_code, codes)
            else:
                # macOS system LibreSSL reports an imprecise server reason, but sends
                # the specific TLS alert to the client. An arbitrary EOF is not accepted.
                self.assertEqual(result['client_error'].reason, alert)

    def test_server_trust_and_hostname_verification_remain_enabled(self):
        for ca, host, codes in ((self.pki.foreign_ca, '127.0.0.1', (19, 20)),
                                (self.pki.ca, 'wrong-host.invalid', (62,))):
            result = self.handshake(self.pki.client, ca, host)
            self.assertNotIn('received', result)
            self.assertIsInstance(result['client_error'], ssl.SSLCertVerificationError)
            self.assertIn(result['client_error'].verify_code, codes)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--openssl', type=Path, required=True)
    options, remaining = parser.parse_known_args()
    OPENSSL = options.openssl
    unittest.main(argv=[__file__, *remaining])
