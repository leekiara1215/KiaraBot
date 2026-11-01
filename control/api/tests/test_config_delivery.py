"""HTTP -> real C++ Core/SQLite -> real Python SDK -> ACK -> HTTP status."""
import json
import os
import subprocess
import time
from pathlib import Path

from conftest import ROOT, Server, validate


class SDK:
    def __init__(self, server):
        self.observations = server.directory / 'sdk-config.jsonl'
        env = {key: value for key, value in os.environ.items() if not key.startswith('KIARABOT_')}
        env.update(PYTHONPATH=os.pathsep.join(str(ROOT / p) for p in ('sdk/python/src', 'sdk/python/build/generated')),
                   PLUGIN_CORE_ENDPOINT=server.endpoint, PLUGIN_AUTH_TOKEN=server.plugin_token,
                   CONFIG_OBSERVATIONS=str(self.observations))
        self.process = subprocess.Popen([os.environ.get('KIARABOT_ACCEPTANCE_PYTHON', str(ROOT / 'sdk/python/.venv/bin/python')), Path(__file__).with_name('config_plugin.py')],
                                       env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def stop(self, expected_terminal=None):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            stdout, stderr = self.process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.communicate()
            raise AssertionError('SDK failed to stop')
        if self.process.returncode != 0:
            # Disable can beat SIGTERM: the real SDK correctly rejects the disabled instance.
            # Accept only that explicitly expected terminal code; retain every other failure.
            assert expected_terminal is not None and self.process.returncode == 1, stderr.decode()
            try:
                terminal = json.loads(stderr.decode().splitlines()[-1])
            except (ValueError, IndexError):
                raise AssertionError(stderr.decode()) from None
            assert terminal == {'terminal_error': expected_terminal}, stderr.decode()

    def records(self):
        return [json.loads(line) for line in self.observations.read_text().splitlines()]


def test_config_application_rejection_reconnect_and_restart(tmp_path):
    server = Server(tmp_path, plugin=True)
    sdk = None
    try:
        token = server.login()['access_token']
        def read():
            status, body, _ = server.request('/plugins/test.plugin/config', token=token)
            assert status == 200, body
            validate(body, 'PluginConfig')
            return body
        def wait(state, version=None):
            deadline = time.monotonic() + 6
            while time.monotonic() < deadline:
                body = read()
                if body['delivery']['state'] == state and (version is None or body['version'] == version):
                    return body
                time.sleep(.02)
            raise AssertionError(body)
        def write(values, key=None):
            before = read()
            body = {'expected_version': before['version'], 'values': values}
            status, result, _ = server.request('/plugins/test.plugin/config', 'PUT', body, token, key)
            assert status == 200, result
            assert 'delivery' not in result
            return body, result
        assert read()['delivery'] == {'state': 'UNAVAILABLE'}
        # Offline write is durable and applied on the first negotiated connection.
        original, desired = write({'generation': 1}, 'configuration-key-0001')
        sdk = SDK(server)
        applied = wait('APPLIED', desired['version'])
        assert applied['delivery']['applied_version'] == desired['version']
        assert sdk.records()[-1]['values'] == {'generation': 1}
        count = len(sdk.records())
        # Successful idempotent replay must not cause another application.
        assert server.request('/plugins/test.plugin/config', 'PUT', original, token, 'configuration-key-0001')[1] == desired
        time.sleep(.1)
        assert len(sdk.records()) == count
        _, second = write({'generation': 2})
        wait('APPLIED', second['version'])
        assert sdk.records()[-1]['values'] == {'generation': 2}
        _, rejected = write({'mode': 'reject'})
        failure = wait('REJECTED', rejected['version'])['delivery']
        assert failure['error']['code'] == 'INVALID_ARGUMENT'
        assert failure['applied_version'] == second['version']
        _, timed_out = write({'mode': 'timeout'})
        failure = wait('REJECTED', timed_out['version'])['delivery']
        assert failure['error']['code'] == 'TIMEOUT'
        assert failure['applied_version'] == second['version']
        _, recovered = write({'generation': 3})
        wait('APPLIED', recovered['version'])
        sdk.stop(); sdk = None
        wait('UNAVAILABLE')
        # Restart both processes: preserve desired version, discard live application evidence.
        server.stop(); server.start()
        assert read()['version'] == recovered['version']
        assert read()['delivery'] == {'state': 'UNAVAILABLE'}
        sdk = SDK(server)
        wait('APPLIED', recovered['version'])
        assert [r['version'] for r in sdk.records()].count(recovered['version']) == 2
        assert server.request('/plugins/test.plugin/disable', 'POST', {}, token)[0] == 202
        wait('UNAVAILABLE')
        assert sdk.process.wait(timeout=5) == 1, 'disabled instance must report its terminal error'
        sdk.stop(expected_terminal='PLUGIN_UNAVAILABLE'); sdk = None
    finally:
        if sdk:
            sdk.stop()
        server.stop()
