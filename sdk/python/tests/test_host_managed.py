"""P08c real Host (P08a) + fake Runtime bridges + real managed SDK plugin process.

The Host binary spawns an actual ``python -m kiarabot_sdk`` child with the deployment
environment allowlist and the bootstrap credential file injection; the child authenticates to
the PluginBridge fake with ``KIARABOT_PLUGIN_BOOTSTRAP``, applies a delivered config and ACKs
it. A fake HostBridge drives the Host's apply/stop commands over mTLS and observes host-local
evidence (command journal, managed execution, observations).

Set ``KIARABOT_TEST_HOST_BINARY`` to the built ``kiarabot-plugin-host`` (or build the repo's
``build/full-locked`` tree) to run; otherwise the test skips because no C++ artifact is part of
the Python checkout. P08d owns the CTest fixture registration.
"""
from __future__ import annotations

import asyncio
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import sys
import time
import uuid

import grpc
import pytest
from google.protobuf.timestamp_pb2 import Timestamp
from kiarabot.v1 import common_pb2 as common, events_pb2 as events, host_pb2 as host
from kiarabot.v1 import host_pb2_grpc as host_grpc, platform_pb2 as platform
from kiarabot_sdk.testing import FakeTargetRuntime

ROOT = Path(__file__).resolve().parents[3]
HOST_BINARY = os.environ.get('KIARABOT_TEST_HOST_BINARY') or str(
    ROOT / 'build/full-locked/bin/kiarabot-plugin-host')
pytestmark = pytest.mark.skipif(
    not Path(HOST_BINARY).is_file(),
    reason='kiarabot-plugin-host binary is not built; set KIARABOT_TEST_HOST_BINARY')


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def timestamp(seconds):
    value = Timestamp()
    value.FromSeconds(int(seconds))
    return value


def run_openssl(*arguments, cwd):
    subprocess.run(['openssl', *arguments], check=True, cwd=cwd, capture_output=True)


def generate_pki(directory):
    directory.mkdir(parents=True, exist_ok=True)
    run_openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'ca.key',
                '-out', 'ca.pem', '-days', '2', '-subj', '/CN=p08c-test-ca', cwd=directory)
    (directory / 'server.ext').write_text(
        'subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n')
    run_openssl('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'server.key',
                '-out', 'server.csr', '-subj', '/CN=localhost', cwd=directory)
    run_openssl('x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key',
                '-CAcreateserial', '-out', 'server.pem', '-days', '2', '-extfile',
                'server.ext', cwd=directory)
    # The Host's own service certificate must satisfy SSL_SERVER purpose and is also its
    # HostBridge client credential, so it carries both EKUs.
    (directory / 'client.ext').write_text('extendedKeyUsage=clientAuth,serverAuth\n')
    run_openssl('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'host.key',
                '-out', 'host.csr', '-subj', '/CN=host-a', cwd=directory)
    run_openssl('x509', '-req', '-in', 'host.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key',
                '-CAcreateserial', '-out', 'host.pem', '-days', '2', '-extfile',
                'client.ext', cwd=directory)


class FakeHostBridge(host_grpc.HostBridgeServicer):
    """mTLS HostBridge fake: registration/fence, commands, receipts and observations."""

    def __init__(self, certificate, private_key, ca, host_id):
        self.host_id = host_id
        self.fence = None
        self.commands = asyncio.Queue()
        self.receipts = asyncio.Queue()
        self.observations = asyncio.Queue()
        self.registrations = asyncio.Queue()
        server = grpc.aio.server()
        host_grpc.add_HostBridgeServicer_to_server(self, server)
        credentials = grpc.ssl_server_credentials(
            [(private_key.read_bytes(), certificate.read_bytes())],
            root_certificates=ca.read_bytes(), require_client_auth=True)
        self.port = server.add_secure_port('127.0.0.1:0', credentials)
        self.endpoint = f'127.0.0.1:{self.port}'
        self._server = server
        self._outgoing = asyncio.Queue()

    async def start(self):
        await self._server.start()

    async def stop(self):
        await self._outgoing.put(None)
        await self._server.stop(0)

    async def send(self, frame):
        await self._outgoing.put(frame)

    async def Connect(self, requests, context):
        first = await requests.__anext__()
        assert first.WhichOneof('body') == 'registration'
        self.registrations.put_nowait(first.registration)
        self.fence = platform.Fence(instance_id=self.host_id, connection_generation=1,
                                    runtime_epoch='epoch-p08c-fixture')
        yield host.RuntimeToHost(registered=host.HostRegisterHostResponse(fence=self.fence))

        async def read():
            async for frame in requests:
                body = frame.WhichOneof('body')
                if body == 'receipt':
                    self.receipts.put_nowait(frame.receipt)
                elif body == 'observation':
                    self.observations.put_nowait(frame.observation)
                elif body == 'heartbeat':
                    continue

        reader = asyncio.create_task(read())
        try:
            while True:
                frame = await self._outgoing.get()
                if frame is None:
                    return
                frame.fence.CopyFrom(self.fence)
                yield frame
        finally:
            reader.cancel()
            try:
                await reader
            except asyncio.CancelledError:
                pass


def request_context():
    context = platform.RequestContext(
        request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()))
    context.deadline.CopyFrom(timestamp(time.time() + 600))
    return context


def handoff():
    return platform.HandoffContext(
        source_service='runtime', message_id=str(uuid.uuid4()),
        payload_sha256=hashlib.sha256(b'p08c-payload').digest(),
        created_at=timestamp(time.time()), valid_until=timestamp(time.time() + 600))


def apply_request(*, host_id, instance_id, installation_id, generation, desired_version,
                  package_sha256, manifest, command_id):
    return host.RuntimeToHost(apply=host.HostApplyDesiredProcessRequest(
        context=request_context(), handoff=handoff(), command_id=command_id,
        process=host.ProcessSpec(
            instance_id=instance_id, installation_id=installation_id,
            execution_generation=generation, desired_version=desired_version,
            desired_state=platform.DESIRED_RUNNING, package_sha256=package_sha256,
            launch_profile='python', arguments=[f'--manifest={manifest}'],
            environment_names=['PYTHONPATH', 'PLUGIN_CORE_ENDPOINT', 'PLUGIN_TLS_ROOT',
                               'PLUGIN_INSTANCE_ID', 'PLUGIN_IO_TIMEOUT',
                               'PLUGIN_HEARTBEAT_INTERVAL', 'PLUGIN_RETRY_DELAY',
                               'KIARABOT_PLUGIN_BOOTSTRAP'])))


def stop_request(*, host_id, instance_id, generation, command_id):
    return host.RuntimeToHost(stop=host.HostStopExecutionRequest(
        context=request_context(), handoff=handoff(), command_id=command_id,
        host_id=host_id, instance_id=instance_id, execution_generation=generation,
        grace_ms=2000))


def write_plugin_package(directory):
    package = directory / 'package'
    package.mkdir(parents=True, exist_ok=True)
    applied = directory / 'applied.jsonl'
    (package / 'subject.py').write_text(
        'import json\n'
        'from pathlib import Path\n'
        'from kiarabot_sdk import Plugin\n'
        'class Subject(Plugin):\n'
        '    async def on_config(self, config, context):\n'
        f'        Path({str(applied)!r}).open("a").write(json.dumps(dict(\n'
        '            version=config.version, config_version=config.config_version,\n'
        '            delivery_id=config.delivery_id, values=dict(config.values))) + "\\n")\n'
        'def create():\n'
        '    return Subject()\n')
    manifest = json.loads((ROOT / 'plugins/event-observer/plugin.json').read_text())
    manifest['entrypoint'] = 'subject:create'
    manifest_path = directory / 'plugin.json'
    manifest_path.write_text(json.dumps(manifest))
    return package, manifest_path, applied


def write_host_config(directory, *, host_id, bridge_endpoint, plugin_env, package, runtime_ca, token):
    credentials = directory / 'credentials'
    credentials.mkdir(exist_ok=True)
    instance_id = str(uuid.uuid4())
    installation_id = str(uuid.uuid4())
    (credentials / f'{instance_id}.bootstrap').write_text(token)
    (credentials / f'{instance_id}.bootstrap').chmod(0o600)
    shutil.copy(runtime_ca, directory / 'runtime-ca.pem')
    environment_names = ['PYTHONPATH', 'PLUGIN_CORE_ENDPOINT', 'PLUGIN_TLS_ROOT',
                         'PLUGIN_INSTANCE_ID', 'PLUGIN_IO_TIMEOUT',
                         'PLUGIN_HEARTBEAT_INTERVAL', 'PLUGIN_RETRY_DELAY',
                         'KIARABOT_PLUGIN_BOOTSTRAP']
    environment = {
        'PYTHONPATH': os.pathsep.join([
            str(ROOT / 'sdk/python/src'), str(ROOT / 'sdk/python/build/generated'),
            str(package)]),
        'PLUGIN_TLS_ROOT': str(directory / 'runtime-ca.pem'),
        'PLUGIN_INSTANCE_ID': instance_id,
        'PLUGIN_IO_TIMEOUT': '2', 'PLUGIN_HEARTBEAT_INTERVAL': '0.2',
        'PLUGIN_RETRY_DELAY': '0.1',
        **plugin_env,
    }
    config = {
        'schema_version': 1, 'service': 'plugin-host',
        'probe_listen': f'127.0.0.1:{free_port()}',
        'grpc_listen': f'127.0.0.1:{free_port()}',
        'tls': {'certificate_file': str(directory / 'tls/host.pem'),
                'private_key_file': str(directory / 'tls/host.key'),
                'ca_file': str(directory / 'tls/ca.pem')},
        'host': {
            'host_id': host_id, 'sqlite_path': str(directory / 'host.sqlite'),
            'runtime_endpoint': bridge_endpoint,
            'credential_directory': str(credentials),
            'packages': [{'plugin_type_id': 'event-observer',
                          'root': str(package), 'sha256': 'a' * 64,
                          'launch_profile': 'python',
                          'allowed_argument_names': ['--manifest']}],
            'profiles': [{'name': 'python', 'executable': sys.executable,
                          'fixed_arguments': [sys.executable, '-m', 'kiarabot_sdk'],
                          'allowed_environment_names': environment_names,
                          'credential_environment': 'KIARABOT_PLUGIN_BOOTSTRAP'}],
            'environment': environment,
        },
    }
    path = directory / 'host.json'
    path.write_text(json.dumps(config))
    return path, instance_id, installation_id, token


async def wait_for(predicate, timeout=10, interval=.01):
    async with asyncio.timeout(timeout):
        while not predicate():
            await asyncio.sleep(interval)


async def test_real_host_spawns_managed_sdk_and_applies_config(tmp_path):
    generate_pki(tmp_path / 'tls')
    host_id = str(uuid.uuid4())
    package_sha = bytes.fromhex('a' * 64)
    bridge = FakeHostBridge(tmp_path / 'tls/server.pem', tmp_path / 'tls/server.key',
                            tmp_path / 'tls/ca.pem', host_id)
    await bridge.start()
    applied_versions = []
    try:
        async with FakeTargetRuntime(features=['plugin.config.v1'],
                                     desired_config_version=1) as runtime:
            package, manifest, applied = write_plugin_package(tmp_path)
            config_path, instance_id, installation_id, token = write_host_config(
                tmp_path, host_id=host_id, bridge_endpoint=bridge.endpoint,
                plugin_env={'PLUGIN_CORE_ENDPOINT': runtime.endpoint},
                package=package, runtime_ca=tmp_path / 'tls' / 'ca.pem',
                token=runtime.token)
            # The plugin trust root is the fake PluginBridge server certificate, written above
            # only as PLUGIN_TLS_ROOT; keep it separate from the HostBridge PKI.
            (tmp_path / 'runtime-ca.pem').write_bytes(runtime.root_cert)
            config = json.loads(config_path.read_text())
            config['host']['environment']['PLUGIN_TLS_ROOT'] = str(tmp_path / 'runtime-ca.pem')
            config['host']['environment']['PLUGIN_CORE_ENDPOINT'] = runtime.endpoint
            config_path.write_text(json.dumps(config))

            host_process = await asyncio.create_subprocess_exec(
                HOST_BINARY, '--config', str(config_path), cwd=str(tmp_path),
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            try:
                await asyncio.wait_for(bridge.registrations.get(), 10)
                await bridge.send(apply_request(
                    host_id=host_id, instance_id=instance_id,
                    installation_id=installation_id, generation=1, desired_version=1,
                    package_sha256=package_sha, manifest=manifest, command_id='command-1'))
                receipt = await asyncio.wait_for(bridge.receipts.get(), 10)
                assert receipt.command_id == 'command-1' and receipt.persisted
                assert receipt.execution.instance_id == instance_id
                observed = await asyncio.wait_for(bridge.observations.get(), 10)
                assert observed.execution.instance_id == instance_id
                # A running managed process has no terminal outcome yet; the Host reports the
                # process fact (marker) without inventing success/failure.
                assert observed.execution.process_marker
                assert observed.execution.desired_version == 1

                registration = await asyncio.wait_for(runtime.registrations.get(), 10)
                assert registration.instance_id == instance_id
                assert registration.supported_features
                await runtime.send_config('v1', {'greeting': 'hello'}, config_version=1,
                                          delivery_id='host-delivery-1')
                ack = await asyncio.wait_for(runtime.config_acknowledgements.get(), 10)
                assert ack.accepted and ack.config_version == 1
                assert ack.HasField('fence') and ack.fence.instance_id == instance_id
                await wait_for(applied.exists)
                applied_versions = [json.loads(line) for line in applied.read_text().splitlines()]
                assert applied_versions == [{'version': 'v1', 'config_version': 1,
                                             'delivery_id': 'host-delivery-1',
                                             'values': {'greeting': 'hello'}}]

                # Host-local evidence: command journal/execution/observations are durable.
                database = sqlite3.connect(tmp_path / 'host.sqlite')
                try:
                    execution = database.execute(
                        'SELECT desired_version, state, outcome FROM managed_executions '
                        'WHERE instance_id=?', (instance_id,)).fetchone()
                    assert execution is not None
                    assert execution[0] == 1
                    assert database.execute('SELECT count(*) FROM command_journal').fetchone()[0] >= 1
                    assert database.execute(
                        'SELECT count(*) FROM execution_observations').fetchone()[0] >= 1
                finally:
                    database.close()

                # Stop through the HostBridge, then re-apply a new execution generation: the
                # managed plugin process restarts and applies the redelivered durable snapshot.
                await bridge.send(stop_request(host_id=host_id, instance_id=instance_id,
                                               generation=1, command_id='command-stop'))
                stopped = await asyncio.wait_for(bridge.receipts.get(), 10)
                assert stopped.command_id == 'command-stop' and stopped.persisted
                runtime.desired_config_version = 2
                await bridge.send(apply_request(
                    host_id=host_id, instance_id=instance_id,
                    installation_id=installation_id, generation=2, desired_version=2,
                    package_sha256=package_sha, manifest=manifest, command_id='command-2'))
                assert (await asyncio.wait_for(bridge.receipts.get(), 10)).persisted
                await asyncio.wait_for(runtime.registrations.get(), 10)
                await runtime.send_config('v2', {'greeting': 'again'}, config_version=2,
                                          delivery_id='host-delivery-2')
                replay_ack = await asyncio.wait_for(runtime.config_acknowledgements.get(), 10)
                assert replay_ack.accepted and replay_ack.config_version == 2
                await wait_for(lambda: applied.exists()
                               and len(applied.read_text().splitlines()) == 2)
                assert json.loads(applied.read_text().splitlines()[1])['delivery_id'] == \
                    'host-delivery-2'
            finally:
                host_process.terminate()
                try:
                    await asyncio.wait_for(host_process.communicate(), 5)
                except asyncio.TimeoutError:
                    host_process.kill()
                    await host_process.communicate()
    finally:
        await bridge.stop()
