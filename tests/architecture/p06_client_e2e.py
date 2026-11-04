#!/usr/bin/env python3
"""P06 full-chain acceptance: real Runtime Bridge with the real SDK plugin and Adapter client.

Unlike p06_bridge_acceptance.py (a manual wire client), this test runs the shipped
kiarabot_sdk Runner and the shipped NoneBot adapter BridgeClient against a real
kiarabot-runtime process with real PostgreSQL and TLS. It covers registration /
ConnectionAuthorization / heartbeat / event delivery lease + ACK / duplicate
reconnect generation / graceful ShutdownNotice drain, and observes the periodic
proof refresh of the real adapter client.

The NoneBot/OneBot platform process and real QQ are intentionally out of scope
(P07/M2); the adapter's target BridgeClient and durable outbox are the real P06c
components under test.
"""
from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path
import signal
import sqlite3
import sys
import tempfile
import time
import uuid

ADAPTER_INSTANCE = '33333333-3333-4333-8333-333333333333'
ACCOUNT = '11111111-1111-4111-8111-111111111111'
PLUGIN_INSTANCE = '44444444-4444-4444-8444-444444444444'
INSTALLATION = '55555555-5555-4555-8555-555555555555'
ADAPTER_ID = '88888888-8888-4888-8888-888888888888'
ADAPTER_BOOTSTRAP = 'a' * 64
PLUGIN_BOOTSTRAP = 'b' * 64
SHUTDOWN_INSTANCE = 'a1a1a1a1-a1a1-4a1a-8a1a-a1a1a1a1a1a1'
SHUTDOWN_BOOTSTRAP = 'c' * 64


class AcceptanceFailure(RuntimeError):
    pass


def require(condition, label):
    if not condition:
        raise AcceptanceFailure(label)


def load_fixture(repo: Path):
    tests_architecture = repo / 'tests/architecture'
    sys.path.insert(0, str(tests_architecture))
    import p06_bridge_acceptance as fixture  # reuse owned PG/PKI helpers only
    return fixture


def load_clients(repo: Path):
    sdk_generated = repo / 'sdk/python/build/generated'
    sdk_src = repo / 'sdk/python/src'
    adapter_src = repo / 'adapters/nonebot/src'
    for path in (sdk_generated, sdk_src, adapter_src):
        require(path.is_dir(), f'client path missing: {path}')
        sys.path.insert(0, str(path))
    from kiarabot.v1 import common_pb2 as common
    from kiarabot.v1 import platform_pb2 as platform
    from kiarabot.v1 import plugins_pb2 as plugins
    from kiarabot_sdk import runtime
    from kiarabot_adapter_nonebot.bridge import BridgeConfig
    from kiarabot_adapter_nonebot.client import BridgeClient
    from kiarabot_adapter_nonebot.mapper import map_message
    from kiarabot_adapter_nonebot.outbox import Outbox
    return (common, platform, plugins, runtime, BridgeConfig, BridgeClient, map_message, Outbox)


def write_manifest(path: Path) -> None:
    path.write_text('''{
      "id": "p06.e2e.plugin",
      "name": "P06 E2E Plugin",
      "version": "1.0.0",
      "api_version": "kiarabot.plugin.v1",
      "runtime": "python-rpc",
      "entrypoint": "p06_e2e_plugin:factory",
      "isolation": "process",
      "ordered": false,
      "capabilities": ["event.message.received"],
      "permissions": ["message.read"]
    }''')


async def wait_until(predicate, label, timeout=15.0, interval=0.02):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        await asyncio.sleep(interval)
    raise AcceptanceFailure(f'{label}: timed out after {timeout}s')


async def start_runtime(fixture, process_binary, config_path, probe_port, error_path):
    child_env = {key: value for key, value in __import__('os').environ.items()
                 if not key.startswith(('KIARABOT_', 'CORE_', 'ONEBOT_'))}
    error_file = error_path.open('wb')
    process = __import__('subprocess').Popen(
        [process_binary, '--config', str(config_path)], cwd=config_path.parent,
        env=child_env, stdout=__import__('subprocess').PIPE, stderr=error_file,
        start_new_session=True)
    require(__import__('select').select([process.stdout], [], [], 10)[0],
            'Runtime did not report startup')
    line = process.stdout.readline().decode(errors='replace').strip()
    require('bridge_port=' in line and 'grpc_port=' in line,
            f'Runtime startup incomplete: {line[:300]}')
    return process, error_file, dict(item.split('=', 1) for item in line.split())


async def async_main(args) -> None:
    repo = Path(args.repo_source).resolve()
    fixture = load_fixture(repo)
    common, platform, plugins, sdk_runtime, BridgeConfig, BridgeClient, map_message, Outbox = (
        load_clients(repo))
    grpc = fixture._grpc()
    from fixtures import TestPki

    root = Path(tempfile.mkdtemp(prefix='kiarabot-p06-e2e-')).resolve()
    root.chmod(0o700)
    process = None
    error_file = None
    tasks = []
    try:
        port = fixture.start_postgres(Path(args.pg_bin).resolve(), root,
                                      Path(args.openssl).resolve(), None)
        admin = (root / 'password').read_text().strip()
        env = {key: value for key, value in __import__('os').environ.items()
               if not key.startswith('PG')}
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
            probe_port, grpc_port, bridge_port = (fixture.free_port(), fixture.free_port(),
                                                  fixture.free_port())
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
            process, error_file, state = await start_runtime(
                fixture, args.runtime, config_path, probe_port, root / 'runtime.err')
            require(int(state['bridge_port']) == bridge_port, 'bridge port mismatch')
            endpoint = f'127.0.0.1:{bridge_port}'

            # ---- real P06b SDK plugin (shipped Runner + manifest) ---------------------
            manifest_path = root / 'plugin.json'
            write_manifest(manifest_path)
            manifest = sdk_runtime.load_manifest(manifest_path)
            delivered: asyncio.Queue = asyncio.Queue()
            dispositions: list[int] = []
            hooks = {'start': 0, 'stop': 0}

            class E2ePlugin(sdk_runtime.Plugin):
                async def on_start(self, context):
                    hooks['start'] += 1

                async def on_event(self, event, context):
                    delivered.put_nowait(event.event_id)
                    return None

                async def on_stop(self, reason):
                    hooks['stop'] += 1

            options = sdk_runtime.Options(
                endpoint, PLUGIN_BOOTSTRAP, tls_root_cert=ca_bytes,
                instance_id=PLUGIN_INSTANCE, io_timeout=5.0, hook_timeout=5.0,
                heartbeat_interval=0.2, retry_delay=0.1, lease_renew_interval=0.2)
            runner = sdk_runtime.Runner(manifest, E2ePlugin(), options)
            sdk_stop = asyncio.Event()
            sdk_task = asyncio.create_task(runner.run(sdk_stop))
            tasks.append((sdk_task, sdk_stop))

            # ---- real P06c adapter BridgeClient + durable outbox ----------------------
            bootstrap_file = root / 'adapter.bootstrap'
            bootstrap_file.write_text(ADAPTER_BOOTSTRAP + '\n')
            bootstrap_file.chmod(0o600)
            adapter_config = BridgeConfig(
                endpoint=endpoint, ca_file=str(ca_path), bootstrap_file=str(bootstrap_file),
                adapter_instance_id=ADAPTER_INSTANCE, account_id=ACCOUNT,
                adapter_id=ADAPTER_ID, timeout=5.0, retry_base=0.1, idle_interval=0.1)
            outbox_path = root / 'adapter-outbox.db'
            outbox = Outbox(outbox_path)
            event_receipts = []

            class ObservedBridgeClient(BridgeClient):
                def handle_event_ack(self, event, ack):
                    # Observe real server replies while retaining the shipped client's handling.
                    event_receipts.append((event.event_id, event.sequence, event.trace_id,
                                           ack.error.code, ack.error.retryable, ack.retry_after_ms))
                    return super().handle_event_ack(event, ack)

            adapter_client = ObservedBridgeClient(adapter_config, outbox)
            adapter_stop = asyncio.Event()
            adapter_task = asyncio.create_task(adapter_client.run(adapter_stop))
            tasks.append((adapter_task, adapter_stop))

            def adapter_ready():
                return adapter_client.authorized and adapter_client.instance == ADAPTER_INSTANCE

            await wait_until(adapter_ready, 'adapter registration/authorization', 20.0)
            # P06c assumptions against the real server.
            require(adapter_client.authorization.principal_id == ADAPTER_INSTANCE,
                    'adapter principal_id mismatch')
            require(adapter_client.authorization.fence.instance_id == ADAPTER_INSTANCE,
                    'adapter assigned_instance_id mismatch')
            require(adapter_client.authorization.fence.execution_generation > 0 and
                    adapter_client.authorization.fence.connection_generation > 0,
                    'adapter fence missing generations')
            require(adapter_client.heartbeat_interval == 30.0,
                    f'adapter heartbeat_interval {adapter_client.heartbeat_interval}')
            require(adapter_client.authorization.refresh_generation == 1 and
                    adapter_client.authorization.credential_version > 0 and
                    adapter_client.authorization.authorization_version > 0,
                    'adapter ConnectionAuthorization fields incomplete')
            require(adapter_client.effective_capabilities == frozenset(),
                    'adapter negotiated unexpected capabilities')

            # ---- event: real adapter mapper -> outbox -> Runtime -> real SDK plugin ---
            raw_message = {
                'post_type': 'message', 'message_type': 'group', 'self_id': ACCOUNT,
                'user_id': '10002', 'group_id': '20001', 'message_id': 'msg-1',
                'time': int(time.time()), 'message': [{'type': 'text', 'data': {'text': 'hi'}}],
                'original_message': [{'type': 'text', 'data': {'text': 'hi'}}],
            }
            event = map_message(raw_message, ADAPTER_ID, ACCOUNT)
            event.source.adapter_id = ADAPTER_ID
            # Only unrelated capacity occupants are provisioned. The measured event enters
            # through the real mapper, durable SQLite outbox and production BridgeClient.
            fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                         "INSERT INTO kiarabot.events(adapter,event_id,fingerprint,"
                         "conversation_key,payload_kind,has_conversation,sequence,has_sequence,"
                         "serialized_envelope,occurred_at,state,received_at) "
                         "SELECT 'p06-capacity','capacity-'||n,decode(repeat('00',32),'hex'),"
                         "'',0,false,NULL,false,''::bytea,1,0,1 FROM generate_series(1,1024) n")
            prior_generation = adapter_client.authorization.fence.connection_generation
            outbox.enqueue(event)
            await wait_until(lambda: any(row[3] == common.RATE_LIMITED for row in event_receipts),
                             'real Runtime capacity rejection', 20.0)
            rejection = next(row for row in event_receipts if row[3] == common.RATE_LIMITED)
            require(rejection[4] and rejection[5] > 0, 'backpressure must be retryable with delay')
            with sqlite3.connect(outbox_path) as persisted:
                row = persisted.execute('SELECT status,payload FROM outbox WHERE id=?',
                                        (event.event_id,)).fetchone()
            require(row is not None and row[0] == 'pending', 'rejected event remains durable')
            pending = type(event).FromString(row[1])
            require((pending.event_id, pending.sequence, pending.trace_id) == rejection[:3],
                    'capacity rejection changed the durable event identity')
            missing = fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                                   'SELECT count(*) FROM kiarabot.events WHERE event_id=' +
                                   fixture.quote_sql(event.event_id))
            require(missing.stdout.strip() == b'0', 'rejected event was committed')
            fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                         "DELETE FROM kiarabot.events WHERE adapter='p06-capacity'")
            first_event = await asyncio.wait_for(delivered.get(), 20)
            require(first_event == event.event_id, 'SDK plugin received a different event id')
            require(adapter_client.authorization.fence.connection_generation > prior_generation,
                    'backpressure did not reconnect the adapter')
            await wait_until(lambda: outbox.next_pending() is None, 'adapter event ACK', 20.0)
            successes = [row for row in event_receipts if row[0] == event.event_id and row[3] == 0]
            require(successes and all(row[:3] == rejection[:3] for row in successes),
                    'backpressure retry changed event id, sequence or trace')
            await wait_until(
                lambda: platform.DELIVERY_ACKNOWLEDGED in runner.last_dispositions,
                'SDK delivery ACK receipt', 20.0)
            rows = fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                                'SELECT state FROM kiarabot.delivery_targets')
            require(rows.stdout.split() == [b'3'], f'delivery target not acked: {rows.stdout!r}')
            unique = fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                                  'SELECT count(*) FROM kiarabot.events WHERE event_id=' +
                                  fixture.quote_sql(event.event_id))
            require(unique.stdout.strip() == b'1' and delivered.empty(),
                    'capacity retry duplicated an event or plugin delivery')

            # ---- periodic proof refresh of the real adapter client -------------------
            before_refresh = adapter_client.authorization.refresh_generation
            await wait_until(
                lambda: adapter_client.authorized and
                adapter_client.authorization.refresh_generation > before_refresh,
                'adapter ConnectionAuthorization refresh', 60.0, interval=0.2)
            require(adapter_client.authorization.fence.instance_id == ADAPTER_INSTANCE,
                    'refresh changed the adapter fence')

            # A post-refresh event proves the refreshed proof/fence is usable end to end.
            raw_message['message_id'] = 'msg-2'
            raw_message['time'] = int(time.time())
            event2 = map_message(raw_message, ADAPTER_ID, ACCOUNT)
            event2.source.adapter_id = ADAPTER_ID
            outbox.enqueue(event2)
            second_event = await asyncio.wait_for(delivered.get(), 20)
            require(second_event == event2.event_id, 'post-refresh event not delivered')
            await wait_until(lambda: outbox.next_pending() is None,
                             'post-refresh adapter event ACK', 20.0)
            # The adapter ACK only confirms inbox persistence. Wait for the SDK plugin's delivery
            # ACK to make the event terminal before killing the Runtime, otherwise the new process
            # legitimately redelivers the still-open event after restart.
            await wait_until(
                lambda: fixture.psql(
                    Path(args.pg_bin), root, port, runtime_password,
                    'SELECT state FROM kiarabot.events WHERE event_id='
                    f'{fixture.quote_sql(event2.event_id)}').stdout.strip() == b'1',
                'SDK event2 terminal ACK', 20.0)

            # ---- ungraceful Runtime loss and reconnect (new connection generation) ---
            first_fence = adapter_client.authorization.fence
            plugin_generation_before = int(fixture.psql(
                Path(args.pg_bin), root, port, runtime_password,
                f"SELECT connection_generation FROM kiarabot.instances WHERE "
                f"id={fixture.quote_sql(PLUGIN_INSTANCE)}").stdout.strip() or b'0')
            import os
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
            error_file.close()
            require(process.returncode != 0, 'SIGKILL Runtime unexpectedly exited cleanly')
            await wait_until(lambda: not adapter_client.authorized,
                             'adapter detected the lost Runtime', 10.0)
            process, error_file, state = await start_runtime(
                fixture, args.runtime, config_path, probe_port, root / 'runtime.err')
            await wait_until(
                lambda: adapter_ready() and
                adapter_client.authorization.fence.connection_generation >
                first_fence.connection_generation,
                'adapter reconnect with a new connection_generation', 20.0)
            # Wait until the real SDK plugin has re-registered on the restarted Runtime before
            # accepting the next event: P06 fan-out only snapshots currently connected instances.
            await wait_until(
                lambda: int(fixture.psql(
                    Path(args.pg_bin), root, port, runtime_password,
                    f"SELECT connection_generation FROM kiarabot.instances WHERE "
                    f"id={fixture.quote_sql(PLUGIN_INSTANCE)}").stdout.strip() or b'0') >
                plugin_generation_before,
                'SDK plugin reconnect registration', 20.0, interval=0.1)
            # The persisted generation is committed before the bridge publishes the reconnect
            # profile/sink; one bounded beat avoids accepting into a not-yet-fanned-out session.
            await asyncio.sleep(1.5)
            raw_message['message_id'] = 'msg-3'
            raw_message['time'] = int(time.time())
            event3 = map_message(raw_message, ADAPTER_ID, ACCOUNT)
            event3.source.adapter_id = ADAPTER_ID
            outbox.enqueue(event3)
            try:
                third_event = await asyncio.wait_for(delivered.get(), 20)
            except asyncio.TimeoutError as error:
                targets = fixture.psql(
                    Path(args.pg_bin), root, port, runtime_password,
                    'SELECT instance,state,attempts FROM kiarabot.delivery_targets')
                raise AcceptanceFailure(
                    f'reconnected event not delivered; targets={targets.stdout!r} '
                    f'outbox_pending={outbox.next_pending() is not None} '
                    f'adapter_authorized={adapter_client.authorized}') from error
            require(third_event == event3.event_id,
                    f'reconnected event mismatch: got {third_event!r} expected {event3.event_id!r}')

            # ---- graceful SIGTERM: real clients observe ShutdownNotice and drain -----
            process.send_signal(signal.SIGTERM)
            await wait_until(lambda: adapter_client.shutdown_seen,
                             'adapter ShutdownNotice', 10.0)
            # A deployment orchestrator stops the adapter after the bounded drain window; the
            # client itself emits the notice observation and would otherwise reconnect.
            adapter_stop.set()
            await asyncio.wait_for(adapter_task, 10)
            require(adapter_client.fatal == 0, 'adapter reported a fatal failure')
            await asyncio.wait_for(sdk_task, 10)
            require(hooks['start'] >= 1 and hooks['stop'] == 1,
                    'SDK plugin lifecycle hooks incomplete')
            require(process.wait(timeout=8) == 0, 'Runtime did not exit cleanly')
            output = (root / 'runtime.err').read_bytes()
            require(b'PRIVATE KEY' not in output and b'password=' not in output,
                    'Runtime diagnostics leaked secret material')
            process = None
            print('P06 real SDK+Adapter full-chain acceptance passed '
                  '(register/auth/refresh/event/lease+ACK/reconnect/shutdown)')
    finally:
        for task, stop in tasks:
            stop.set()
            if not task.done():
                task.cancel()
            try:
                await asyncio.wait_for(task, 5)
            except (asyncio.CancelledError, asyncio.TimeoutError, Exception):
                pass
        if process is not None and process.poll() is None:
            try:
                __import__('os').killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)
        if error_file is not None:
            error_file.close()
        pg_bin = Path(args.pg_bin)
        if (root / 'data/postmaster.pid').exists():
            __import__('subprocess').run(
                [pg_bin / 'pg_ctl', '-D', root / 'data', '-m', 'immediate', '-w', '-t', '20',
                 'stop'], capture_output=True, timeout=30)
        __import__('shutil').rmtree(root, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', required=True)
    parser.add_argument('--seed', required=True)
    parser.add_argument('--initializer', required=True)
    parser.add_argument('--openssl', required=True)
    parser.add_argument('--pg-bin', required=True)
    parser.add_argument('--repo-source', required=True)
    arguments = parser.parse_args()
    asyncio.run(async_main(arguments))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
