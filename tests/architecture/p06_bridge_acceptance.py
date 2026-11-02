#!/usr/bin/env python3
"""P06 external Bridge acceptance: real TLS gRPC + real Runtime process + real PostgreSQL.

The fixture initializes a disposable PG16.4 cluster, seeds an externally managed adapter binding
and plugin instance with bootstrap credentials through the compiled seed driver, starts the real
kiarabot-runtime process with both listeners, then exercises the frozen A1/B1/C surface with a
manual gRPC client: registration, assigned identity, ConnectionAuthorization, per-frame
fence+proof, adapter event acceptance, plugin delivery lease/renew/ACK, refresh rotation and
bounded shutdown. No production credential or external endpoint is accepted.
"""
from __future__ import annotations

import argparse
import asyncio
from contextlib import suppress
import json
import os
from pathlib import Path
import secrets
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

ADAPTER_INSTANCE = '33333333-3333-4333-8333-333333333333'
ACCOUNT = '11111111-1111-4111-8111-111111111111'
PLUGIN_INSTANCE = '44444444-4444-4444-8444-444444444444'
INSTALLATION = '55555555-5555-4555-8555-555555555555'
SHUTDOWN_INSTANCE = 'a1a1a1a1-a1a1-4a1a-8a1a-a1a1a1a1a1a1'
SHUTDOWN_BOOTSTRAP = 'c' * 64
OTHER_INSTANCE = '99999999-9999-4999-8999-999999999999'
OTHER_INSTALLATION = 'abababab-abab-4bab-8bab-abababababab'
UNBOUND_ACCOUNT = '22222222-2222-4222-8222-222222222222'
ADAPTER_ID = '88888888-8888-4888-8888-888888888888'
ADAPTER_BOOTSTRAP = 'a' * 64
PLUGIN_BOOTSTRAP = 'b' * 64


class AcceptanceFailure(RuntimeError):
    pass


def require(condition, label):
    if not condition:
        raise AcceptanceFailure(label)


def quote_sql(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def run(command, timeout=60, env=None):
    result = subprocess.run(command, env=env, capture_output=True, timeout=timeout)
    if result.returncode != 0:
        raise AcceptanceFailure(
            f'{Path(command[0]).name} failed ({result.returncode}): '
            f'{result.stderr.decode(errors="replace")[:500]}')
    return result


def start_postgres(pg_bin: Path, root: Path, openssl: Path, log) -> int:
    password = root / 'password'
    password.write_text(secrets.token_hex(32) + '\n')
    password.chmod(0o600)
    for service in ('runtime',):
        for role in ('migration', 'runtime'):
            role_file = root / f'password-p03_{service}_{role}'
            role_file.write_text(secrets.token_hex(32) + '\n')
            role_file.chmod(0o600)
    run([pg_bin / 'initdb', '-D', root / 'data', '-A', 'scram-sha-256', '-U', 'p03_admin',
         '--pwfile', password, '--no-locale', '--encoding=UTF8'], env={'LC_ALL': 'C'})
    run([openssl, 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
         '-nodes', '-days', '1', '-subj', '/CN=localhost', '-addext',
         'subjectAltName=DNS:localhost', '-keyout', root / 'server.key',
         '-out', root / 'server.pem'])
    (root / 'server.key').chmod(0o600)
    (root / 'data/pg_hba.conf').write_text(
        'hostssl all all 127.0.0.1/32 scram-sha-256\nhostnossl all all 127.0.0.1/32 reject\n')
    port = free_port()
    with (root / 'data/postgresql.conf').open('a') as stream:
        stream.write(f"\nlisten_addresses='127.0.0.1'\nport={port}\nssl=on\n"
                     f"ssl_cert_file='{root}/server.pem'\nssl_key_file='{root}/server.key'\n"
                     f"unix_socket_directories='{root}'\n")
    run([pg_bin / 'pg_ctl', '-D', root / 'data', '-l', root / 'pg.log', '-w', '-t',
         '20', 'start'], env={'LC_ALL': 'C'})
    return port


def psql(pg_bin: Path, root: Path, port: int, password: str, sql: str):
    env = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
    env['PGPASSWORD'] = password
    env['LC_ALL'] = 'C'
    connection = (f'host=localhost port={port} dbname=p03_runtime '
                  f'user=p03_runtime_runtime sslmode=verify-full '
                  f'sslrootcert={root / "server.pem"}')
    return run([pg_bin / 'psql', connection, '-tA', '-v', 'ON_ERROR_STOP=1', '-c', sql],
               env=env)


async def bridge_channel(port: int, ca: bytes):
    grpc = _grpc()
    return grpc.aio.secure_channel(f'127.0.0.1:{port}', grpc.ssl_channel_credentials(
        root_certificates=ca), options=[('grpc.max_receive_message_length', 1024 * 1024),
                                        ('grpc.max_send_message_length', 1024 * 1024)])


def _grpc():
    import grpc
    return grpc


def proto_modules(repo: Path):
    generated = repo / 'sdk/python/build/generated'
    require((generated / 'kiarabot/v1/services_pb2_grpc.py').is_file(),
            'generated Python protos missing (run the full build first)')
    sys.path.insert(0, str(generated))
    from kiarabot.v1 import (actions_pb2, adapters_pb2, common_pb2, events_pb2, platform_pb2,
                             plugins_pb2, services_pb2_grpc)
    return (actions_pb2, adapters_pb2, common_pb2, events_pb2, platform_pb2, plugins_pb2,
            services_pb2_grpc)


def timestamp(seconds: float):
    from google.protobuf.timestamp_pb2 import Timestamp
    value = Timestamp()
    value.FromNanoseconds(int(seconds * 1_000_000_000))
    return value


async def read_or_fail(call, label, timeout=15.0):
    try:
        response = await asyncio.wait_for(call.read(), timeout)
    except asyncio.TimeoutError as error:
        raise AcceptanceFailure(f'{label}: timed out waiting for Runtime frame') from error
    require(response is not None, f'{label}: stream closed unexpectedly')
    return response


async def expect_rpc_error(call, label, allowed):
    grpc = _grpc()
    try:
        response = await asyncio.wait_for(call.read(), 5)
        raise AcceptanceFailure(f'{label}: expected a gRPC error, got {response}')
    except grpc.RpcError as error:
        require(error.code() in allowed, f'{label}: unexpected gRPC code {error.code()}')


def fence_fields(fence):
    require(fence.instance_id and fence.runtime_epoch and fence.HasField('execution_generation')
            and fence.execution_generation > 0 and fence.HasField('connection_generation')
            and fence.connection_generation > 0, 'incomplete Runtime fence')
    return fence


async def async_main(args) -> None:
    repo = Path(args.repo_source).resolve()
    runtime = Path(args.runtime).resolve()
    seed = Path(args.seed).resolve()
    initializer = Path(args.initializer).resolve()
    openssl = Path(args.openssl).resolve()
    pg_bin = Path(args.pg_bin).resolve()
    (actions, adapters, common, events, platform, plugins, stubs) = proto_modules(repo)
    grpc = _grpc()
    sys.path.insert(0, str(repo / 'tests/architecture'))
    from fixtures import TestPki

    root = Path(tempfile.mkdtemp(prefix='kiarabot-p06-bridge-')).resolve()
    root.chmod(0o700)
    process = None
    channel = None
    try:
        port = start_postgres(pg_bin, root, openssl, None)
        admin_password = (root / 'password').read_text().strip()
        env = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
        env['PGPASSWORD'] = admin_password
        env['LC_ALL'] = 'C'
        admin = (f'host=localhost port={port} dbname=postgres user=p03_admin '
                 f'sslmode=verify-full sslrootcert={root / "server.pem"}')
        migration_password = (root / 'password-p03_runtime_migration').read_text().strip()
        runtime_password = (root / 'password-p03_runtime_runtime').read_text().strip()
        psql_admin = [pg_bin / 'psql', admin, '-tA', '-v', 'ON_ERROR_STOP=1', '-c']
        run([*psql_admin, f"CREATE ROLE p03_runtime_migration LOGIN NOINHERIT NOSUPERUSER "
                          f"NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD "
                          f"{quote_sql(migration_password)}"], env=env)
        run([*psql_admin, f"CREATE ROLE p03_runtime_runtime LOGIN NOINHERIT NOSUPERUSER "
                          f"NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD "
                          f"{quote_sql(runtime_password)}"], env=env)
        run([*psql_admin, 'CREATE DATABASE p03_runtime OWNER p03_runtime_migration'], env=env)
        run([*psql_admin, 'REVOKE ALL ON DATABASE p03_runtime FROM PUBLIC'], env=env)
        run([*psql_admin, 'GRANT CONNECT ON DATABASE p03_runtime TO p03_runtime_runtime'],
            env=env)
        run([initializer, '--initialize-empty', 'runtime', 'localhost', str(port),
             'p03_runtime', 'p03_runtime_migration', 'p03_runtime_runtime',
             str(root / 'server.pem'), str(root / 'password-p03_runtime_migration')], env=env)
        seeded = run([seed, str(root), str(port), ADAPTER_INSTANCE, ACCOUNT, PLUGIN_INSTANCE,
                      ADAPTER_BOOTSTRAP, PLUGIN_BOOTSTRAP, INSTALLATION, SHUTDOWN_INSTANCE,
                      SHUTDOWN_BOOTSTRAP], env=env)
        require(b'SEEDED' in seeded.stdout, 'seed driver did not report success')

        with TestPki(openssl) as pki:
            server_leaf = pki.servers['runtime']
            ca_bytes = pki.ca.certificate.read_bytes()
            probe_port, grpc_port, bridge_port = free_port(), free_port(), free_port()
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
            path = root / 'runtime.json'
            path.write_text(json.dumps(config))
            path.chmod(0o600)
            child_env = {key: value for key, value in os.environ.items()
                         if not key.startswith(('KIARABOT_', 'CORE_', 'ONEBOT_'))}
            error_path = root / 'runtime.err'
            error_file = error_path.open('wb')
            process = subprocess.Popen([runtime, '--config', str(path)], cwd=root,
                                       env=child_env, stdout=subprocess.PIPE,
                                       stderr=error_file, start_new_session=True)
            require(select.select([process.stdout], [], [], 10)[0],
                    'Runtime did not report startup')
            startup = process.stdout.readline().decode(errors='replace').strip()
            require('bridge_port=' in startup and 'grpc_port=' in startup,
                    f'Runtime startup line incomplete: {startup[:300]}')
            state = dict(item.split('=', 1) for item in startup.split())
            require(int(state['bridge_port']) == bridge_port, 'Runtime bridge port mismatch')
            require(f'warning' not in startup.lower(), 'Runtime startup reported an error')

            channel = await bridge_channel(bridge_port, ca_bytes)
            adapter_factory = channel.stream_stream(
                '/kiarabot.v1.AdapterBridge/Connect',
                request_serializer=adapters.AdapterToCore.SerializeToString,
                response_deserializer=adapters.CoreToAdapter.FromString)
            plugin_factory = channel.stream_stream(
                '/kiarabot.v1.PluginBridge/Connect',
                request_serializer=plugins.PluginToCore.SerializeToString,
                response_deserializer=plugins.CoreToPlugin.FromString)
            adapter_call = adapter_factory(
                metadata=(('authorization', 'Bearer ' + ADAPTER_BOOTSTRAP),))
            plugin_call = plugin_factory(
                metadata=(('authorization', 'Bearer ' + PLUGIN_BOOTSTRAP),))

            deadline = timestamp(time.time() + 5)
            request_id = str(__import__('uuid').uuid4())
            trace_id = str(__import__('uuid').uuid4())
            await plugin_call.write(plugins.PluginToCore(register=plugins.RegisterPluginRequest(
                manifest=plugins.PluginManifest(
                    id='p06.bridge.plugin', name='P06 Bridge Plugin', version='1.0.0',
                    api_version='kiarabot.plugin.v1', runtime='python-rpc',
                    entrypoint='plugin:factory', isolation='process', config_schema='{}',
                    ordered=False, capabilities=['event.message.received'],
                    permissions=['message.read', 'message.send', 'member.mute', 'member.kick',
                                  'request.approve', 'storage.namespace']),
                instance_id=PLUGIN_INSTANCE,
                installation_id=INSTALLATION,
                supported_features=['plugin.config.v1', 'action.result.v1'])))
            plugin_registered = await read_or_fail(plugin_call, 'plugin registration')
            require(plugin_registered.WhichOneof('body') == 'register_result',
                    'plugin registration returned the wrong frame')
            plugin_result = plugin_registered.register_result
            require(plugin_result.accepted and not plugin_result.HasField('error'),
                    'plugin registration rejected')
            require(plugin_result.fence.instance_id == PLUGIN_INSTANCE,
                    'plugin assigned instance mismatch')
            require(plugin_result.desired_config_version == 1,
                    'plugin desired_config_version producer missing')
            plugin_fence = fence_fields(plugin_result.fence)
            require(plugin_result.authorization.HasField('fence') and
                    len(plugin_result.authorization.proof) == 32 and
                    len(plugin_result.authorization.refresh_credential) == 32 and
                    plugin_result.authorization.refresh_generation == 1 and
                    plugin_result.authorization.credential_version > 0 and
                    plugin_result.authorization.authorization_version > 0 and
                    plugin_result.authorization.principal_id == PLUGIN_INSTANCE,
                    'plugin ConnectionAuthorization incomplete')
            plugin_proof = plugin_result.authorization.proof

            await adapter_call.write(adapters.AdapterToCore(register=adapters.RegisterAdapterRequest(
                adapter_id=ADAPTER_ID, implementation='p06-acceptance',
                implementation_version='1.0.0', protocol_version='kiarabot.v1',
                adapter_instance_id=ADAPTER_INSTANCE,
                accounts=[adapters.Account(account_id=ACCOUNT, enabled=True)],
                capabilities=[common.Capability(name='action.send_message.text.v1', version='1', supported=True),
                              common.Capability(name='action.react_message.v1', version='1', supported=True),
                              common.Capability(name='action.delete_message.v1', version='1', supported=True),
                              common.Capability(name='action.mute_member.v1', version='1', supported=True),
                              common.Capability(name='action.kick_member.v1', version='1', supported=True),
                              common.Capability(name='action.approve_request.v1', version='1', supported=True)],
                context=platform.RequestContext(request_id=request_id, trace_id=trace_id,
                                                deadline=deadline))))
            adapter_registered = await read_or_fail(adapter_call, 'adapter registration')
            require(adapter_registered.WhichOneof('body') == 'register_result',
                    'adapter registration returned the wrong frame')
            adapter_result = adapter_registered.register_result
            require(adapter_result.accepted and not adapter_result.HasField('error'),
                    'adapter registration rejected')
            # P06c assumption: assigned instance equals ADAPTER_INSTANCE_ID.
            require(adapter_result.assigned_instance_id == ADAPTER_INSTANCE,
                    'adapter assigned instance differs from ADAPTER_INSTANCE_ID')
            require(adapter_result.required_protocol_version == 'kiarabot.v1',
                    'adapter protocol negotiation mismatch')
            require(1 <= adapter_result.heartbeat_interval_seconds <= 3600,
                    'adapter heartbeat interval invalid')
            adapter_fence = fence_fields(adapter_result.fence)
            adapter_auth = adapter_result.authorization
            require(adapter_auth.HasField('fence') and
                    adapter_auth.fence == adapter_result.fence and
                    len(adapter_auth.proof) == 32 and
                    len(adapter_auth.refresh_credential) == 32 and
                    adapter_auth.refresh_generation == 1 and
                    adapter_auth.credential_version > 0 and
                    adapter_auth.authorization_version > 0 and
                    adapter_auth.principal_id == ADAPTER_INSTANCE,
                    'adapter ConnectionAuthorization incomplete')
            adapter_proof = adapter_auth.proof
            adapter_refresh = adapter_auth.refresh_credential
            adapter_refresh_generation = adapter_auth.refresh_generation

            # ---- P07 Action: plugin submit -> Runtime dispatch -> adapter result -> receipt ----
            action_id = str(__import__('uuid').uuid4())
            action_idempotency = 'p07-action-' + action_id
            action_request_id = str(__import__('uuid').uuid4())

            def action_envelope(identifier, idempotency, conversation='group-1',
                                deadline_offset=30, payload='send_message',
                                conversation_type=common.ConversationRef.GROUP,
                                account_id=ACCOUNT):
                conversation_ref = common.ConversationRef(
                    id=conversation, type=conversation_type, account_id=account_id)
                envelope = actions.ActionEnvelope(
                    action_id=identifier, idempotency_key=idempotency,
                    deadline=timestamp(time.time() + deadline_offset), trace_id=identifier,
                    target_source=common.SourceRef(
                        adapter_id=ADAPTER_ID, adapter_instance_id=ADAPTER_INSTANCE,
                        account_id=account_id),
                    target=common.Subject(conversation_id=conversation))
                if payload == 'send_message':
                    envelope.send_message.CopyFrom(actions.SendMessage(
                        conversation=conversation_ref,
                        message=common.Message(plain_text='hello from p07')))
                elif payload == 'react_message':
                    envelope.react_message.CopyFrom(actions.ReactMessage(
                        message_id='p07-message', reaction='like',
                        conversation=conversation_ref))
                elif payload == 'delete_message':
                    envelope.delete_message.CopyFrom(actions.DeleteMessage(
                        message_id='p07-message', conversation=conversation_ref))
                elif payload == 'mute_member':
                    envelope.mute_member.CopyFrom(actions.MuteMember(
                        group_id=conversation, user_id='p07-member', duration_seconds=60))
                elif payload == 'kick_member':
                    envelope.kick_member.CopyFrom(actions.KickMember(
                        group_id=conversation, user_id='p07-member', reason='p07-policy'))
                elif payload == 'approve_request':
                    envelope.approve_request.CopyFrom(actions.ApproveRequest(
                        request_id='p07-request', approve=True, reason='p07-policy'))
                else:
                    raise AssertionError('unknown action payload ' + payload)
                return envelope

            first_action_envelope = action_envelope(action_id, action_idempotency)
            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                action=plugins.PluginActionRequest(
                    action=first_action_envelope,
                    request_id=action_request_id)))
            submitted = await read_or_fail(plugin_call, 'plugin action submit')
            require(submitted.WhichOneof('body') == 'action_response' and
                    submitted.action_response.HasField('result'),
                    'plugin action submission failed')
            require(submitted.action_response.result.action_id == action_id and
                    submitted.action_response.result.status == actions.ACCEPTED and
                    submitted.action_response.result.execution_outcome ==
                    platform.EXECUTION_NOT_STARTED,
                    'plugin action submission snapshot mismatch')

            dispatched = await read_or_fail(adapter_call, 'adapter action dispatch', timeout=10)
            require(dispatched.WhichOneof('body') == 'action' and
                    dispatched.action.action_id == action_id,
                    'adapter did not receive the submitted Action')
            require(dispatched.action.dispatch_attempt_id and
                    dispatched.action.dispatch_sequence >= 1,
                    'Runtime did not fill dispatch attempt/sequence')
            dispatch_attempt = dispatched.action.dispatch_attempt_id

            terminal = actions.ActionResult(
                action_id=action_id, status=actions.SUCCEEDED,
                platform_message_id='platform-message-1',
                execution_outcome=platform.EXECUTION_KNOWN_SUCCEEDED,
                dispatch_attempt_id=dispatch_attempt,
                completed_at=timestamp(time.time()))
            await adapter_call.write(adapters.AdapterToCore(
                fence=adapter_fence, connection_proof=adapter_proof, action_result=terminal))
            receipt = await read_or_fail(adapter_call, 'adapter action receipt')
            require(receipt.WhichOneof('body') == 'action_receipt' and
                    receipt.action_receipt.action_id == action_id and
                    receipt.action_receipt.dispatch_attempt_id == dispatch_attempt and
                    receipt.action_receipt.disposition == actions.ACTION_RECEIPT_RECORDED and
                    receipt.action_receipt.result.status == actions.SUCCEEDED and
                    receipt.action_receipt.result.platform_message_id == 'platform-message-1',
                    'adapter terminal result receipt mismatch')

            await adapter_call.write(adapters.AdapterToCore(
                fence=adapter_fence, connection_proof=adapter_proof, action_result=terminal))
            duplicate_receipt = await read_or_fail(adapter_call, 'adapter duplicate result receipt')
            require(duplicate_receipt.action_receipt.disposition ==
                    actions.ACTION_RECEIPT_DUPLICATE,
                    'duplicate terminal result was not DUPLICATE')

            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                action_query=plugins.PluginActionQuery(
                    request_id=str(__import__('uuid').uuid4()),
                    trace_id=str(__import__('uuid').uuid4()),
                    deadline=timestamp(time.time() + 5), action_id=action_id)))
            queried = await read_or_fail(plugin_call, 'plugin action query')
            require(queried.WhichOneof('body') == 'action_response' and
                    queried.action_response.HasField('result') and
                    queried.action_response.result.status == actions.SUCCEEDED and
                    queried.action_response.result.dispatch_attempt_id == dispatch_attempt,
                    'plugin action query did not observe the terminal result')

            # Evidence-only report: a second Action dispatched with UNKNOWN evidence stays
            # non-terminal and never overwrites a terminal row.
            evidence_action_id = str(__import__('uuid').uuid4())
            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                action=plugins.PluginActionRequest(
                    action=action_envelope(evidence_action_id,
                                           'p07-evidence-' + evidence_action_id),
                    request_id=str(__import__('uuid').uuid4()))))
            evidence_submitted = await read_or_fail(plugin_call, 'second plugin action submit')
            require(evidence_submitted.action_response.HasField('result') and
                    evidence_submitted.action_response.result.action_id == evidence_action_id,
                    'second plugin action submission failed')
            evidence_dispatch = await read_or_fail(adapter_call, 'second adapter dispatch',
                                                   timeout=10)
            require(evidence_dispatch.WhichOneof('body') == 'action' and
                    evidence_dispatch.action.action_id == evidence_action_id,
                    'second action was not dispatched')
            await adapter_call.write(adapters.AdapterToCore(
                fence=adapter_fence, connection_proof=adapter_proof,
                action_evidence=actions.ActionExecutionEvidence(
                    action_id=evidence_action_id,
                    dispatch_attempt_id=evidence_dispatch.action.dispatch_attempt_id,
                    execution_outcome=platform.EXECUTION_UNKNOWN,
                    observed_at=timestamp(time.time()))))
            evidence_receipt = await read_or_fail(adapter_call, 'adapter evidence receipt')
            require(evidence_receipt.WhichOneof('body') == 'action_receipt' and
                    evidence_receipt.action_receipt.disposition ==
                    actions.ACTION_RECEIPT_EVIDENCE_ONLY and
                    evidence_receipt.action_receipt.result.status == actions.ACCEPTED and
                    evidence_receipt.action_receipt.result.execution_outcome ==
                    platform.EXECUTION_UNKNOWN,
                    'evidence-only receipt mismatch')
            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                action_query=plugins.PluginActionQuery(
                    request_id=str(__import__('uuid').uuid4()),
                    trace_id=str(__import__('uuid').uuid4()),
                    deadline=timestamp(time.time() + 5), action_id=evidence_action_id)))
            evidence_query = await read_or_fail(plugin_call, 'evidence action query')
            require(evidence_query.action_response.HasField('result') and
                    evidence_query.action_response.result.status == actions.ACCEPTED and
                    evidence_query.action_response.result.execution_outcome ==
                    platform.EXECUTION_UNKNOWN,
                    'evidence did not preserve the non-terminal snapshot')

            async def submit_action(identifier, idempotency, conversation='group-1',
                                    deadline_offset=30, payload='send_message',
                                    conversation_type=common.ConversationRef.GROUP,
                                    account_id=ACCOUNT):
                await plugin_call.write(plugins.PluginToCore(
                    fence=plugin_fence, connection_proof=plugin_proof,
                    action=plugins.PluginActionRequest(
                        action=action_envelope(identifier, idempotency, conversation,
                                               deadline_offset, payload=payload,
                                               conversation_type=conversation_type,
                                               account_id=account_id),
                        request_id=str(__import__('uuid').uuid4()))))
                response = await read_or_fail(plugin_call, 'action submit ' + identifier)
                require(response.WhichOneof('body') == 'action_response',
                        'action response body mismatch')
                return response.action_response

            async def read_dispatch(label):
                frame = await read_or_fail(adapter_call, label, timeout=10)
                require(frame.WhichOneof('body') == 'action' and
                        frame.action.dispatch_attempt_id,
                        label + ': no dispatched Action with attempt id')
                return frame.action

            # ---- result vs evidence race on the same dispatched attempt -----------------
            race_id = str(__import__('uuid').uuid4())
            await submit_action(race_id, 'p07-race-' + race_id)
            race_dispatch = await read_dispatch('race dispatch')
            race_evidence = actions.ActionExecutionEvidence(
                action_id=race_id,
                dispatch_attempt_id=race_dispatch.dispatch_attempt_id,
                execution_outcome=platform.EXECUTION_UNKNOWN,
                observed_at=timestamp(time.time()))
            race_result = actions.ActionResult(
                action_id=race_id, status=actions.SUCCEEDED,
                execution_outcome=platform.EXECUTION_KNOWN_SUCCEEDED,
                dispatch_attempt_id=race_dispatch.dispatch_attempt_id,
                completed_at=timestamp(time.time()))
            await adapter_call.write(adapters.AdapterToCore(
                fence=adapter_fence, connection_proof=adapter_proof,
                action_evidence=race_evidence))
            await adapter_call.write(adapters.AdapterToCore(
                fence=adapter_fence, connection_proof=adapter_proof,
                action_result=race_result))
            race_first = await read_or_fail(adapter_call, 'race first receipt')
            race_second = await read_or_fail(adapter_call, 'race second receipt')
            require(race_first.action_receipt.disposition ==
                    actions.ACTION_RECEIPT_EVIDENCE_ONLY and
                    race_second.action_receipt.disposition ==
                    actions.ACTION_RECEIPT_RECORDED and
                    race_second.action_receipt.result.status == actions.SUCCEEDED,
                    'evidence/result race did not preserve wire order')

            # ---- cancel vs already-dispatched Action ------------------------------------
            cancel_id = str(__import__('uuid').uuid4())
            await submit_action(cancel_id, 'p07-cancel-' + cancel_id,
                                conversation='group-cancel')
            await read_dispatch('cancel dispatch')
            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                action_cancel=plugins.PluginActionCancel(
                    request_id=str(__import__('uuid').uuid4()),
                    trace_id=str(__import__('uuid').uuid4()),
                    deadline=timestamp(time.time() + 5), action_id=cancel_id)))
            cancel_response = await read_or_fail(plugin_call, 'cancel dispatched response')
            require(cancel_response.WhichOneof('body') == 'action_response' and
                    cancel_response.action_response.HasField('error') and
                    cancel_response.action_response.error.code == common.CONFLICT,
                    'cancel of dispatched Action was not CONFLICT')

            # ---- duplicate submission is replayed without a second dispatch -------------
            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                action=plugins.PluginActionRequest(
                    action=first_action_envelope,
                    request_id=str(__import__('uuid').uuid4()))))
            duplicate_submit = await read_or_fail(plugin_call, 'duplicate action submit')
            require(duplicate_submit.action_response.HasField('result') and
                    duplicate_submit.action_response.result.status == actions.SUCCEEDED and
                    duplicate_submit.action_response.result.action_id == action_id,
                    'duplicate submission did not replay the original snapshot')
            # No second dispatch frame: the next expected adapter frames (quota Action frames)
            # are asserted by action id below, so a duplicate dispatch would surface there.
            await asyncio.sleep(1.2)

            # ---- persistent per-account/group quota window (3/group/minute) ------------
            quota_conversation = 'group-quota'
            quota_identifiers = [str(__import__('uuid').uuid4()) for _ in range(4)]
            for identifier in quota_identifiers:
                await plugin_call.write(plugins.PluginToCore(
                    fence=plugin_fence, connection_proof=plugin_proof,
                    action=plugins.PluginActionRequest(
                        action=action_envelope(identifier, 'p07-quota-' + identifier,
                                               quota_conversation),
                        request_id=str(__import__('uuid').uuid4()))))
            quota_responses = [
                await read_or_fail(plugin_call, 'quota submit') for _ in range(4)]
            accepted_quota = sum(1 for response in quota_responses
                                 if response.action_response.HasField('result'))
            rejected_quota = sum(
                1 for response in quota_responses
                if response.action_response.HasField('error') and
                response.action_response.error.code == common.RATE_LIMITED)
            require(accepted_quota == 3 and rejected_quota == 1,
                    f'quota window mismatch accepted={accepted_quota} rejected={rejected_quota}')
            dispatched_quota = [await read_dispatch('quota dispatch') for _ in range(3)]
            require(sorted(frame.action_id for frame in dispatched_quota) ==
                    sorted(quota_identifiers[:3]),
                    'quota dispatch ids do not match the three admitted Actions')

            # ---- adapter disconnect/reconnect resends the old attempt's result ----------
            replay_id = str(__import__('uuid').uuid4())
            await submit_action(replay_id, 'p07-replay-' + replay_id,
                                conversation='group-replay')
            replay_dispatch = await read_dispatch('reconnect replay dispatch')
            adapter_call.cancel()
            with suppress(asyncio.CancelledError, Exception):
                await adapter_call
            adapter_call = adapter_factory(
                metadata=(('authorization', 'Bearer ' + ADAPTER_BOOTSTRAP),))
            await adapter_call.write(adapters.AdapterToCore(register=adapters.RegisterAdapterRequest(
                adapter_id=ADAPTER_ID, implementation='p06-acceptance',
                implementation_version='1.0.0', protocol_version='kiarabot.v1',
                adapter_instance_id=ADAPTER_INSTANCE,
                accounts=[adapters.Account(account_id=ACCOUNT, enabled=True)],
                capabilities=[common.Capability(name='action.send_message.text.v1', version='1', supported=True),
                              common.Capability(name='action.react_message.v1', version='1', supported=True),
                              common.Capability(name='action.delete_message.v1', version='1', supported=True),
                              common.Capability(name='action.mute_member.v1', version='1', supported=True),
                              common.Capability(name='action.kick_member.v1', version='1', supported=True),
                              common.Capability(name='action.approve_request.v1', version='1', supported=True)],
                context=platform.RequestContext(
                    request_id=str(__import__('uuid').uuid4()),
                    trace_id=str(__import__('uuid').uuid4()),
                    deadline=timestamp(time.time() + 5)))))
            reconnect_registration = await read_or_fail(adapter_call,
                                                        'reconnect registration')
            require(reconnect_registration.WhichOneof('body') == 'register_result' and
                    reconnect_registration.register_result.accepted,
                    'adapter reconnect registration rejected')
            adapter_fence = fence_fields(reconnect_registration.register_result.fence)
            adapter_auth = reconnect_registration.register_result.authorization
            adapter_proof = adapter_auth.proof
            adapter_refresh = adapter_auth.refresh_credential
            adapter_refresh_generation = adapter_auth.refresh_generation
            replay_result = actions.ActionResult(
                action_id=replay_id, status=actions.SUCCEEDED,
                execution_outcome=platform.EXECUTION_KNOWN_SUCCEEDED,
                dispatch_attempt_id=replay_dispatch.dispatch_attempt_id,
                completed_at=timestamp(time.time()))
            await adapter_call.write(adapters.AdapterToCore(
                fence=adapter_fence, connection_proof=adapter_proof,
                action_result=replay_result))
            replay_receipt = await read_or_fail(adapter_call, 'reconnect replay receipt')
            require(replay_receipt.action_receipt.disposition ==
                    actions.ACTION_RECEIPT_RECORDED,
                    'reconnected adapter could not report the old dispatch attempt')
            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                action_query=plugins.PluginActionQuery(
                    request_id=str(__import__('uuid').uuid4()),
                    trace_id=str(__import__('uuid').uuid4()),
                    deadline=timestamp(time.time() + 5), action_id=replay_id)))
            replay_query = await read_or_fail(plugin_call, 'reconnect replay query')
            require(replay_query.action_response.result.status == actions.SUCCEEDED,
                    'replayed result was not visible after reconnect')

            # ---- EXPIRED late result is evidence-only and never rewrites the terminal row
            expired_id = str(__import__('uuid').uuid4())
            await submit_action(expired_id, 'p07-expired-' + expired_id,
                                conversation='group-expired', deadline_offset=2.5)
            expired_dispatch = await read_dispatch('expired dispatch')
            await asyncio.sleep(4.5)
            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                action_query=plugins.PluginActionQuery(
                    request_id=str(__import__('uuid').uuid4()),
                    trace_id=str(__import__('uuid').uuid4()),
                    deadline=timestamp(time.time() + 5), action_id=expired_id)))
            expired_query = await read_or_fail(plugin_call, 'expired query')
            require(expired_query.action_response.result.status == actions.EXPIRED and
                    expired_query.action_response.result.execution_outcome ==
                    platform.EXECUTION_UNKNOWN,
                    'dispatched Action did not expire as UNKNOWN: result={} status={} outcome={} error={}'.format(
                        expired_query.action_response.HasField('result'),
                        expired_query.action_response.result.status,
                        expired_query.action_response.result.execution_outcome,
                        expired_query.action_response.error if
                        expired_query.action_response.HasField('error') else None))
            late_result = actions.ActionResult(
                action_id=expired_id, status=actions.SUCCEEDED,
                execution_outcome=platform.EXECUTION_KNOWN_SUCCEEDED,
                dispatch_attempt_id=expired_dispatch.dispatch_attempt_id,
                completed_at=timestamp(time.time()))
            await adapter_call.write(adapters.AdapterToCore(
                fence=adapter_fence, connection_proof=adapter_proof,
                action_result=late_result))
            late_receipt = await read_or_fail(adapter_call, 'late expired result receipt')
            require(late_receipt.action_receipt.disposition ==
                    actions.ACTION_RECEIPT_EVIDENCE_ONLY and
                    late_receipt.action_receipt.result.status == actions.EXPIRED,
                    'late result did not become evidence-only')
            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                action_query=plugins.PluginActionQuery(
                    request_id=str(__import__('uuid').uuid4()),
                    trace_id=str(__import__('uuid').uuid4()),
                    deadline=timestamp(time.time() + 5), action_id=expired_id)))
            late_query = await read_or_fail(plugin_call, 'late result query')
            require(late_query.action_response.result.status == actions.EXPIRED and
                    late_query.action_response.result.execution_outcome ==
                    platform.EXECUTION_UNKNOWN,
                    'late result rewrote the expired terminal snapshot')

            # ---- P07f wire Action policy gate for mute/kick -------------------------------
            def seed_action_policy(*, mute=True, kick=True, groups=('group-1',),
                                   mute_budget=10, kick_budget=10, updated_offset=-2,
                                   account=ACCOUNT, version=1):
                allowed = 'ARRAY[' + ','.join(quote_sql(group) for group in groups) + ']'
                updated = int((time.time() + updated_offset) * 1_000_000_000)
                psql(pg_bin, root, port, runtime_password,
                     "INSERT INTO kiarabot.action_policies(account_id,mute_enabled,kick_enabled,"
                     "allowed_group_ids,mute_daily_budget,kick_daily_budget,version,updated_at) "
                     f"VALUES ({quote_sql(account)},{str(mute).lower()},{str(kick).lower()},"
                     f"{allowed},{mute_budget},{kick_budget},{version},{updated}) "
                     "ON CONFLICT (account_id) DO UPDATE SET mute_enabled=EXCLUDED.mute_enabled,"
                     "kick_enabled=EXCLUDED.kick_enabled,"
                     "allowed_group_ids=EXCLUDED.allowed_group_ids,"
                     "mute_daily_budget=EXCLUDED.mute_daily_budget,"
                     "kick_daily_budget=EXCLUDED.kick_daily_budget,"
                     "version=EXCLUDED.version,updated_at=EXCLUDED.updated_at")

            def policy_count(sql):
                return int(psql(pg_bin, root, port, runtime_password, sql).stdout.strip())

            def action_counts(action_id):
                quoted = quote_sql(action_id)
                return (policy_count(
                            f"SELECT count(*) FROM kiarabot.actions WHERE action_id={quoted}"),
                        policy_count(
                            f"SELECT count(*) FROM kiarabot.quota_windows "
                            f"WHERE action_id={quoted}"))

            async def expect_rejected(action_id, idempotency, expected_code, label,
                                      payload='mute_member', conversation='group-1',
                                      conversation_type=common.ConversationRef.GROUP,
                                      account_id=ACCOUNT):
                response = await submit_action(
                    action_id, idempotency, conversation=conversation, payload=payload,
                    conversation_type=conversation_type, account_id=account_id)
                require(response.HasField('error') and response.error.code == expected_code,
                        f'{label}: expected {expected_code}, got {response}')
                # A rejected Action is denied before the insert/outbox step. Reading the shared
                # adapter stream with a timeout would cancel the gRPC read and poison the stream,
                # so absence of a dispatch is proven by (a) no persisted Action row/quota and
                # (b) the next allowed dispatch being exactly the next action_id below.
                actions_rows, quota_rows = action_counts(action_id)
                require(actions_rows == 0 and quota_rows == 0,
                        f'{label}: rejected Action persisted or consumed quota')

            seed_action_policy()
            baseline_mute = policy_count(
                f"SELECT COALESCE(SUM(used),0) FROM kiarabot.daily_budgets "
                f"WHERE kind=4 AND account_id={quote_sql(ACCOUNT)}")
            baseline_kick = policy_count(
                f"SELECT COALESCE(SUM(used),0) FROM kiarabot.daily_budgets "
                f"WHERE kind=5 AND account_id={quote_sql(ACCOUNT)}")

            def rejected_id(label):
                return str(__import__('uuid').uuid4()), 'p07-policy-' + label + '-' + str(
                    __import__('uuid').uuid4())

            action_id, idempotency = rejected_id('disabled')
            seed_action_policy(mute=False)
            await expect_rejected(action_id, idempotency, common.PERMISSION_DENIED,
                                  'mute disabled')
            action_id, idempotency = rejected_id('whitelist')
            seed_action_policy(groups=('other-group',))
            await expect_rejected(action_id, idempotency, common.PERMISSION_DENIED,
                                  'mute group outside allowlist')
            action_id, idempotency = rejected_id('budget')
            seed_action_policy(mute_budget=0)
            await expect_rejected(action_id, idempotency, common.PERMISSION_DENIED,
                                  'mute zero daily budget')
            action_id, idempotency = rejected_id('future')
            seed_action_policy(updated_offset=3600)
            await expect_rejected(action_id, idempotency, common.PERMISSION_DENIED,
                                  'mute future policy updated_at')
            action_id, idempotency = rejected_id('account')
            seed_action_policy()
            await expect_rejected(action_id, idempotency, common.PERMISSION_DENIED,
                                  'mute account mismatch', account_id=UNBOUND_ACCOUNT)
            # Wire difference from the domain rule: decode_plugin_action normalizes mute/kick to
            # conversation_kind=1 (group), so a private-looking group id reaches the policy and is
            # denied by the allowlist (PERMISSION_DENIED), not by the domain non-group
            # INVALID_ARGUMENT branch. An empty conversation id exercises the pre-policy
            # INVALID_ARGUMENT admission path that is reachable from the wire.
            action_id, idempotency = rejected_id('foreign-group')
            await expect_rejected(action_id, idempotency, common.PERMISSION_DENIED,
                                  'mute non-allowlisted conversation',
                                  conversation='private-1',
                                  conversation_type=common.ConversationRef.PRIVATE)
            action_id, idempotency = rejected_id('empty-group')
            await expect_rejected(action_id, idempotency, common.INVALID_ARGUMENT,
                                  'mute empty conversation', conversation='')
            require(policy_count(
                        f"SELECT COALESCE(SUM(used),0) FROM kiarabot.daily_budgets "
                        f"WHERE kind=4 AND account_id={quote_sql(ACCOUNT)}") == baseline_mute,
                    'rejected mute consumed the daily budget')

            async def allowed_policy_action(payload, expected_kind):
                seed_action_policy()
                identifier = str(__import__('uuid').uuid4())
                response = await submit_action(identifier, 'p07-policy-ok-' + identifier,
                                               payload=payload)
                require(not response.HasField('error') and
                        response.result.status == actions.ACCEPTED and
                        response.result.execution_outcome == platform.EXECUTION_NOT_STARTED,
                        f'{payload}: allowed Action was not accepted: {response}')
                dispatch = await read_dispatch(payload + ' policy dispatch')
                require(dispatch.action_id == identifier and dispatch.HasField(payload),
                        f'{payload}: dispatch payload mismatch')
                result = actions.ActionResult(
                    action_id=identifier, status=actions.SUCCEEDED,
                    execution_outcome=platform.EXECUTION_KNOWN_SUCCEEDED,
                    dispatch_attempt_id=dispatch.dispatch_attempt_id,
                    completed_at=timestamp(time.time()))
                await adapter_call.write(adapters.AdapterToCore(
                    fence=adapter_fence, connection_proof=adapter_proof,
                    action_result=result))
                receipt = await read_or_fail(adapter_call, payload + ' policy receipt')
                require(receipt.action_receipt.disposition == actions.ACTION_RECEIPT_RECORDED and
                        receipt.action_receipt.result.status == actions.SUCCEEDED,
                        f'{payload}: receipt mismatch')
                state = policy_count(
                    f"SELECT state FROM kiarabot.actions WHERE action_id="
                    f"{quote_sql(identifier)}")
                require(state == 1, f'{payload}: terminal state not persisted')
                quota = policy_count(
                    f"SELECT count(*) FROM kiarabot.quota_windows WHERE action_id="
                    f"{quote_sql(identifier)} AND kind={expected_kind}")
                require(quota > 0, f'{payload}: accepted Action did not consume quota')
                return identifier

            await allowed_policy_action('mute_member', 4)
            await allowed_policy_action('kick_member', 5)
            require(policy_count(
                        f"SELECT COALESCE(SUM(used),0) FROM kiarabot.daily_budgets "
                        f"WHERE kind=4 AND account_id={quote_sql(ACCOUNT)}") == baseline_mute + 1,
                    'allowed mute did not consume one daily budget unit')
            require(policy_count(
                        f"SELECT COALESCE(SUM(used),0) FROM kiarabot.daily_budgets "
                        f"WHERE kind=5 AND account_id={quote_sql(ACCOUNT)}") == baseline_kick + 1,
                    'allowed kick did not consume one daily budget unit')

            # ---- P09b storage: PluginBridge host_request frames -> RuntimeStorageApplication ----
            async def storage_frame(label, *, idempotency_key='', **operation):
                await plugin_call.write(plugins.PluginToCore(
                    fence=plugin_fence, connection_proof=plugin_proof,
                    host_request=plugins.HostRequest(
                        request_id=str(__import__('uuid').uuid4()),
                        trace_id=str(__import__('uuid').uuid4()),
                        deadline=timestamp(time.time() + 5),
                        idempotency_key=idempotency_key, **operation)))
                frame = await read_or_fail(plugin_call, label)
                require(frame.WhichOneof('body') == 'host_response',
                        f'{label}: wrong response frame')
                require(frame.host_response.request_id, f'{label}: request id missing')
                return frame.host_response

            def require_storage_error(response, label, code):
                require(response.HasField('error') and response.error.code == code,
                        f'{label}: unexpected error {response.error}')

            private_scope = common.NamespaceScope(instance_private=True)
            shared_scope = common.NamespaceScope(installation_shared=True)
            set_idem = 'p09-private-set-' + str(__import__('uuid').uuid4())
            stored = await storage_frame(
                'storage private set',
                idempotency_key=set_idem,
                storage_set=plugins.StorageSetRequest(
                    key='p09/alpha', value=b'one', scope=private_scope))
            require(not stored.HasField('error') and stored.storage_set.version == 1,
                    'private storage set failed')
            fetched = await storage_frame(
                'storage private get',
                storage_get=plugins.StorageGetRequest(key='p09/alpha', scope=private_scope))
            require(not fetched.HasField('error') and fetched.storage_get.value == b'one' and
                    fetched.storage_get.version == 1, 'private storage get mismatch')
            replayed = await storage_frame(
                'storage private replay',
                idempotency_key=set_idem,
                storage_set=plugins.StorageSetRequest(
                    key='p09/alpha', value=b'one', scope=private_scope))
            require(not replayed.HasField('error') and
                    replayed.storage_set.version == stored.storage_set.version,
                    'private receipt replay changed the version')
            require_storage_error(await storage_frame(
                'storage private conflicting replay',
                idempotency_key=set_idem,
                storage_set=plugins.StorageSetRequest(
                    key='p09/alpha', value=b'changed', scope=private_scope)),
                'private conflicting replay', common.CONFLICT)
            cas = await storage_frame(
                'storage private cas',
                idempotency_key='p09-private-cas-' + str(__import__('uuid').uuid4()),
                storage_compare_and_set=plugins.StorageCompareAndSetRequest(
                    key='p09/alpha', value=b'two', scope=private_scope,
                    condition=common.StorageCondition(version=1)))
            require(not cas.HasField('error') and
                    cas.storage_compare_and_set.entry.value == b'two' and
                    cas.storage_compare_and_set.entry.version == 2, 'private CAS mismatch')
            require_storage_error(await storage_frame(
                'storage private stale cas',
                idempotency_key='p09-private-cas-stale-' + str(__import__('uuid').uuid4()),
                storage_compare_and_set=plugins.StorageCompareAndSetRequest(
                    key='p09/alpha', value=b'three', scope=private_scope,
                    condition=common.StorageCondition(version=1))),
                'private stale CAS', common.CONFLICT)
            removed = await storage_frame(
                'storage private delete',
                idempotency_key='p09-private-del-' + str(__import__('uuid').uuid4()),
                storage_delete=plugins.StorageDeleteRequest(
                    key='p09/alpha', scope=private_scope,
                    condition=common.StorageCondition(version=2)))
            require(not removed.HasField('error') and removed.storage_delete.deleted and
                    removed.storage_delete.version == 2, 'private delete mismatch')
            require_storage_error(await storage_frame(
                'storage private get after delete',
                storage_get=plugins.StorageGetRequest(key='p09/alpha', scope=private_scope)),
                'private get after delete', common.NOT_FOUND)
            ttl_deadline = timestamp(time.time() + 1)
            ttl_set = await storage_frame(
                'storage private ttl set',
                idempotency_key='p09-private-ttl-' + str(__import__('uuid').uuid4()),
                storage_set=plugins.StorageSetRequest(
                    key='p09/ttl', value=b'ttl', scope=private_scope, expires_at=ttl_deadline))
            require(not ttl_set.HasField('error') and ttl_set.storage_set.HasField('expires_at'),
                    'private TTL set mismatch')
            await asyncio.sleep(1.2)
            require_storage_error(await storage_frame(
                'storage private expired get',
                storage_get=plugins.StorageGetRequest(key='p09/ttl', scope=private_scope)),
                'private expired get', common.NOT_FOUND)
            replay_deadline = timestamp(time.time() + 1)
            replay_idem = 'p09-private-ttl-replay-' + str(__import__('uuid').uuid4())
            replay_set = await storage_frame(
                'storage private ttl replay set',
                idempotency_key=replay_idem,
                storage_set=plugins.StorageSetRequest(
                    key='p09/ttl-replay', value=b'ttl', scope=private_scope,
                    expires_at=replay_deadline))
            require(not replay_set.HasField('error'), 'private ttl replay set failed')
            await asyncio.sleep(1.2)
            replayed_ttl = await storage_frame(
                'storage private ttl replay',
                idempotency_key=replay_idem,
                storage_set=plugins.StorageSetRequest(
                    key='p09/ttl-replay', value=b'ttl', scope=private_scope,
                    expires_at=replay_deadline))
            require(not replayed_ttl.HasField('error') and
                    replayed_ttl.storage_set.version == replay_set.storage_set.version,
                    'ttl receipt replay did not return the original snapshot')
            require_storage_error(await storage_frame(
                'storage private ttl replay get',
                storage_get=plugins.StorageGetRequest(key='p09/ttl-replay', scope=private_scope)),
                'ttl replay did not extend TTL', common.NOT_FOUND)
            psql(pg_bin, root, port, runtime_password,
                 "INSERT INTO kiarabot.namespace_quota_counters(scope_kind,scope_id,key_count,"
                 "byte_count,updated_at) VALUES (0," + quote_sql(PLUGIN_INSTANCE) +
                 ",1024,0,1),(1," + quote_sql(INSTALLATION) + ",1024,0,1) "
                 "ON CONFLICT (scope_kind,scope_id) DO UPDATE SET key_count=1024,"
                 "byte_count=0,updated_at=1")
            require_storage_error(await storage_frame(
                'storage private quota',
                idempotency_key='p09-private-quota-' + str(__import__('uuid').uuid4()),
                storage_set=plugins.StorageSetRequest(
                    key='p09/quota', value=b'over', scope=private_scope)),
                'private quota rejection', common.RATE_LIMITED)
            psql(pg_bin, root, port, runtime_password,
                 "UPDATE kiarabot.namespace_quota_counters SET key_count=0,byte_count=0 "
                 "WHERE scope_kind IN (0,1)")
            shared_namespace = str(__import__('uuid').uuid4())
            psql(pg_bin, root, port, runtime_password,
                 "DELETE FROM kiarabot.namespaces WHERE installation_id=" +
                 quote_sql(INSTALLATION) + " AND kind=1")
            psql(pg_bin, root, port, runtime_password,
                 "INSERT INTO kiarabot.namespaces(namespace_id,installation_id,kind,"
                 "owner_instance,disabled,version,created_at,updated_at) VALUES (" +
                 quote_sql(shared_namespace) + "," + quote_sql(INSTALLATION) +
                 ",1,NULL,false,2,1,1);"
                 "INSERT INTO kiarabot.namespace_members(namespace_id,instance_id,can_read,"
                 "can_write,version,joined_at) VALUES (" + quote_sql(shared_namespace) + "," +
                 quote_sql(PLUGIN_INSTANCE) + ",true,true,2,1);"
                 "INSERT INTO kiarabot.namespace_values(namespace_id,key,value,version,"
                 "expires_at,updated_at) VALUES (" + quote_sql(shared_namespace) +
                 ",'shared/seed','seed',1,NULL,1)")
            shared_get = await storage_frame(
                'storage shared get',
                storage_get=plugins.StorageGetRequest(key='shared/seed', scope=shared_scope))
            require(not shared_get.HasField('error') and
                    shared_get.storage_get.value == b'seed',
                    'shared storage get mismatch')
            require_storage_error(await storage_frame(
                'storage shared set without condition',
                idempotency_key='p09-shared-nocond-' + str(__import__('uuid').uuid4()),
                storage_set=plugins.StorageSetRequest(
                    key='shared/key', value=b'shared', scope=shared_scope)),
                'shared set requires condition', common.INVALID_ARGUMENT)
            shared_set = await storage_frame(
                'storage shared set',
                idempotency_key='p09-shared-set-' + str(__import__('uuid').uuid4()),
                storage_set=plugins.StorageSetRequest(
                    key='shared/key', value=b'shared', scope=shared_scope,
                    condition=common.StorageCondition(absent=True)))
            require(not shared_set.HasField('error') and shared_set.storage_set.version == 1,
                    'shared storage set mismatch')
            # P13e: deleting one member/namespace must not delete another member's or the
            # owner's private data. Seeds a second member on the shared namespace, a second
            # installation's shared namespace/value, and a private value; then removes the
            # current plugin's membership row and proves access is denied while all rows remain.
            other_namespace = str(__import__('uuid').uuid4())
            private_keep = await storage_frame(
                'storage private keep set',
                idempotency_key='p09-private-keep-' + str(__import__('uuid').uuid4()),
                storage_set=plugins.StorageSetRequest(
                    key='p09/keep', value=b'private-keep', scope=private_scope))
            require(not private_keep.HasField('error'), 'private keep set failed')
            psql(pg_bin, root, port, runtime_password,
                 "INSERT INTO kiarabot.namespace_members(namespace_id,instance_id,can_read,"
                 "can_write,version,joined_at) VALUES (" + quote_sql(shared_namespace) + "," +
                 quote_sql(OTHER_INSTANCE) + ",true,true,1,1);"
                 "INSERT INTO kiarabot.namespace_values(namespace_id,key,value,version,"
                 "expires_at,updated_at) VALUES (" + quote_sql(shared_namespace) +
                 ",'shared/other-member','other',1,NULL,1);"
                 "INSERT INTO kiarabot.namespaces(namespace_id,installation_id,kind,"
                 "owner_instance,disabled,version,created_at,updated_at) VALUES (" +
                 quote_sql(other_namespace) + "," + quote_sql(OTHER_INSTALLATION) +
                 ",1,NULL,false,1,1,1);"
                 "INSERT INTO kiarabot.namespace_members(namespace_id,instance_id,can_read,"
                 "can_write,version,joined_at) VALUES (" + quote_sql(other_namespace) + "," +
                 quote_sql(OTHER_INSTANCE) + ",true,true,1,1);"
                 "INSERT INTO kiarabot.namespace_values(namespace_id,key,value,version,"
                 "expires_at,updated_at) VALUES (" + quote_sql(other_namespace) +
                 ",'other/keep','other-namespace',1,NULL,1)")
            psql(pg_bin, root, port, runtime_password,
                 "DELETE FROM kiarabot.namespace_members WHERE namespace_id=" +
                 quote_sql(shared_namespace) + " AND instance_id=" +
                 quote_sql(PLUGIN_INSTANCE))
            require_storage_error(await storage_frame(
                'storage revoked member denial',
                storage_get=plugins.StorageGetRequest(key='shared/seed', scope=shared_scope)),
                'revoked member access', common.PERMISSION_DENIED)
            first_values = int(psql(pg_bin, root, port, runtime_password,
                                     "SELECT count(*) FROM kiarabot.namespace_values WHERE "
                                     "namespace_id=" + quote_sql(shared_namespace)).stdout.strip())
            require(first_values == 3,
                    f'revoked member deletion removed shared values: {first_values}')
            other_values = int(psql(pg_bin, root, port, runtime_password,
                                     "SELECT count(*) FROM kiarabot.namespace_values WHERE "
                                     "namespace_id=" + quote_sql(other_namespace)).stdout.strip())
            require(other_values == 1, 'member revocation touched another namespace')
            other_member = int(psql(pg_bin, root, port, runtime_password,
                                     "SELECT count(*) FROM kiarabot.namespace_members WHERE "
                                     "namespace_id=" + quote_sql(shared_namespace) +
                                     " AND instance_id=" + quote_sql(OTHER_INSTANCE)).stdout.strip())
            require(other_member == 1, 'member revocation removed another member')
            psql(pg_bin, root, port, runtime_password,
                 "INSERT INTO kiarabot.namespace_members(namespace_id,instance_id,can_read,"
                 "can_write,version,joined_at) VALUES (" + quote_sql(shared_namespace) + "," +
                 quote_sql(PLUGIN_INSTANCE) + ",true,true,3,1) ON CONFLICT "
                 "(namespace_id,instance_id) DO UPDATE SET can_read=true,can_write=true,"
                 "version=3")
            psql(pg_bin, root, port, runtime_password,
                 "UPDATE kiarabot.namespace_members SET can_read=true,can_write=false "
                 "WHERE namespace_id=" + quote_sql(shared_namespace))
            require_storage_error(await storage_frame(
                'storage shared member denial',
                idempotency_key='p09-shared-denied-' + str(__import__('uuid').uuid4()),
                storage_set=plugins.StorageSetRequest(
                    key='shared/denied', value=b'x', scope=shared_scope,
                    condition=common.StorageCondition(absent=True))),
                'shared member denial', common.PERMISSION_DENIED)
            psql(pg_bin, root, port, runtime_password,
                 "UPDATE kiarabot.namespace_members SET can_read=true,can_write=true "
                 "WHERE namespace_id=" + quote_sql(shared_namespace))
            psql(pg_bin, root, port, runtime_password,
                 "UPDATE kiarabot.namespaces SET disabled=true WHERE namespace_id=" +
                 quote_sql(shared_namespace))
            require_storage_error(await storage_frame(
                'storage shared disabled',
                storage_get=plugins.StorageGetRequest(key='shared/seed', scope=shared_scope)),
                'shared disabled namespace', common.CONFLICT)
            require_storage_error(await storage_frame(
                'storage explicit shared id',
                storage_get=plugins.StorageGetRequest(
                    key='shared/seed',
                    scope=common.NamespaceScope(installation_shared_id=shared_namespace))),
                'plugin explicit shared namespace id', common.CAPABILITY_NOT_SUPPORTED)
            # Deleting the whole shared namespace must not touch another installation's
            # namespace or the current instance's private namespace.
            psql(pg_bin, root, port, runtime_password,
                 "DELETE FROM kiarabot.namespaces WHERE namespace_id=" +
                 quote_sql(shared_namespace))
            remaining = int(psql(pg_bin, root, port, runtime_password,
                                 "SELECT count(*) FROM kiarabot.namespace_values WHERE "
                                 "namespace_id=" + quote_sql(other_namespace)).stdout.strip())
            require(remaining == 1, 'shared namespace deletion cascaded into another namespace')
            private_after = await storage_frame(
                'storage private after shared delete',
                storage_get=plugins.StorageGetRequest(key='p09/keep', scope=private_scope))
            require(not private_after.HasField('error') and
                    private_after.storage_get.value == b'private-keep',
                    'shared namespace deletion removed private data')

            event_id = str(__import__('uuid').uuid4())
            envelope = events.EventEnvelope(
                event_id=event_id, trace_id=event_id, schema_version='kiarabot.v1',
                occurred_at=timestamp(time.time()),
                source=common.SourceRef(adapter_id=ADAPTER_ID, adapter_instance_id=ADAPTER_INSTANCE,
                                        account_id=ACCOUNT),
                subject=common.Subject(user_id='10001', conversation_id='group-1',
                                       group_id='group-1'),
                message_received=events.MessageReceived(
                    message_id='message-1', sender_id='10001',
                    conversation=common.ConversationRef(id='group-1',
                                                        type=common.ConversationRef.GROUP,
                                                        account_id=ACCOUNT)))
            await adapter_call.write(adapters.AdapterToCore(
                fence=adapter_fence, connection_proof=adapter_proof, event=envelope))
            event_ack = await read_or_fail(adapter_call, 'adapter event_ack')
            require(event_ack.WhichOneof('body') == 'event_ack',
                    'adapter event_ack returned the wrong frame')
            require(event_ack.event_ack.status == adapters.AdapterEventAck.PERSISTED,
                    f'adapter event rejected: status={event_ack.event_ack.status} '
                    f'error={event_ack.event_ack.error.code}')
            require(event_ack.event_ack.event_id == event_id, 'event_ack id mismatch')

            try:
                delivered = await read_or_fail(plugin_call, 'plugin delivery', timeout=15)
            except AcceptanceFailure as error:
                targets = psql(pg_bin, root, port, runtime_password,
                               'SELECT instance,state,attempts,(SELECT octet_length(serialized_envelope) FROM kiarabot.events) FROM kiarabot.delivery_targets')
                process.poll()
                error_file.flush()
                diagnostics = error_path.read_bytes()[-1200:]
                raise AcceptanceFailure(
                    f'plugin delivery timeout; targets={targets.stdout!r} '
                    f'child_exit={process.returncode} stderr={diagnostics!r}') from error
            require(delivered.WhichOneof('body') == 'event',
                    'plugin did not receive the delivered event')
            require(delivered.event.event_id == event_id, 'delivered event id mismatch')
            attempt = delivered.delivery_attempt
            require(attempt.event_id == event_id and attempt.attempt_id and
                    attempt.attempt_number >= 1 and attempt.HasField('started_at') and
                    attempt.HasField('lease_until'), 'delivery attempt incomplete')
            fence_fields(attempt.fence)
            require(attempt.fence == plugin_fence, 'delivery attempt fence differs from plugin fence')
            plugin_proof = plugin_result.authorization.proof

            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                renew_delivery=attempt))
            renew_receipt = await read_or_fail(plugin_call, 'plugin renew receipt')
            require(renew_receipt.WhichOneof('body') == 'delivery_receipt',
                    'renew returned the wrong frame')
            require(renew_receipt.delivery_receipt.disposition ==
                    platform.DELIVERY_RENEWED, 'renew disposition mismatch')
            require(renew_receipt.delivery_receipt.attempt.attempt_id == attempt.attempt_id,
                    'renew receipt attempt mismatch')

            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                event_ack=plugins.PluginEventAck(event_id=event_id, accepted=True,
                                                 attempt=attempt)))
            ack_receipt = await read_or_fail(plugin_call, 'plugin ack receipt')
            require(ack_receipt.WhichOneof('body') == 'delivery_receipt',
                    'ACK returned the wrong frame')
            require(ack_receipt.delivery_receipt.disposition ==
                    platform.DELIVERY_ACKNOWLEDGED, 'ACK disposition mismatch')

            # Stale attempt: the acknowledged attempt is no longer current and must never confirm
            # a future attempt. The stream stays usable.
            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                renew_delivery=attempt))
            stale_receipt = await read_or_fail(plugin_call, 'stale renew receipt')
            require(stale_receipt.WhichOneof('body') == 'delivery_receipt' and
                    stale_receipt.delivery_receipt.disposition == platform.DELIVERY_STALE,
                    'stale attempt was not DELIVERY_STALE')

            # Rejected retryable delivery: REJECTED, then maintenance redelivers a new attempt.
            event_id_2 = str(__import__('uuid').uuid4())
            envelope_2 = events.EventEnvelope(
                event_id=event_id_2, trace_id=event_id_2, schema_version='kiarabot.v1',
                occurred_at=timestamp(time.time()),
                source=common.SourceRef(adapter_id=ADAPTER_ID,
                                        adapter_instance_id=ADAPTER_INSTANCE,
                                        account_id=ACCOUNT),
                subject=common.Subject(user_id='10001', conversation_id='group-1',
                                       group_id='group-1'),
                message_received=events.MessageReceived(
                    message_id='message-2', sender_id='10001',
                    conversation=common.ConversationRef(id='group-1',
                                                        type=common.ConversationRef.GROUP,
                                                        account_id=ACCOUNT)))
            await adapter_call.write(adapters.AdapterToCore(
                fence=adapter_fence, connection_proof=adapter_proof, event=envelope_2))
            event_ack_2 = await read_or_fail(adapter_call, 'second adapter event_ack')
            require(event_ack_2.event_ack.status == adapters.AdapterEventAck.PERSISTED,
                    'second adapter event rejected')
            delivered_2 = await read_or_fail(plugin_call, 'second plugin delivery', timeout=15)
            attempt_2 = delivered_2.delivery_attempt
            require(delivered_2.WhichOneof('body') == 'event' and
                    delivered_2.event.event_id == event_id_2 and
                    attempt_2.attempt_number >= 1, 'second delivery incomplete')
            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                event_ack=plugins.PluginEventAck(
                    event_id=event_id_2, accepted=False,
                    error=common.Error(code=common.UPSTREAM_UNAVAILABLE, retryable=True),
                    attempt=attempt_2)))
            rejected_receipt = await read_or_fail(plugin_call, 'rejected receipt')
            require(rejected_receipt.WhichOneof('body') == 'delivery_receipt' and
                    rejected_receipt.delivery_receipt.disposition ==
                    platform.DELIVERY_REJECTED, 'rejected delivery disposition mismatch')
            redelivered = await read_or_fail(plugin_call, 'redelivery', timeout=15)
            attempt_3 = redelivered.delivery_attempt
            require(redelivered.WhichOneof('body') == 'event' and
                    redelivered.event.event_id == event_id_2 and
                    attempt_3.attempt_number > attempt_2.attempt_number,
                    'retryable rejection was not redelivered with a new attempt')
            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                event_ack=plugins.PluginEventAck(event_id=event_id_2, accepted=True,
                                                 attempt=attempt_3)))
            redelivery_ack = await read_or_fail(plugin_call, 'redelivery ack receipt')
            require(redelivery_ack.delivery_receipt.disposition ==
                    platform.DELIVERY_ACKNOWLEDGED, 'redelivery ACK mismatch')

            refresh_request_id = str(__import__('uuid').uuid4())
            await adapter_call.write(adapters.AdapterToCore(
                fence=adapter_fence, connection_proof=adapter_proof,
                refresh_authorization=platform.RefreshConnectionAuthorization(
                    request_id=refresh_request_id, fence=adapter_fence, proof=adapter_proof,
                    refresh_credential=adapter_refresh,
                    refresh_generation=adapter_refresh_generation)))
            refreshed = await read_or_fail(adapter_call, 'adapter refresh result')
            require(refreshed.WhichOneof('body') == 'authorization_result',
                    'refresh returned the wrong frame')
            result = refreshed.authorization_result
            require(result.request_id == refresh_request_id, 'refresh request id mismatch')
            require(result.WhichOneof('outcome') == 'authorization', 'refresh was rejected')
            renewed = result.authorization
            require(renewed.fence == adapter_fence, 'refresh changed the fence')
            require(renewed.refresh_generation == adapter_refresh_generation + 1,
                    'refresh generation did not advance')
            require(renewed.credential_version == adapter_auth.credential_version,
                    'refresh changed credential version')
            require(renewed.authorization_version >= adapter_auth.authorization_version,
                    'refresh lowered authorization version')
            require(renewed.proof != adapter_proof and
                    renewed.refresh_credential != adapter_refresh,
                    'refresh did not rotate both secrets')
            adapter_proof = renewed.proof
            adapter_refresh = renewed.refresh_credential
            adapter_refresh_generation = renewed.refresh_generation

            psql_result = psql(pg_bin, root, port, runtime_password,
                               'SELECT state FROM kiarabot.delivery_targets')
            require(sorted(psql_result.stdout.split()) == [b'3', b'3'],
                    f'delivery targets not acknowledged: {psql_result.stdout!r}')

            # Revocation must win within <=1s even though the peer still has valid proof.
            psql(pg_bin, root, port, runtime_password,
                 f"UPDATE kiarabot.bridge_credentials SET revoked=true WHERE "
                 f"resource_id={quote_sql(PLUGIN_INSTANCE)}")
            await asyncio.sleep(0.7)
            started = time.monotonic()
            await plugin_call.write(plugins.PluginToCore(
                fence=plugin_fence, connection_proof=plugin_proof,
                heartbeat=plugins.PluginHeartbeat(instance_id=PLUGIN_INSTANCE,
                                                  sent_at_unix_ms=int(time.time() * 1000))))
            await expect_rpc_error(plugin_call, 'revoked plugin stream',
                                   {_grpc().StatusCode.UNAUTHENTICATED,
                                    _grpc().StatusCode.PERMISSION_DENIED})
            require(time.monotonic() - started < 3.0, 'revocation took too long')

            # Cross-account injection must terminate the adapter stream without persistence or
            # fan-out.
            targets_before = psql(pg_bin, root, port, runtime_password,
                                  'SELECT count(*) FROM kiarabot.delivery_targets').stdout.strip()
            bad_event = str(__import__('uuid').uuid4())
            bad_envelope = events.EventEnvelope(
                event_id=bad_event, trace_id=bad_event, schema_version='kiarabot.v1',
                occurred_at=timestamp(time.time()),
                source=common.SourceRef(adapter_id=ADAPTER_ID,
                                        adapter_instance_id=ADAPTER_INSTANCE,
                                        account_id=UNBOUND_ACCOUNT),
                subject=common.Subject(user_id='10002', conversation_id='group-2',
                                       group_id='group-2'),
                message_received=events.MessageReceived(
                    message_id='message-bad', sender_id='10002',
                    conversation=common.ConversationRef(id='group-2',
                                                        type=common.ConversationRef.GROUP,
                                                        account_id=UNBOUND_ACCOUNT)))
            await adapter_call.write(adapters.AdapterToCore(
                fence=adapter_fence, connection_proof=adapter_proof, event=bad_envelope))
            await expect_rpc_error(adapter_call, 'unbound account event',
                                   {_grpc().StatusCode.PERMISSION_DENIED,
                                    _grpc().StatusCode.UNAUTHENTICATED})
            targets_after = psql(pg_bin, root, port, runtime_password,
                                 'SELECT count(*) FROM kiarabot.delivery_targets').stdout.strip()
            require(targets_before == targets_after,
                    'cross-account event was persisted or fanned out')

            # A fresh accepted stream receives the bounded ShutdownNotice before closure.
            shutdown_factory = channel.stream_stream(
                '/kiarabot.v1.PluginBridge/Connect',
                request_serializer=plugins.PluginToCore.SerializeToString,
                response_deserializer=plugins.CoreToPlugin.FromString)
            shutdown_call = shutdown_factory(
                metadata=(('authorization', 'Bearer ' + SHUTDOWN_BOOTSTRAP),))
            await shutdown_call.write(plugins.PluginToCore(
                register=plugins.RegisterPluginRequest(
                    manifest=plugins.PluginManifest(
                        id='p06.bridge.shutdown', name='P06 Shutdown Plugin', version='1.0.0',
                        api_version='kiarabot.plugin.v1', runtime='python-rpc',
                        entrypoint='plugin:factory', isolation='process', config_schema='{}',
                        ordered=False, capabilities=['event.message.received'],
                        permissions=['message.read']),
                    instance_id=SHUTDOWN_INSTANCE, supported_features=[])))
            shutdown_registered = await read_or_fail(shutdown_call, 'shutdown plugin registration')
            require(shutdown_registered.WhichOneof('body') == 'register_result' and
                    shutdown_registered.register_result.accepted,
                    'shutdown plugin registration rejected')

            process.send_signal(signal.SIGTERM)
            notice = await read_or_fail(shutdown_call, 'shutdown notice', timeout=5)
            require(notice.WhichOneof('body') == 'shutdown',
                    'Runtime closed without a ShutdownNotice')
            require(notice.shutdown.reason == 'shutdown' and
                    notice.shutdown.drain_timeout_ms == 1000,
                    'ShutdownNotice payload mismatch')
            require(process.wait(timeout=8) == 0, 'Runtime did not shut down cleanly')
            output = (process.stdout.read() if process.stdout is not None else b'') + (
                error_path.read_bytes() if error_path.exists() else b'')
            require(b'PRIVATE KEY' not in output and b'password=' not in output,
                    'Runtime diagnostics leaked secret material')
            process = None
            print('P06 external bridge TLS acceptance passed '
                  '(register/event/lease/renew/ack/refresh/stale/rejected-retry/'
                  'revocation/cross-account-rejection/shutdown-notice)')
    finally:
        if channel is not None:
            await channel.close()
        if process is not None and process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
        if 'error_file' in locals():
            error_file.close()
        if (root / 'data/postmaster.pid').exists():
            subprocess.run([pg_bin / 'pg_ctl', '-D', root / 'data', '-m', 'immediate', '-w',
                            '-t', '20', 'stop'], capture_output=True, timeout=30)
        shutil.rmtree(root, ignore_errors=True)


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
