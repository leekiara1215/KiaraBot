#!/usr/bin/env python3
"""P08b-3b HostBridge e2e: real PG16.4 + real Runtime process + fake mTLS Host client.

Covers the Runtime side of the P08 managed-host path end to end without the real
kiarabot-plugin-host binary (that remains the 3b host-process leg): deployment
allowlist registration, dispatch of a seeded host_command apply frame, receipt and
observation persistence in PG, duplicate observation dedup, reconnect generation and
old-fence rejection. It does not claim the P08 exit gate while the real Host binary
leg is missing.
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time

FINGERPRINT_QUERY = (
    "SELECT state,receipt_persisted FROM kiarabot.host_commands "
    "WHERE command_id='hc-e2e'")
COUNT_QUERY = "SELECT count(*) FROM kiarabot.host_observations WHERE host_id='host-a'"


class Failure(RuntimeError):
    pass


def require(condition, label):
    if not condition:
        raise Failure(label)


def load_fixture(repo: Path):
    sys.path.insert(0, str(repo / 'tests/architecture'))
    import p06_bridge_acceptance as fixture
    return fixture


def load_protos(repo: Path):
    for path in (repo / 'sdk/python/build/generated', repo / 'sdk/python/src'):
        require(path.is_dir(), f'missing generated/proto path: {path}')
        sys.path.insert(0, str(path))
    from kiarabot.v1 import common_pb2, host_pb2, host_pb2_grpc, platform_pb2
    import grpc
    return grpc, common_pb2, host_pb2, host_pb2_grpc, platform_pb2


def start_runtime(runtime, config_path, error_path):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('KIARABOT_', 'CORE_', 'ONEBOT_', 'PG'))}
    error_file = error_path.open('wb')
    process = subprocess.Popen([runtime, '--config', str(config_path)], cwd=config_path.parent,
                               env=env, stdout=subprocess.PIPE, stderr=error_file,
                               start_new_session=True)
    if not select.select([process.stdout], [], [], 10)[0]:
        process.kill()
        raise Failure('Runtime did not report startup')
    line = process.stdout.readline().decode(errors='replace').strip()
    require('grpc_port=' in line, f'Runtime startup incomplete: {line[:300]}')
    return process, error_file, dict(item.split('=', 1) for item in line.split())


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


async def main_async(args) -> None:
    repo = Path(args.repo_source).resolve()
    fixture = load_fixture(repo)
    grpc, common, host_pb2, host_grpc, platform = load_protos(repo)
    from fixtures import TestPki

    root = Path(tempfile.mkdtemp(prefix='kiarabot-p08-e2e-')).resolve()
    root.chmod(0o700)
    process = None
    error_file = None
    try:
        port = fixture.start_postgres(Path(args.pg_bin).resolve(), root,
                                      Path(args.openssl).resolve(), None)
        admin = (root / 'password').read_text().strip()
        env = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
        env['PGPASSWORD'] = admin
        env['LC_ALL'] = 'C'
        admin_dsn = (f'host=localhost port={port} dbname=postgres user=p03_admin '
                     f'sslmode=verify-full sslrootcert={root / "server.pem"}')
        runtime_password = (root / 'password-p03_runtime_runtime').read_text().strip()
        psql_admin = [Path(args.pg_bin) / 'psql', admin_dsn, '-tA', '-v',
                      'ON_ERROR_STOP=1', '-c']
        fixture.run([*psql_admin,
                     f"CREATE ROLE p03_runtime_migration LOGIN NOINHERIT NOSUPERUSER "
                     f"NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD "
                     f"{fixture.quote_sql((root / 'password-p03_runtime_migration').read_text().strip())}"],
                    env=env)
        fixture.run([*psql_admin,
                     f"CREATE ROLE p03_runtime_runtime LOGIN NOINHERIT NOSUPERUSER "
                     f"NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD "
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

        with TestPki(Path(args.openssl).resolve()) as pki:
            server_leaf = pki.servers['runtime']
            host_leaf = pki.client
            host_der = subprocess.check_output(
                [args.openssl, 'x509', '-in', str(host_leaf.certificate), '-outform', 'der'])
            fingerprint = hashlib.sha256(host_der).hexdigest()
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
                'hosts': [{'host_id': 'host-a', 'certificate_sha256': fingerprint}],
            }
            config_path = root / 'runtime.json'
            config_path.write_text(json.dumps(config))
            config_path.chmod(0o600)
            now_sql = "(extract(epoch from clock_timestamp())*1000000000)::bigint"
            fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                         "INSERT INTO kiarabot.host_commands("
                         "command_id,host_id,instance_id,installation_id,kind,"
                         "execution_generation,desired_version,desired_state,package_sha256,"
                         "launch_profile,payload_sha256,state,created_at,updated_at) VALUES("
                         "'hc-e2e','host-a','instance-1','installation-1',1,1,1,0,"
                         f"repeat('a',64),'python',repeat('b',64),0,{now_sql},{now_sql})")
            process, error_file, state = start_runtime(args.runtime, config_path,
                                                       root / 'runtime.err')
            endpoint = f'127.0.0.1:{state["grpc_port"]}'
            channel = grpc.aio.secure_channel(
                endpoint,
                grpc.ssl_channel_credentials(
                    root_certificates=pki.ca.certificate.read_bytes(),
                    private_key=host_leaf.key.read_bytes(),
                    certificate_chain=host_leaf.certificate.read_bytes()))
            stub = host_grpc.HostBridgeStub(channel)
            stream = stub.Connect()
            registration = host_pb2.HostToRuntime(
                registration=host_pb2.HostRegisterHostRequest(
                    host=host_pb2.HostRecord(host_id='host-a')))
            await stream.write(registration)
            registered = await stream.read()
            require(registered.WhichOneof('body') == 'registered', 'registration rejected')
            fence = registered.registered.fence
            require(fence.instance_id == 'host-a' and fence.connection_generation == 1,
                    'unexpected registration fence')
            dispatched = await asyncio.wait_for(stream.read(), timeout=5)
            if dispatched.WhichOneof('body') != 'apply':
                error = dispatched.failure.error
                error_file.flush()
                raise Failure(f'apply not dispatched: {dispatched.WhichOneof("body")} '
                              f'code={error.code} message={error.message}; '
                              f'runtime_stderr={(root / "runtime.err").read_text(errors="replace")[-1500:]}')
            require(dispatched.apply.command_id == 'hc-e2e' and
                    dispatched.apply.process.instance_id == 'instance-1' and
                    dispatched.apply.process.installation_id == 'installation-1' and
                    dispatched.apply.process.execution_generation == 1 and
                    dispatched.apply.process.desired_version == 1,
                    'dispatched apply snapshot mismatch')
            execution = host_pb2.ExecutionRecord(
                host_id='host-a', instance_id='instance-1', execution_generation=1,
                desired_version=1, outcome=platform.EXECUTION_KNOWN_SUCCEEDED,
                process_marker='marker-e2e', command_id='hc-e2e')
            execution.observed_at.GetCurrentTime()
            await stream.write(host_pb2.HostToRuntime(
                fence=fence, receipt=host_pb2.HostCommandReceipt(
                    command_id='hc-e2e', persisted=True, execution=execution)))
            observation = host_pb2.HostToRuntime(
                fence=fence,
                observation=host_pb2.HostWatchExecutionsResponse(
                    execution=execution,
                    cursor=platform.StreamCursor(
                        source_service='host', epoch=fence.runtime_epoch, sequence=1)))
            await stream.write(observation)
            from google.protobuf.timestamp_pb2 import Timestamp
            heartbeat = Timestamp()
            heartbeat.GetCurrentTime()
            await stream.write(host_pb2.HostToRuntime(
                fence=fence, heartbeat=host_pb2.HostHeartbeat(fence=fence, sent_at=heartbeat)))
            ack = await asyncio.wait_for(stream.read(), timeout=5)
            require(ack.WhichOneof('body') == 'heartbeat_ack', 'heartbeat ack missing')
            # Duplicate observation must be accepted idempotently and not create a second row.
            await stream.write(observation)
            heartbeat.GetCurrentTime()
            await stream.write(host_pb2.HostToRuntime(
                fence=fence, heartbeat=host_pb2.HostHeartbeat(fence=fence, sent_at=heartbeat)))
            ack = await asyncio.wait_for(stream.read(), timeout=5)
            require(ack.WhichOneof('body') == 'heartbeat_ack', 'duplicate observation failed')
            state_row = fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                                     FINGERPRINT_QUERY).stdout.decode().strip()
            require(state_row.startswith('2|t'), f'command receipt not persisted: {state_row}')
            require(fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                                 COUNT_QUERY).stdout.decode().strip() == '1',
                    'observation not deduplicated')
            await stream.done_writing()
            # Reconnect gets a new connection generation; an old fence must be rejected.
            stream = stub.Connect()
            await stream.write(registration)
            second = await stream.read()
            require(second.WhichOneof('body') == 'registered', 'second registration rejected')
            require(second.registered.fence.connection_generation == 2,
                    'second generation not advanced')
            old_heartbeat = Timestamp()
            old_heartbeat.GetCurrentTime()
            await stream.write(host_pb2.HostToRuntime(
                fence=fence,
                heartbeat=host_pb2.HostHeartbeat(fence=fence, sent_at=old_heartbeat)))
            rejected = await asyncio.wait_for(stream.read(), timeout=5)
            require(rejected.WhichOneof('body') == 'failure',
                    f'old fence was not rejected: {rejected.WhichOneof("body")}')
            await stream.done_writing()
            await channel.close()
            require(fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                                 COUNT_QUERY).stdout.decode().strip() == '1',
                    'observation count changed after reconnect')
            print('P08 HostBridge e2e: PASS')
    finally:
        stop_process(process)
        if error_file:
            error_file.close()
        fixture = load_fixture(Path(args.repo_source).resolve())
        fixture.run([Path(args.pg_bin) / 'pg_ctl', '-D', str(root / 'data'), '-m',
                     'immediate', '-w', '-t', '20', 'stop'], timeout=30,
                    env={key: value for key, value in os.environ.items()
                         if not key.startswith('PG')})
        import shutil
        shutil.rmtree(root, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', required=True)
    parser.add_argument('--initializer', required=True)
    parser.add_argument('--openssl', required=True)
    parser.add_argument('--pg-bin', required=True)
    parser.add_argument('--repo-source', required=True)
    args = parser.parse_args()
    try:
        asyncio.run(main_async(args))
        return 0
    except Exception as error:  # noqa: BLE001 - test harness reports a single bounded failure
        print(f'P08 HostBridge e2e FAILED: {type(error).__name__}: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
