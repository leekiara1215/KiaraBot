#!/usr/bin/env python3
"""P13g TEST-ONLY joint lifecycle e2e.

Real PG16.4 + real mTLS Runtime (test-composition binary) + fake mTLS HostBridge client. The
Scheduler participant is an injected fake supplied only by the test-composition binary; the
production Runtime remains fail-closed until P13f option A/B is confirmed. This is not production
evidence and is labelled test-only.
"""
from __future__ import annotations

import argparse
import asyncio
from pathlib import Path
import json
import os
import select
import signal
import subprocess
import sys
import tempfile
import time
import traceback
import uuid

HOST_ID = 'host-a'
INSTALLATION = 'installation-1'
PLUGIN_TYPE = 'plugin-type'


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
    sys.path.insert(0, str(repo / 'sdk/python/build/generated'))
    import grpc
    from kiarabot.v1 import (common_pb2, host_pb2, platform_pb2, runtime_pb2,
                             runtime_pb2_grpc)
    return grpc, common_pb2, host_pb2, platform_pb2, runtime_pb2, runtime_pb2_grpc


def start_runtime(runtime, config_path, root):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('KIARABOT_', 'CORE_', 'PG', 'P13G_'))}
    env['KIARABOT_P13G_SCHEDULER_MODE_FILE'] = str(root / 'scheduler-mode')
    env['KIARABOT_P13G_SCHEDULER_STATE'] = str(root / 'scheduler-state')
    (root / 'scheduler-mode').write_text('done\n')
    error_file = (root / 'runtime.err').open('wb')
    process = subprocess.Popen([runtime, '--config', str(config_path)], cwd=root, env=env,
                               stdout=subprocess.PIPE, stderr=error_file, start_new_session=True)
    if not select.select([process.stdout], [], [], 15)[0]:
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


async def read(stream, label, timeout=10.0):
    try:
        value = await asyncio.wait_for(stream.read(), timeout)
    except asyncio.TimeoutError as error:
        raise Failure(f'{label}: stream read timed out') from error
    require(value is not None, f'{label}: stream closed')
    return value


async def main_async(args) -> None:
    repo = Path(args.repo_source).resolve()
    fixture = load_fixture(repo)
    grpc, common, host_pb2, platform, runtime_pb2, runtime_grpc = load_protos(repo)
    from fixtures import TestPki

    root = Path(tempfile.mkdtemp(prefix='kiarabot-p13g-')).resolve()
    root.chmod(0o700)
    process = None
    error_file = None
    channel = None
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
                     "CREATE ROLE p03_runtime_migration LOGIN NOINHERIT NOSUPERUSER NOCREATEDB "
                     f"NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD {fixture.quote_sql(migration)}"],
                    env=env)
        fixture.run([*psql_admin,
                     "CREATE ROLE p03_runtime_runtime LOGIN NOINHERIT NOSUPERUSER NOCREATEDB "
                     f"NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD {fixture.quote_sql(runtime_password)}"],
                    env=env)
        fixture.run([*psql_admin, 'CREATE DATABASE p03_runtime OWNER p03_runtime_migration'], env=env)
        fixture.run([*psql_admin, 'REVOKE ALL ON DATABASE p03_runtime FROM PUBLIC'], env=env)
        fixture.run([*psql_admin, 'GRANT CONNECT ON DATABASE p03_runtime TO p03_runtime_runtime'],
                    env=env)
        fixture.run([args.initializer, '--initialize-empty', 'runtime', 'localhost', str(port),
                     'p03_runtime', 'p03_runtime_migration', 'p03_runtime_runtime',
                     str(root / 'server.pem'), str(root / 'password-p03_runtime_migration')], env=env)

        def psql(sql):
            return fixture.psql(Path(args.pg_bin), root, port, runtime_password, sql)

        def seed_instance(instance: str, credential: bool = False):
            psql("INSERT INTO kiarabot.installations(id,plugin_type,version,state,manifest) "
                 f"VALUES ({fixture.quote_sql(INSTALLATION)},{fixture.quote_sql(PLUGIN_TYPE)},1,0,''::bytea) "
                 "ON CONFLICT (id) DO NOTHING")
            psql("INSERT INTO kiarabot.instances(id,installation,plugin_type,desired_version,"
                 "applied_config_version,fence_instance,runtime_epoch,execution_id,"
                 "execution_generation,connection_id,connection_generation,version,state,host,desired) "
                 f"VALUES ({fixture.quote_sql(instance)},{fixture.quote_sql(INSTALLATION)},"
                 f"{fixture.quote_sql(PLUGIN_TYPE)},1,0,NULL,NULL,NULL,NULL,NULL,NULL,1,0,"
                 f"{fixture.quote_sql(HOST_ID)},0)")
            if credential:
                psql("INSERT INTO kiarabot.bridge_credentials"
                     "(id,resource_id,digest,created_at,revoked,version) "
                     f"VALUES ('cred-{instance}',{fixture.quote_sql(instance)},"
                     "decode(repeat('ab',32),'hex'),1,false,1)")

        def instance_row(instance: str):
            return psql("SELECT state,version FROM kiarabot.instances WHERE id=" +
                        fixture.quote_sql(instance)).stdout.decode().strip()

        def instance_version(instance: str) -> int:
            return int(instance_row(instance).split('|')[1])

        def credential_revoked(instance: str):
            return psql("SELECT revoked FROM kiarabot.bridge_credentials WHERE resource_id=" +
                        fixture.quote_sql(instance)).stdout.decode().strip()

        def scheduler_calls():
            path = root / 'scheduler-state'
            if not path.is_file():
                return []
            return path.read_text().splitlines()

        def set_scheduler_mode(mode: str):
            (root / 'scheduler-mode').write_text(mode + '\n')

        with TestPki(Path(args.openssl).resolve()) as pki:
            server_leaf = pki.servers['runtime']
            host_leaf = pki.client
            host_der = subprocess.check_output(
                [args.openssl, 'x509', '-in', str(host_leaf.certificate), '-outform', 'der'])
            fingerprint = __import__('hashlib').sha256(host_der).hexdigest()
            probe_port, grpc_port = fixture.free_port(), fixture.free_port()
            config = {
                'schema_version': 1, 'service': 'runtime',
                'probe_listen': f'127.0.0.1:{probe_port}',
                'grpc_listen': f'127.0.0.1:{grpc_port}',
                'tls': {'certificate_file': str(server_leaf.certificate),
                        'private_key_file': str(server_leaf.key),
                        'ca_file': str(pki.ca.certificate)},
                'database': {'host': 'localhost', 'port': port, 'name': 'p03_runtime',
                             'migration_role': 'p03_runtime_migration',
                             'runtime_role': 'p03_runtime_runtime',
                             'password_file': str(root / 'password-p03_runtime_runtime'),
                             'ca_file': str(root / 'server.pem')},
                'hosts': [{'host_id': HOST_ID, 'certificate_sha256': fingerprint}],
            }
            config_path = root / 'runtime.json'
            config_path.write_text(json.dumps(config))
            config_path.chmod(0o600)
            process, error_file, state = start_runtime(args.runtime, config_path, root)
            endpoint = f'127.0.0.1:{state["grpc_port"]}'
            host_channel = grpc.aio.secure_channel(
                endpoint, grpc.ssl_channel_credentials(
                    root_certificates=pki.ca.certificate.read_bytes(),
                    private_key=host_leaf.key.read_bytes(),
                    certificate_chain=host_leaf.certificate.read_bytes()))
            host_stub = __import__('kiarabot.v1.host_pb2_grpc', fromlist=['x']).HostBridgeStub(
                host_channel)
            client_channel = grpc.aio.secure_channel(
                endpoint, grpc.ssl_channel_credentials(
                    root_certificates=pki.ca.certificate.read_bytes(),
                    private_key=host_leaf.key.read_bytes(),
                    certificate_chain=host_leaf.certificate.read_bytes()))
            runtime_stub = runtime_grpc.RuntimeServiceStub(client_channel)

            async def connect_host():
                stream = host_stub.Connect()
                await stream.write(host_pb2.HostToRuntime(
                    registration=host_pb2.HostRegisterHostRequest(
                        host=host_pb2.HostRecord(host_id=HOST_ID))))
                registered = await read(stream, 'host registration')
                require(registered.WhichOneof('body') == 'registered', 'host registration rejected')
                return stream, registered.registered.fence

            stream, fence = await connect_host()

            def request_context(idempotency: str):
                from google.protobuf.timestamp_pb2 import Timestamp
                deadline = Timestamp()
                deadline.FromNanoseconds(int((time.time() + 30) * 1_000_000_000))
                return platform.RequestContext(
                    request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()),
                    deadline=deadline, idempotency_key=idempotency)

            def resource(instance):
                return platform.ResourceRef(kind=platform.RESOURCE_INSTANCE, id=instance)

            async def begin_delete(instance, expected_version=1, idempotency=None):
                return await runtime_stub.BeginDelete(runtime_pb2.RuntimeBeginDeleteRequest(
                    context=request_context(idempotency or 'p13g-delete-' + instance),
                    resource=resource(instance), expected_version=expected_version), timeout=10)

            async def get_operation(reference):
                response = await runtime_stub.GetOperation(
                    runtime_pb2.RuntimeGetOperationRequest(
                        context=request_context('p13g-op-' + str(uuid.uuid4())),
                        operation=reference), timeout=10)
                return response.operation

            async def wait_for_operation(reference, predicate, label, timeout=20):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    operation = await get_operation(reference)
                    if predicate(operation):
                        return operation
                    await asyncio.sleep(0.2)
                raise Failure(label + ': operation predicate timed out')

            async def wait_host_blocked(reference, label):
                return await wait_for_operation(
                    reference,
                    lambda op: any(p.participant == 'host' and
                                   p.state == platform.OPERATION_BLOCKED
                                   for p in op.participants), label)

            async def send_heartbeat(stream):
                from google.protobuf.timestamp_pb2 import Timestamp
                now = Timestamp()
                now.GetCurrentTime()
                await stream.write(host_pb2.HostToRuntime(
                    fence=fence, heartbeat=host_pb2.HostHeartbeat(fence=fence, sent_at=now)))
                return await read(stream, 'heartbeat ack')

            async def receive_stop(stream):
                # Heartbeat acks and other delivery frames may interleave; scan bounded frames.
                for _ in range(8):
                    frame = await read(stream, 'stop dispatch')
                    if frame.WhichOneof('body') == 'stop':
                        return frame.stop
                raise Failure('stop frame not found in bounded stream window')

            async def send_receipt(stream, command_id: str, outcome):
                execution = host_pb2.ExecutionRecord(
                    host_id=HOST_ID, instance_id='', execution_generation=1, desired_version=1,
                    outcome=outcome, process_marker='p13g-marker', command_id=command_id)
                execution.observed_at.GetCurrentTime()
                await stream.write(host_pb2.HostToRuntime(
                    fence=fence, receipt=host_pb2.HostCommandReceipt(
                        command_id=command_id, persisted=True, execution=execution)))

            # ---- main: Host + Scheduler participant convergence to DELETED ----
            main_instance = str(uuid.uuid4())
            seed_instance(main_instance, credential=True)
            first = await begin_delete(main_instance)
            first_ref = first.operation
            duplicate = await begin_delete(
                main_instance, idempotency='p13g-delete-' + main_instance)
            require(duplicate.operation.operation_id == first_ref.operation_id,
                    'duplicate BeginDelete was not idempotent')
            blocked = await wait_for_operation(
                first_ref,
                lambda op: any(p.participant == 'host' and p.state == platform.OPERATION_BLOCKED
                               for p in op.participants), 'host participant blocked')
            require(any(p.participant == 'scheduler' and
                        p.state == platform.OPERATION_SUCCEEDED for p in blocked.participants),
                    'fake scheduler participant did not converge')
            await send_heartbeat(stream)
            stop = await receive_stop(stream)
            require(stop.instance_id == main_instance, 'stop command instance mismatch')
            await send_receipt(stream, stop.command_id, platform.EXECUTION_KNOWN_SUCCEEDED)
            completed = await wait_for_operation(
                first_ref, lambda op: op.state == platform.OPERATION_SUCCEEDED,
                'joint deletion complete')
            require(all(p.state in (platform.OPERATION_SUCCEEDED, platform.OPERATION_FAILED)
                        for p in completed.participants), 'participants not terminal')
            require(instance_row(main_instance).startswith('3|'), 'instance not DELETED')
            require(credential_revoked(main_instance) == 't', 'delete did not revoke credential')
            calls_before_restore = list(scheduler_calls())

            restored = await runtime_stub.RestoreResource(runtime_pb2.RuntimeRestoreResourceRequest(
                context=request_context('p13g-restore-' + main_instance),
                resource=resource(main_instance)), timeout=10)
            restored_operation = await wait_for_operation(
                restored.operation, lambda op: op.state == platform.OPERATION_SUCCEEDED,
                'restore complete')
            require(instance_row(main_instance).startswith('0|'), 'restore did not reactivate')
            require(credential_revoked(main_instance) == 't',
                    'restore resurrected a revoked credential')
            require(scheduler_calls() == calls_before_restore,
                    'restore re-created Scheduler tasks/leases')
            require([p.participant for p in restored_operation.participants] == ['runtime'],
                    'restore fabricated participants')

            # Re-delete after restore: participants are re-scheduled and reach DELETED again.
            reseeded = await begin_delete(
                main_instance, expected_version=instance_version(main_instance),
                idempotency='p13g-redelete-' + main_instance + '-' + str(uuid.uuid4()))
            reseeded_ref = reseeded.operation
            reseeded_operation = await get_operation(reseeded_ref)
            require(any(p.participant == 'scheduler' for p in reseeded_operation.participants),
                    're-delete did not re-schedule the Scheduler participant')
            await wait_host_blocked(reseeded_ref, 're-delete host blocked')
            await send_heartbeat(stream)
            stop2 = await receive_stop(stream)
            await send_receipt(stream, stop2.command_id, platform.EXECUTION_KNOWN_SUCCEEDED)
            await wait_for_operation(reseeded_ref,
                                     lambda op: op.state == platform.OPERATION_SUCCEEDED,
                                     're-delete complete')

            # ---- Host offline: participant stays BLOCKED, never DELETED ----
            offline_instance = str(uuid.uuid4())
            seed_instance(offline_instance)
            await stream.done_writing()
            await host_channel.close()
            offline = await begin_delete(offline_instance)
            await wait_host_blocked(offline.operation, 'offline host blocked')
            await asyncio.sleep(1.5)
            offline_op = await get_operation(offline.operation)
            require(offline_op.state == platform.OPERATION_BLOCKED and
                    any(p.participant == 'host' and p.state == platform.OPERATION_BLOCKED
                        for p in offline_op.participants),
                    'offline Host did not keep the operation BLOCKED')
            require(instance_row(offline_instance).startswith('2|'),
                    'offline Host deletion did not stay DELETING')

            # Reconnect Host and finish the offline deletion through a real receipt.
            host_channel = grpc.aio.secure_channel(
                endpoint, grpc.ssl_channel_credentials(
                    root_certificates=pki.ca.certificate.read_bytes(),
                    private_key=host_leaf.key.read_bytes(),
                    certificate_chain=host_leaf.certificate.read_bytes()))
            host_stub = __import__('kiarabot.v1.host_pb2_grpc', fromlist=['x']).HostBridgeStub(
                host_channel)
            stream, fence = await connect_host()
            # Registration itself may dispatch the pending stop before the heartbeat ack; accept
            # both frames in either order instead of assuming ack-first.
            from google.protobuf.timestamp_pb2 import Timestamp
            now = Timestamp()
            now.GetCurrentTime()
            await stream.write(host_pb2.HostToRuntime(
                fence=fence, heartbeat=host_pb2.HostHeartbeat(fence=fence, sent_at=now)))
            pending_stop = None
            for _ in range(6):
                frame = await read(stream, 'reconnected stop')
                if frame.WhichOneof('body') == 'stop':
                    pending_stop = frame.stop
                    break
            require(pending_stop is not None and pending_stop.instance_id == offline_instance,
                    'reconnected Host did not receive offline stop')
            await send_receipt(stream, pending_stop.command_id,
                               platform.EXECUTION_KNOWN_SUCCEEDED)
            await wait_for_operation(offline.operation,
                                     lambda op: op.state == platform.OPERATION_SUCCEEDED,
                                     'offline Host deletion completes after reconnect')

            # ---- UNKNOWN receipt: BLOCKED, never fake stopped ----
            unknown_instance = str(uuid.uuid4())
            seed_instance(unknown_instance)
            unknown = await begin_delete(unknown_instance)
            await wait_host_blocked(unknown.operation, 'unknown host blocked')
            await send_heartbeat(stream)
            unknown_stop = await receive_stop(stream)
            require(unknown_stop.instance_id == unknown_instance, 'unknown stop instance mismatch')
            await send_receipt(stream, unknown_stop.command_id, platform.EXECUTION_UNKNOWN)
            await asyncio.sleep(1.5)
            unknown_op = await get_operation(unknown.operation)
            require(unknown_op.state == platform.OPERATION_BLOCKED,
                    'UNKNOWN receipt faked terminal completion')
            require(instance_row(unknown_instance).startswith('2|'),
                    'UNKNOWN receipt changed instance out of DELETING')

            # ---- Scheduler unknown/offline matrix then done ----
            for mode in ('unknown', 'offline'):
                instance = str(uuid.uuid4())
                seed_instance(instance)
                set_scheduler_mode(mode)
                scheduled = await begin_delete(instance)
                await wait_host_blocked(scheduled.operation, f'scheduler {mode} host blocked')
                await send_heartbeat(stream)
                stop = await receive_stop(stream)
                await send_receipt(stream, stop.command_id, platform.EXECUTION_KNOWN_SUCCEEDED)
                await asyncio.sleep(1.5)
                op = await get_operation(scheduled.operation)
                require(op.state == platform.OPERATION_BLOCKED,
                        f'scheduler {mode} faked completion')
                set_scheduler_mode('done')
                await wait_for_operation(scheduled.operation,
                                         lambda value: value.state == platform.OPERATION_SUCCEEDED,
                                         f'scheduler {mode} recovery')

            require(credential_revoked(main_instance) == 't',
                    'restore/deletion sequence revived a credential')
            print('P13g test-only joint lifecycle e2e: PASS')
    except BaseException as error:
        error_path = root / 'runtime.err'
        if error_path.is_file():
            print('P13g runtime stderr:\n' +
                  error_path.read_text(errors='replace')[-4000:], file=sys.stderr)
        raise error
    finally:
        stop_process(process)
        if error_file:
            error_file.close()
        if channel is not None:
            await channel.close()
        fixture = load_fixture(Path(args.repo_source).resolve())
        if (root / 'data/postmaster.pid').exists():
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
    except Exception as error:  # noqa: BLE001
        traceback.print_exc()
        print(f'P13g joint e2e FAILED: {type(error).__name__}: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
