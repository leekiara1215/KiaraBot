"""Isolated loopback listener; secrets are test-only and never printed."""
import os
import selectors
import signal
import subprocess
import sys
import sqlite3
import tempfile
from pathlib import Path

binary = sys.argv[1]
environment = {key: value for key, value in os.environ.items()
               if not key.startswith('KIARABOT_ADAPTER_') and not key.startswith('KIARABOT_PLUGIN_')
               and key != 'KIARABOT_RUNTIME_DB'}
args = [binary, '--grpc-listen', '127.0.0.1:0']
if subprocess.run(args, env=environment, capture_output=True, timeout=5).returncode != 1:
    raise RuntimeError('Missing credentials did not fail startup')
environment.update(KIARABOT_ADAPTER_ID='test', KIARABOT_ADAPTER_TOKEN='x' * 32)
if subprocess.run([binary, '--grpc-listen', '0.0.0.0:0'], env=environment,
                  capture_output=True, timeout=5).returncode != 1:
    raise RuntimeError('External address accepted')
for name, value in (('KIARABOT_AUDIT_LIMIT', 'abc'), ('KIARABOT_ACTION_LIMIT', '0'),
                    ('KIARABOT_EVENT_QUEUE_LIMIT', '0'), ('KIARABOT_ACK_TIMEOUT_MS', '1'),
                    ('KIARABOT_MAINTENANCE_INTERVAL_MS', '0'),
                    ('KIARABOT_RETENTION_HOURS', '1')):
    invalid = dict(environment, **{name: value})
    if subprocess.run(args, env=invalid, capture_output=True, timeout=5).returncode != 1:
        raise RuntimeError('Invalid ' + name + ' did not fail startup')
tuned = dict(environment, KIARABOT_EVENT_QUEUE_LIMIT='128', KIARABOT_AUDIT_LIMIT='5000',
             KIARABOT_ACTION_LIMIT='500', KIARABOT_ACK_TIMEOUT_MS='5000',
             KIARABOT_MAINTENANCE_INTERVAL_MS='1000', KIARABOT_RETENTION_HOURS='48')
with subprocess.Popen(args, env=tuned, stdout=subprocess.PIPE,
                      stderr=subprocess.PIPE) as process:
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            if not selector.select(timeout=5):
                raise RuntimeError('Tuned runtime configuration did not start')
            line = process.stdout.readline()
        if b'transport_listening=true' not in line:
            raise RuntimeError('Tuned runtime configuration rejected')
        process.send_signal(signal.SIGTERM)
        process.communicate(timeout=5)
        if process.returncode != 0:
            raise RuntimeError('Tuned runtime configuration failed to stop')
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
for stop in (signal.SIGINT, signal.SIGTERM):
    with subprocess.Popen(args, env=environment, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE) as process:
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                if not selector.select(timeout=5):
                    raise RuntimeError('Listener did not start')
                line = process.stdout.readline()
            if b'transport_listening=true' not in line or b'rpc_ready=false' not in line:
                raise RuntimeError('Incorrect startup status')
            process.send_signal(stop)
            output, error = process.communicate(timeout=5)
            if process.returncode != 0 or b'process_stop' not in output:
                raise RuntimeError('Shutdown failed')
            if b'x' * 32 in output + error:
                raise RuntimeError('Credential leaked')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
print('gRPC process startup, invalid config and signals passed')

if '--sqlite' in sys.argv[2:]:
    with tempfile.TemporaryDirectory(prefix='kiarabot-startup-') as directory:
        database = Path(directory) / 'runtime.db'
        durable = dict(environment, KIARABOT_RUNTIME_DB=str(database),
                       KIARABOT_ADAPTER_ACCOUNTS='account', KIARABOT_PLUGIN_ID='plugin',
                       KIARABOT_PLUGIN_TOKEN='p' * 32, KIARABOT_PLUGIN_PERMISSIONS='message.read')
        for invalid in ('', ':memory:', 'file::memory:?cache=shared', str(Path(directory) / 'missing' / 'db')):
            result = subprocess.run(args, env=dict(durable, KIARABOT_RUNTIME_DB=invalid),
                                    capture_output=True, timeout=5)
            assert result.returncode == 1
        for attempt in range(2):
            with subprocess.Popen(args, env=durable, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE) as process:
                try:
                    with selectors.DefaultSelector() as selector:
                        selector.register(process.stdout, selectors.EVENT_READ)
                        assert selector.select(timeout=5)
                        line = process.stdout.readline()
                        assert b'adapter_ingress_ready=true' in line
                        assert b'plugin_delivery_ready=true' in line
                    process.send_signal(signal.SIGTERM)
                    process.communicate(timeout=5)
                    assert process.returncode == 0
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=5)
        with sqlite3.connect(database) as connection:
            assert connection.execute('PRAGMA user_version').fetchone()[0] == 5
            connection.execute('PRAGMA user_version=999')
        assert subprocess.run(args, env=durable, capture_output=True, timeout=5).returncode == 1
    print('Durable ingress startup, reopen and corrupt-schema refusal passed')
