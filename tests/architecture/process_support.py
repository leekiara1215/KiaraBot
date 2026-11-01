"""Bounded black-box process control. Child output is retained privately, not dumped."""
import http.client
import json
import os
import re
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from fixtures import free_endpoint


class AcceptanceFailure(RuntimeError):
    pass


def assert_equal(actual, expected, label):
    if actual != expected:
        raise AcceptanceFailure(f'{label}: expected {expected!r}, got {actual!r}')


def child_environment():
    # P02 config is explicit; no inherited legacy credentials, proxies or production endpoints.
    return {key: value for key, value in os.environ.items()
            if not key.startswith(('KIARABOT_', 'CORE_', 'ONEBOT_'))
            and key.upper() not in {'HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'NO_PROXY'}}


def assert_sanitized(output):
    if re.search(rb'-----BEGIN (?:[A-Z ]*PRIVATE KEY|CERTIFICATE)-----|postgres(?:ql)?://', output):
        raise AcceptanceFailure('diagnostic leaked PEM or database connection material')


def assert_exit(result, code, category):
    assert_equal(result.returncode, code, 'child exit')
    output = result.stdout + result.stderr
    assert_sanitized(output)
    if code and not re.search(category, output.decode(errors='replace'), re.IGNORECASE):
        raise AcceptanceFailure('failure diagnostic did not identify the expected category')


def run_once(binary, arguments, directory):
    try:
        return subprocess.run([str(binary), *map(str, arguments)], cwd=directory,
                              env=child_environment(), capture_output=True, timeout=5)
    except subprocess.TimeoutExpired as error:
        raise AcceptanceFailure('short-lived CLI failed to terminate within 5s') from error


def service_config(service, pki):
    leaf = pki.servers[service]
    config = {'schema_version': 1, 'service': service, 'probe_listen': free_endpoint(),
              'tls': {'certificate_file': str(leaf.certificate), 'private_key_file': str(leaf.key),
                      'ca_file': str(pki.ca.certificate)}}
    if service != 'control':
        config['grpc_listen'] = free_endpoint()
        while config['grpc_listen'] == config['probe_listen']:
            config['grpc_listen'] = free_endpoint()
    return config


def write_config(directory, service, config):
    path = Path(directory) / (service + '.json')
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    return path


def http_probe(endpoint, path, method='GET'):
    host, port = endpoint.rsplit(':', 1)
    connection = http.client.HTTPConnection(host, int(port), timeout=1)
    try:
        connection.request(method, path)
        response = connection.getresponse()
        body = response.read(65537)
        if len(body) > 65536:
            raise AcceptanceFailure('unbounded probe response')
        return response.status, body
    finally:
        connection.close()


def assert_probe(endpoint, path, expected_status, state=None):
    status, body = http_probe(endpoint, path)
    assert_equal(status, expected_status, path + ' HTTP status')
    if state is not None:
        try:
            payload = json.loads(body)
        except (UnicodeError, json.JSONDecodeError) as error:
            raise AcceptanceFailure('probe response is not JSON') from error
        assert_equal(payload, {'state': state}, path + ' ProcessProbe')


def assert_closed(endpoint):
    host, port = endpoint.rsplit(':', 1)
    with socket.socket() as connection:
        connection.settimeout(0.3)
        if connection.connect_ex((host, int(port))) == 0:
            raise AcceptanceFailure('listener remained active after child exit')


class ServiceProcess:
    def __init__(self, binary, config, directory, service):
        self.binary, self.config, self.directory, self.service = binary, config, directory, service
        self.process = None

    def __enter__(self):
        path = write_config(self.directory, self.service, self.config)
        self._log = tempfile.TemporaryFile()
        try:
            self.process = subprocess.Popen([str(self.binary), '--config', str(path)], cwd=self.directory,
                                            env=child_environment(), stdout=self._log, stderr=self._log,
                                            start_new_session=True)
        except BaseException:
            self._log.close()
            raise
        return self

    def output(self):
        self._log.flush()
        self._log.seek(0)
        return self._log.read(1024 * 1024)

    def wait_live(self):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                assert_sanitized(self.output())
                raise AcceptanceFailure(f'{self.service} exited before live with code {self.process.returncode}')
            try:
                status, body = http_probe(self.config['probe_listen'], '/health/live')
            except (OSError, http.client.HTTPException):
                time.sleep(0.03)
                continue
            assert_equal(status, 200, self.service + ' initial live status')
            assert_equal(json.loads(body), {'state': 'HEALTHY'}, self.service + ' initial live payload')
            return
        raise AcceptanceFailure(self.service + ' did not become live within 10s')

    def stop(self, stop_signal=signal.SIGTERM):
        if self.process.poll() is not None:
            raise AcceptanceFailure('service exited before shutdown request')
        started = time.monotonic()
        self.process.send_signal(stop_signal)
        try:
            code = self.process.wait(timeout=5)
        except subprocess.TimeoutExpired as error:
            raise AcceptanceFailure('service failed bounded signal shutdown') from error
        assert_equal(code, 0, self.service + ' signal exit')
        assert_sanitized(self.output())
        for key in ('probe_listen', 'grpc_listen'):
            if key in self.config:
                assert_closed(self.config[key])
        return time.monotonic() - started

    def __exit__(self, *_):
        try:
            if self.process is not None and self.process.poll() is None:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=5)
        finally:
            self._log.close()
