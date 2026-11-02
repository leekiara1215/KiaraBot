#!/usr/bin/env python3
"""P07d full-chain acceptance: real NoneBot adapter process + fake OneBot WS + real Runtime.

The adapter runs as a real child process against a loopback OneBot v11 websocket and the real
Runtime AdapterBridge (real PG16.4 + mTLS). A shipped SDK plugin submits Action envelopes through
the real PluginBridge; Runtime dispatches them to the adapter, which calls the fake OneBot
platform API, reports result/evidence and receives a durable receipt. No real QQ/NapCat involved.
"""
from __future__ import annotations

import argparse
import asyncio
from datetime import datetime, timedelta, timezone
from pathlib import Path
import json
import os
import signal
import socket
import sys
import tempfile
import time
import uuid

from websockets.asyncio.server import serve

ADAPTER_INSTANCE = '33333333-3333-4333-8333-333333333333'
ACCOUNT = '11111111-1111-4111-8111-111111111111'
PLUGIN_INSTANCE = '44444444-4444-4444-8444-444444444444'
INSTALLATION = '55555555-5555-4555-8555-555555555555'
ADAPTER_ID = '88888888-8888-4888-8888-888888888888'
ADAPTER_BOOTSTRAP = 'a' * 64
PLUGIN_BOOTSTRAP = 'b' * 64
SHUTDOWN_INSTANCE = 'a1a1a1a1-a1a1-4a1a-8a1a-a1a1a1a1a1a1'
SHUTDOWN_BOOTSTRAP = 'c' * 64
ONEBOT_TOKEN = 'platform-p07d-token'


class AcceptanceFailure(RuntimeError):
    pass


def require(condition, label):
    if not condition:
        raise AcceptanceFailure(label)


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def load_repo(repo: Path):
    sys.path.insert(0, str(repo / 'tests/architecture'))
    import p06_client_e2e as p06
    return p06


def message_event(message_id: str, text: str) -> dict:
    return {
        'time': int(time.time()), 'self_id': 10001, 'post_type': 'message',
        'message_type': 'group', 'sub_type': 'normal', 'message_id': message_id,
        'group_id': '20001', 'user_id': '10002', 'raw_message': text, 'font': 0,
        'sender': {'user_id': 10002, 'nickname': 'tester'},
        'message': [{'type': 'text', 'data': {'text': text}}],
        'original_message': [{'type': 'text', 'data': {'text': text}}],
    }


class FakeOneBot:
    """Loopback OneBot v11 reverse-WS platform with success/failure response modes."""

    def __init__(self):
        self.calls = []
        self.mode = 'success'
        self.connections = 0
        self._server = None
        self._sockets = []
        self.port = 0

    async def start(self):
        self._server = await serve(self._handle, '127.0.0.1', 0)
        self.port = self._server.sockets[0].getsockname()[1]
        return self

    async def stop(self):
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()

    async def send_event(self, event: dict):
        for socket in list(self._sockets):
            await socket.send(json.dumps(event))

    async def _handle(self, socket):
        if socket.request.headers.get('Authorization') != f'Bearer {ONEBOT_TOKEN}':
            await socket.close(code=1008)
            return
        self.connections += 1
        self._sockets.append(socket)
        await socket.send(json.dumps(dict(time=int(time.time()), self_id=10001,
                                          post_type='meta_event', meta_event_type='lifecycle',
                                          sub_type='connect')))
        try:
            async for encoded in socket:
                call = json.loads(encoded)
                self.calls.append(call)
                if self.mode == 'delay':
                    await asyncio.sleep(0.4)
                if self.mode == 'platform_fail':
                    await socket.send(json.dumps({
                        'status': 'failed', 'retcode': 1200, 'data': None,
                        'echo': call.get('echo')}))
                    continue
                data = {'message_id': 987} if call.get('action') in (
                    'send_group_msg', 'send_private_msg') else None
                await socket.send(json.dumps({
                    'status': 'ok', 'retcode': 0, 'data': data, 'echo': call.get('echo')}))
        except Exception:  # noqa: BLE001 - platform disconnect is a test scenario
            pass
        finally:
            if socket in self._sockets:
                self._sockets.remove(socket)


def write_manifest(path: Path) -> None:
    path.write_text(json.dumps({
        'id': 'p07d.e2e.plugin', 'name': 'P07d E2E Plugin', 'version': '1.0.0',
        'api_version': 'kiarabot.plugin.v1', 'runtime': 'python-rpc',
        'entrypoint': 'p07d_subject:create', 'isolation': 'process', 'ordered': False,
        'capabilities': ['event.message.received'],
        'permissions': ['message.read', 'message.send'],
    }))


async def async_main(args) -> int:
    repo = Path(args.repo_source).resolve()
    p06 = load_repo(repo)
    fixture = p06.load_fixture(repo)
    common, platform, plugins, sdk_runtime = p06.load_clients(repo)[:4]
    from fixtures import TestPki
    root = Path(tempfile.mkdtemp(prefix='kiarabot-p07d-e2e-')).resolve()
    root.chmod(0o700)
    process = None
    adapter_process = None
    onebot = await FakeOneBot().start()
    tasks = []
    try:
        port = fixture.start_postgres(Path(args.pg_bin).resolve(), root,
                                      Path(args.openssl).resolve(), None)
        admin = (root / 'password').read_text().strip()
        env = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
        env['PGPASSWORD'] = admin
        env['LC_ALL'] = 'C'
        admin_dsn = (f'host=localhost port={port} dbname=postgres user=p03_admin '
                     f'sslmode=verify-full sslrootcert={root / "server.pem"}')
        migration = (root / 'password-p03_runtime_migration').read_text().strip()
        runtime_password = (root / 'password-p03_runtime_runtime').read_text().strip()
        psql_admin = [Path(args.pg_bin) / 'psql', admin_dsn, '-tA', '-v', 'ON_ERROR_STOP=1', '-c']
        fixture.run([*psql_admin,
                     f"CREATE ROLE p03_runtime_migration LOGIN NOINHERIT NOSUPERUSER "
                     f"NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD "
                     f"{fixture.quote_sql(migration)}"], env=env)
        fixture.run([*psql_admin,
                     f"CREATE ROLE p03_runtime_runtime LOGIN NOINHERIT NOSUPERUSER NOCREATEDB "
                     f"NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD "
                     f"{fixture.quote_sql(runtime_password)}"], env=env)
        fixture.run([*psql_admin, 'CREATE DATABASE p03_runtime OWNER p03_runtime_migration'],
                    env=env)
        fixture.run([*psql_admin, 'REVOKE ALL ON DATABASE p03_runtime FROM PUBLIC'], env=env)
        fixture.run([*psql_admin,
                     'GRANT CONNECT ON DATABASE p03_runtime TO p03_runtime_runtime'], env=env)
        fixture.run([args.initializer, '--initialize-empty', 'runtime', 'localhost', str(port),
                     'p03_runtime', 'p03_runtime_migration', 'p03_runtime_runtime',
                     str(root / 'server.pem'), str(root / 'password-p03_runtime_migration')],
                    env=env)
        seeded = fixture.run([args.seed, str(root), str(port), ADAPTER_INSTANCE, ACCOUNT,
                              PLUGIN_INSTANCE, ADAPTER_BOOTSTRAP, PLUGIN_BOOTSTRAP, INSTALLATION,
                              SHUTDOWN_INSTANCE, SHUTDOWN_BOOTSTRAP], env=env)
        require(b'SEEDED' in seeded.stdout, 'seed driver failed')
        with TestPki(Path(args.openssl).resolve()) as pki:
            server_leaf = pki.servers['runtime']
            ca_bytes = pki.ca.certificate.read_bytes()
            ca_path = pki.ca.certificate
            probe_port, grpc_port, bridge_port = (free_port(), free_port(), free_port())
            config = {
                'schema_version': 1, 'service': 'runtime',
                'probe_listen': f'127.0.0.1:{probe_port}',
                'grpc_listen': f'127.0.0.1:{grpc_port}',
                'bridge_listen': f'127.0.0.1:{bridge_port}',
                'tls': {'certificate_file': str(server_leaf.certificate),
                        'private_key_file': str(server_leaf.key),
                        'ca_file': str(pki.ca.certificate)},
                'database': {'host': 'localhost', 'port': port, 'name': 'p03_runtime',
                             'migration_role': 'p03_runtime_migration',
                             'runtime_role': 'p03_runtime_runtime',
                             'password_file': str(root / 'password-p03_runtime_runtime'),
                             'ca_file': str(root / 'server.pem')},
            }
            config_path = root / 'runtime.json'
            config_path.write_text(json.dumps(config))
            config_path.chmod(0o600)
            process, error_file, state = await p06.start_runtime(
                fixture, args.runtime, config_path, probe_port, root / 'runtime.err')
            endpoint = f'127.0.0.1:{bridge_port}'

            def psql(sql):
                return fixture.psql(Path(args.pg_bin), root, port, runtime_password, sql)

            def action_state(action_id):
                output = psql(
                    'SELECT state,execution_outcome,dispatch_state,platform_message_id FROM '
                    f'kiarabot.actions WHERE action_id={fixture.quote_sql(action_id)}')
                text = output.stdout.decode().strip()
                return tuple(text.split('|')) if text else None

            async def wait_state(action_id, timeout=40):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    state = action_state(action_id)
                    if state is not None and state[0] != '0':
                        return state
                    await asyncio.sleep(0.1)
                raise AcceptanceFailure(f'action {action_id} did not reach terminal state')

            plugin_events, plugin_actions, cancel_errors = [], [], []
            write_manifest(root / 'plugin.json')
            manifest = sdk_runtime.load_manifest(root / 'plugin.json')

            class Subject(sdk_runtime.Plugin):
                def __init__(self):
                    self.context = None
                    self.submits = 0

                async def on_start(self, context):
                    self.context = context

                async def on_event(self, event, context):
                    # The P07 event path is observed separately; Action scenarios are invoked
                    # explicitly so the platform-response matrix is deterministic.
                    plugin_events.append(event.event_id)

                async def submit(self, *, cancel_after_dispatch=False):
                    import kiarabot.v1.actions_pb2 as actions
                    envelope = actions.ActionEnvelope(
                        action_id=str(uuid.uuid4()), idempotency_key=str(uuid.uuid4()),
                        trace_id=str(uuid.uuid4()))
                    envelope.deadline.FromDatetime(datetime.now(timezone.utc) +
                                                    timedelta(seconds=30))
                    envelope.target_source.adapter_id = ADAPTER_INSTANCE
                    envelope.target_source.account_id = ACCOUNT
                    envelope.target_source.adapter_instance_id = ADAPTER_INSTANCE
                    self.submits += 1
                    conversation = f'2000{self.submits}'
                    envelope.target.conversation_id = conversation
                    envelope.send_message.conversation.id = conversation
                    envelope.send_message.conversation.account_id = ACCOUNT
                    envelope.send_message.conversation.type = common.ConversationRef.GROUP
                    envelope.send_message.message.plain_text = 'e2e'
                    result = await self.context.submit_action(envelope)
                    plugin_actions.append(result.action_id)
                    if cancel_after_dispatch:
                        for _ in range(100):
                            state = action_state(result.action_id)
                            if state and state[2] == '1':
                                break
                            await asyncio.sleep(0.05)
                        try:
                            await self.context.cancel_action(result.action_id)
                        except sdk_runtime.PluginError as error:
                            cancel_errors.append(error.code)
                    return result.action_id

            subject = Subject()
            options = sdk_runtime.Options(
                endpoint, PLUGIN_BOOTSTRAP, tls_root_cert=ca_bytes,
                instance_id=PLUGIN_INSTANCE, io_timeout=5.0, hook_timeout=5.0,
                heartbeat_interval=0.2, retry_delay=0.1, lease_renew_interval=0.2)
            runner = sdk_runtime.Runner(manifest, subject, options)
            sdk_stop = asyncio.Event()
            sdk_task = asyncio.create_task(runner.run(sdk_stop))
            tasks.append((sdk_task, sdk_stop))

            # Event fan-out only snapshots currently connected plugin instances; wait for the
            # real SDK plugin registration before injecting the platform event.
            await p06.wait_until(
                lambda: psql('SELECT connection_generation FROM kiarabot.instances WHERE id=' +
                             fixture.quote_sql(PLUGIN_INSTANCE)).stdout.strip() not in (b'',),
                'SDK plugin registration', 25)
            bootstrap_file = root / 'adapter.bootstrap'
            bootstrap_file.write_text(ADAPTER_BOOTSTRAP + '\n')
            bootstrap_file.chmod(0o600)
            adapter_src = repo / 'adapters/nonebot/src'
            sdk_generated = repo / 'sdk/python/build/generated'
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith(('BRIDGE_', 'ADAPTER_', 'ACCOUNT_', 'OUTBOX_',
                                                  'HEALTH_', 'ONEBOT_', 'PYTHONPATH'))}
            environment.update(
                BRIDGE_ENDPOINT=endpoint, BRIDGE_CA_FILE=str(ca_path),
                BRIDGE_BOOTSTRAP_CREDENTIAL_FILE=str(bootstrap_file),
                ADAPTER_INSTANCE_ID=ADAPTER_INSTANCE, ACCOUNT_ID=ACCOUNT,
                OUTBOX_PATH=str(root / 'adapter-outbox.db'), HEALTH_PORT=str(free_port()),
                ONEBOT_WS_URL=f'ws://127.0.0.1:{onebot.port}',
                ONEBOT_ACCESS_TOKEN=ONEBOT_TOKEN, ONEBOT_SELF_ID='10001',
                PYTHONPATH=os.pathsep.join([str(adapter_src), str(sdk_generated)]))
            adapter_process = await asyncio.create_subprocess_exec(
                sys.executable, '-m', 'kiarabot_adapter_nonebot', cwd=repo / 'adapters/nonebot',
                env=environment, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            try:
                # ---- success: real plugin Action -> Runtime dispatch -> real adapter process
                # -> fake OneBot platform -> result/evidence -> durable receipt. ----
                await p06.wait_until(lambda: onebot.connections >= 1,
                                     'OneBot platform connection', 20)
                # AdapterBridge registration must precede Action dispatch; the platform WS
                # connection alone does not imply the Runtime action sink exists.
                await p06.wait_until(
                    lambda: psql('SELECT instance,connection_generation FROM kiarabot.adapters'
                                 ).stdout.strip() not in (b'',),
                    'adapter bridge registration', 30)
                await asyncio.sleep(0.3)
                # The adapter event ingress still runs separately: one mapped platform message
                # must be durably ACKed by Runtime even if plugin delivery is observed below.
                await onebot.send_event(message_event('msg-p07d', 'hello'))
                await asyncio.sleep(1.0)
                first = await subject.submit()
                await p06.wait_until(lambda: action_state(first) is not None, 'Action accepted', 20)
                await p06.wait_until(lambda: onebot.calls, 'adapter called fake OneBot', 30)
                require(onebot.calls[0]['action'] == 'send_group_msg',
                        'unexpected platform API call')
                success = await wait_state(first)
                require(success[:2] == ('1', '1'),
                        f'success Action not KNOWN_SUCCEEDED: {success}')
                require(success[3] not in ('', '\\N'),
                        f'platform message id missing: {success}')
                # The adapter outbox proves the platform event was durably accepted by Runtime
                # before Action dispatch; plugin event fan-out is asserted separately in P07c.
                import sqlite3
                with sqlite3.connect(root / 'adapter-outbox.db') as db:
                    require(db.execute("SELECT count(*) FROM outbox WHERE status='acked'"
                                       ).fetchone()[0] >= 1,
                            'adapter platform event was not ACKed by Runtime')

                # ---- platform failure: retcode maps to known-failed, never success ----
                onebot.mode = 'platform_fail'
                second = await subject.submit()
                await p06.wait_until(lambda: len(plugin_actions) >= 2,
                                     'second plugin Action', 30)
                failed = await wait_state(second)
                require(failed[:2] == ('2', '2'),
                        f'platform failure not KNOWN_FAILED: {failed}')

                # ---- duplicate platform event: dedupe means no third plugin Action ----
                onebot.mode = 'success'
                events_before = int(psql('SELECT count(*) FROM kiarabot.events'
                                         ).stdout.decode().strip() or '0')
                await onebot.send_event(message_event('msg-p07d', 'hello'))
                await asyncio.sleep(1.0)
                events_after = int(psql('SELECT count(*) FROM kiarabot.events'
                                        ).stdout.decode().strip() or '0')
                require(events_after == events_before,
                        'duplicate platform frame created a second Runtime event')
                require(len(plugin_actions) == 2,
                        'duplicate platform frame created another Action')

                # ---- cancellation after dispatch: CONFLICT, no fake success ----
                third = await subject.submit(cancel_after_dispatch=True)
                await p06.wait_until(lambda: len(plugin_actions) >= 3,
                                     'third plugin Action', 30)
                await p06.wait_until(lambda: bool(cancel_errors),
                                     'post-dispatch cancel conflict', 30)
                require(common.CONFLICT in cancel_errors,
                        f'post-dispatch cancel did not conflict: {cancel_errors}')
                third_state = await wait_state(third)
                require(third_state[0] == '1',
                        f'cancelled-after-dispatch Action was rewritten: {third_state}')

                # ---- adapter crash/restart: platform disconnect + adapter reconnect, then one
                # more Action completes without duplicate platform side effects. ----
                for socket in list(onebot._sockets):
                    await socket.close()
                await p06.wait_until(lambda: onebot.connections >= 2,
                                     'adapter platform reconnect', 30)
                before = len(onebot.calls)
                fourth = await subject.submit()
                await p06.wait_until(lambda: len(plugin_actions) >= 4,
                                     'Action after reconnect', 30)
                reconnected = await wait_state(fourth)
                require(reconnected[0] == '1',
                        f'Action after adapter platform reconnect failed: {reconnected}')
                require(len(onebot.calls) == before + 1,
                        'reconnect duplicated a platform call')
            finally:
                adapter_output = b''
                if adapter_process.returncode is None:
                    adapter_process.terminate()
                    try:
                        adapter_output, adapter_error = await asyncio.wait_for(
                            adapter_process.communicate(), 15)
                    except asyncio.TimeoutError:
                        adapter_process.kill()
                        adapter_output, adapter_error = await adapter_process.communicate()
                require(ADAPTER_BOOTSTRAP.encode() not in adapter_output,
                        'adapter logs leaked bootstrap credential')
                require(ONEBOT_TOKEN.encode() not in adapter_output,
                        'adapter logs leaked platform token')
            sdk_stop.set()
            await asyncio.wait_for(sdk_task, 15)
            process.send_signal(signal.SIGTERM)
            await p06.wait_until(lambda: process.poll() is not None, 'Runtime stop', 10)
            require(process.returncode == 0, 'Runtime did not exit cleanly')
            process = None
    finally:
        await onebot.stop()
        for task, stop in tasks:
            if stop is not None:
                stop.set()
            if task is not None and not task.done():
                task.cancel()
                try:
                    await asyncio.wait_for(task, 5)
                except (asyncio.CancelledError, asyncio.TimeoutError, Exception):
                    pass
        if adapter_process is not None and adapter_process.returncode is None:
            adapter_process.kill()
            await adapter_process.wait()
        if process is not None and process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)
        import shutil
        if (root / 'data/postmaster.pid').exists():
            import subprocess
            subprocess.run([Path(args.pg_bin) / 'pg_ctl', '-D', root / 'data', '-m', 'immediate',
                            '-w', '-t', '20', 'stop'], capture_output=True, timeout=30)
        shutil.rmtree(root, ignore_errors=True)
    print('P07d real NoneBot adapter + fake OneBot + real Runtime full-chain e2e passed')
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', required=True)
    parser.add_argument('--seed', required=True)
    parser.add_argument('--initializer', required=True)
    parser.add_argument('--openssl', required=True)
    parser.add_argument('--pg-bin', required=True)
    parser.add_argument('--repo-source', required=True)
    args = parser.parse_args()
    asyncio.run(async_main(args))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
