#!/usr/bin/env python3
"""Exercise health probes against a local HTTP server."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'healthcheck.sh'
TOKEN = 'test-token-123'


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ('/health/live', '/health/ready'):
            status = 200
        elif self.path == '/api/v1/health':
            status = 200 if self.headers.get('Authorization') == f'Bearer {TOKEN}' else 401
        else:
            status = 404
        self.send_response(status)
        self.end_headers()
        self.wfile.write(b'{}')

    def log_message(self, *_args):
        pass


class HealthcheckTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.url = f'http://127.0.0.1:{cls.server.server_port}'

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.thread.join()
        cls.server.server_close()

    def probe(self, endpoint, *args):
        return subprocess.run([str(SCRIPT), '--url', self.url, '--endpoint', endpoint, *args],
                              text=True, capture_output=True, check=False)

    def test_public_probes_and_obsolete_path(self):
        for endpoint in ('/health/live', '/health/ready'):
            self.assertEqual(self.probe(endpoint).returncode, 0)
        self.assertEqual(self.probe('/health/details').returncode, 2)

    def test_authenticated_health_and_rejections(self):
        with tempfile.TemporaryDirectory() as directory:
            token_file = Path(directory) / 'token'
            token_file.write_text(TOKEN + '\n')
            result = self.probe('/api/v1/health', '--token-file', str(token_file))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn(TOKEN, result.stdout + result.stderr)
            token_file.write_text('wrong-token')
            self.assertEqual(self.probe('/api/v1/health', '--token-file', str(token_file)).returncode, 1)
            token_file.write_text('bad\nheader')
            self.assertEqual(self.probe('/api/v1/health', '--token-file', str(token_file)).returncode, 2)
            self.assertEqual(self.probe('/health/live', '--token-file', str(token_file)).returncode, 2)
        self.assertEqual(self.probe('/api/v1/health').returncode, 2)


if __name__ == '__main__':
    unittest.main()
