"""Test the acceptance oracle itself, never substitute this for real service runs."""
import http.server
import json
import signal
import subprocess
import sys
import threading
import unittest
from pathlib import Path

from process_support import (AcceptanceFailure, assert_closed, assert_exit,
                             assert_probe, assert_sanitized, child_environment)


class OracleTest(unittest.TestCase):
    def test_negative_requires_exact_exit_and_expected_diagnostic(self):
        assert_exit(subprocess.CompletedProcess([], 2, b'', b'configuration invalid'), 2, 'config')
        for actual in (0, 1, -signal.SIGSEGV):
            with self.assertRaises(AcceptanceFailure):
                assert_exit(subprocess.CompletedProcess([], actual, b'', b'config'), 2, 'config')
        with self.assertRaises(AcceptanceFailure):
            assert_exit(subprocess.CompletedProcess([], 2, b'', b'unrelated failure'), 2, 'config')

    def test_diagnostics_reject_secret_material(self):
        for secret in (b'-----BEGIN PRIVATE KEY-----', b'-----BEGIN CERTIFICATE-----',
                       b'postgresql://user:password@host/database'):
            with self.assertRaises(AcceptanceFailure):
                assert_sanitized(secret)

    def test_missing_binary_is_failure_with_no_skip(self):
        script = Path(__file__).with_name('run.py')
        result = subprocess.run([sys.executable, str(script)], cwd=script.parent,
                                env=child_environment(), capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b'explicit executable --openssl', result.stderr)
        self.assertNotIn(b'passed', result.stdout)
        self.assertNotIn(b'skip', result.stdout.lower())

    def test_fault_injection_checks_exact_sigkill_and_independent_child(self):
        from isolation_acceptance import kill_child
        children = [subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'],
                                     start_new_session=True) for _ in range(2)]
        try:
            kill_child(children[0])
            self.assertEqual(children[0].returncode, -signal.SIGKILL)
            self.assertIsNone(children[1].poll())
            with self.assertRaises(AcceptanceFailure):
                kill_child(children[0])
        finally:
            for child in children:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=5)

    def test_http_oracle_checks_status_and_complete_probe_shape(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                status, payload = {
                    '/live': (200, {'state': 'HEALTHY'}),
                    '/ready': (503, {'state': 'UNHEALTHY'}),
                    '/fake-ready': (200, {'state': 'UNHEALTHY'}),
                    '/bad-shape': (503, {'state': 'UNHEALTHY', 'fake': True}),
                }[self.path]
                self.send_response(status)
                self.end_headers()
                self.wfile.write(json.dumps(payload).encode())

            def log_message(self, *_):
                pass

        with http.server.HTTPServer(('127.0.0.1', 0), Handler) as server:
            endpoint = '127.0.0.1:' + str(server.server_port)
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                assert_probe(endpoint, '/live', 200, 'HEALTHY')
                assert_probe(endpoint, '/ready', 503, 'UNHEALTHY')
                for path in ('/fake-ready', '/bad-shape'):
                    with self.assertRaises(AcceptanceFailure):
                        assert_probe(endpoint, path, 503, 'UNHEALTHY')
                with self.assertRaises(AcceptanceFailure):
                    assert_closed(endpoint)
            finally:
                server.shutdown()
                thread.join(timeout=2)
                self.assertFalse(thread.is_alive())
        assert_closed(endpoint)


if __name__ == '__main__':
    unittest.main()
