#!/usr/bin/env python3
"""STR06: real Model/Identity mTLS, Model PG, public configuration and local HTTP SSE.

Owned run_database_test.py fixture only. No Runtime/Bridge/SDK feature is advertised here.
"""
from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys
import threading
import time
import traceback
import uuid

REPO = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(REPO / 'tests/architecture'), str(REPO / 'core/tests/platform'),
                str(REPO / 'proto/tests')]
import identity_bootstrap_test as bootstrap
import runtime_identity_e2e_test as rt
import p05b_model_chain_e2e as chain
import p14_native_single_host as native

STATE = {'stage': 'ownership'}


def require(value, label):
    if not value:
        raise AssertionError(label)


def prepare(root, enrolled):
    chain.prepare(root, enrolled)
    STATE.update(chain.STATE)
    STATE['read_assignment'] = str(uuid.uuid4())
    STATE['admin'] = enrolled['administrator_id']
    enrolled['administrator_assignments'].append({
        'assignment_id': STATE['read_assignment'], 'permissions': ['resource.read'],
        'scope': [{'kind': 'RESOURCE_MODEL', 'id': STATE['model_id']}]})
    enrolled['control_assignments'].append({
        'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
        'scope': [{'kind': 'RESOURCE_MODEL', 'id': STATE['model_id']}]})


class Provider:
    def __init__(self):
        self.requests = []
        self.release = threading.Event()
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                try:
                    body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                    require(self.headers.get('Authorization') == 'Bearer str-local-fixture',
                            'Provider receives sealed secret only at infrastructure')
                    require(body['model'] == 'str-fixture-model', 'pinned provider model')
                    text = body['messages'][0]['content']
                    outer.requests.append((bool(body.get('stream')), text))
                    if text == '429':
                        self.send_response(429)
                        self.send_header('Content-Length', '0')
                        self.send_header('Retry-After', '1')
                        self.end_headers()
                        return
                    if not body.get('stream'):
                        result = json.dumps({'id': 'str-unary', 'choices': [{'message': {
                            'role': 'assistant', 'content': 'whole'}}],
                            'usage': {'prompt_tokens': 1, 'completion_tokens': 2}}).encode()
                        self.send_response(200)
                        self.send_header('Content-Type', 'application/json')
                        self.send_header('Content-Length', str(len(result)))
                        self.end_headers()
                        self.wfile.write(result)
                        return
                    require(body.get('stream_options') == {'include_usage': True}, 'usage requested')
                    self.send_response(200)
                    self.send_header('Content-Type', 'text/event-stream')
                    self.send_header('Connection', 'close')
                    self.end_headers()

                    def event(value):
                        payload = value if isinstance(value, str) else json.dumps(value)
                        self.wfile.write(('data: ' + payload + '\r\n\r\n').encode())
                        self.wfile.flush()

                    event({'id': 'str-stream', 'choices': [{'index': 0, 'delta': {
                        'role': 'assistant', 'content': 'increment'}, 'finish_reason': None}]})
                    if text == 'partial':
                        self.close_connection = True
                        return
                    if text in {'hold', 'stall'}:
                        require(outer.release.wait(10), 'bounded fixture producer barrier')
                    event({'id': 'str-stream', 'choices': [{'index': 0, 'delta': {},
                           'finish_reason': 'length' if text == 'length' else 'stop'}]})
                    event({'choices': [], 'usage': {'prompt_tokens': 1, 'completion_tokens': 2}})
                    event('[DONE]')
                    self.close_connection = True
                except (BrokenPipeError, ConnectionResetError):
                    pass
                except Exception:
                    STATE['provider_error'] = True
                    self.close_connection = True

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def scenario(root):
    import grpc
    from kiarabot.v1 import (common_pb2 as common, identity_pb2 as identity,
                             identity_pb2_grpc as identity_grpc, model_pb2 as model,
                             model_pb2_grpc as model_grpc, model_stream_pb2 as stream,
                             observations_pb2 as observations, platform_pb2 as platform,
                             policy_pb2 as policy)
    from model_stream_contract_rules import model_stream_errors

    STATE['stage'] = 'production composition'
    pg_bin, port, initializer = Path(sys.argv[6]), int(sys.argv[2]), sys.argv[3]
    identity_bin, model_bin = sys.argv[7:9]
    database = native.create_database(root, pg_bin, port, initializer, 'model')
    rt.identity_security_material(root)
    tls = root / 'bootstrap-tls'
    children, channels = [], []
    provider = Provider()
    try:
        identity_child, identity_state = rt.daemon(identity_bin, root / 'identity.json')
        children.append(identity_child)
        identity_endpoint = '127.0.0.1:' + identity_state['grpc_port']
        root_key = root / 'model-root.key'
        root_key.write_text(secrets.token_hex(32) + '\n')
        root_key.chmod(0o600)
        config = {'schema_version': 1, 'service': 'model', 'grpc_listen': '127.0.0.1:0',
                  'probe_listen': '127.0.0.1:0', 'database': database,
                  'identity_endpoint': identity_endpoint,
                  'tls': {'certificate_file': str(tls / 'model.pem'),
                          'private_key_file': str(tls / 'model.key'), 'ca_file': str(tls / 'ca.pem')},
                  'model_security': {'root_key_file': {'id': 'model-root-v1', 'file': str(root_key)},
                                     'key_version': 1}}
        config_path = root / 'str06-model.json'
        chain.write_json(config_path, config)
        model_child, model_state = rt.daemon(model_bin, config_path)
        children.append(model_child)
        endpoint = '127.0.0.1:' + model_state['grpc_port']

        def channel(endpoint, leaf='control'):
            connection = rt.channel(endpoint, tls, leaf + '.pem', leaf + '.key')
            channels.append(connection)
            grpc.channel_ready_future(connection).result(timeout=5)
            return connection

        admin_identity = identity_grpc.IdentityServiceStub(channel(identity_endpoint))
        runtime_identity = identity_grpc.IdentityServiceStub(channel(identity_endpoint, 'runtime'))
        admin_model = model_grpc.ModelServiceStub(channel(endpoint))
        runtime_model = model_grpc.ModelServiceStub(channel(endpoint, 'runtime'))
        access = chain.admin_login(admin_identity, root)
        model_id = STATE['model_id']
        scope = platform.ResourceScope(resources=[platform.ResourceRef(kind=platform.RESOURCE_MODEL, id=model_id)])

        def issue(permissions):
            return chain.issue(admin_identity, access, 'model', list(scope.resources), permissions,
                               chain.pem_fingerprint(tls / 'control.pem'))

        manage = issue([policy.PERMISSION_MODEL_MANAGE])
        STATE['stage'] = 'public Model configuration'
        updated = admin_model.UpdateModel(model.ModelUpdateModelRequest(context=rt.context(15, manage),
            config=model.ModelConfiguration(model_id=model_id, alias='str-model',
                provider_kind='openai_chat_completions', endpoint='http://127.0.0.1:' +
                str(provider.server.server_port) + '/v1/chat/completions',
                provider_model='str-fixture-model', timeout_ms=5000, max_concurrency=2,
                state=platform.LIFECYCLE_DISABLED)), timeout=10)
        admin_model.RotateProviderSecret(model.ModelRotateProviderSecretRequest(
            context=rt.context(15, manage), model_id=model_id, new_secret=b'str-local-fixture'), timeout=10)
        admin_model.EnableModel(model.ModelEnableModelRequest(context=rt.context(15, manage),
            model_id=model_id, expected_version=updated.model.config_version), timeout=10)

        instance_subject = chain.register_instance_principal(admin_identity, STATE['instance'], root, pg_bin, port)
        parent = chain.issue_instance_parent_model(admin_identity, access, instance_subject,
            STATE['instance'], model_id, chain.pem_fingerprint(tls / 'control.pem'))
        child = runtime_identity.ExchangeDelegation(identity.IdentityExchangeDelegationRequest(
            context=rt.context(15), parent_proof=parent, audience='model', scope=scope,
            permissions=[policy.PERMISSION_MODEL_CHAT],
            client_certificate_sha256=chain.pem_fingerprint(tls / 'runtime.pem')), timeout=10).delegation.proof

        def request(text, logical=None):
            result = model.ModelGatewayChatRequest(context=rt.context(8, child),
                logical_call_id=logical or str(uuid.uuid4()), model_id=model_id,
                messages=[{'role': 'user', 'text': text}])
            result.context.idempotency_key = ''
            return result

        def collect(value):
            result = list(runtime_model.ChatStream(value, timeout=10))
            require(not model_stream_errors(result), 'production wire state machine')
            return result

        STATE['stage'] = 'increment before Provider completion'
        first = request('hold')
        iterator = runtime_model.ChatStream(first, timeout=10)
        frames = [next(iterator), next(iterator)]
        require(frames[0].HasField('started') and frames[1].HasField('delta') and
                not provider.release.is_set(), 'delta received while real HTTP Provider is blocked')
        provider.release.set()
        frames.extend(iterator)
        require(not model_stream_errors(frames) and
                frames[-1].terminal.termination == stream.MODEL_STREAM_COMPLETED and
                frames[-1].terminal.usage.known and frames[-1].terminal.may_have_billed,
                'complete only after Provider DONE and real PG txn2')
        count = len(provider.requests)
        replay = collect(first)
        require(len(replay) == 2 and replay[-1].terminal.termination == stream.MODEL_STREAM_REPLAY and
                len(provider.requests) == count, 'same stream identity never reinvokes Provider')
        unary_replay = runtime_model.Chat(first, timeout=10)
        require(not unary_replay.HasField('result') and len(provider.requests) == count,
                'stream to old unary replay')
        old = request('unary')
        require(runtime_model.Chat(old, timeout=10).result.text == 'whole', 'old production stream=false')
        count = len(provider.requests)
        require(collect(old)[-1].terminal.termination == stream.MODEL_STREAM_REPLAY and
                len(provider.requests) == count, 'old unary to stream replay')
        require(any(not streaming for streaming, _ in provider.requests), 'old Provider transport retained')

        STATE['stage'] = 'partial truncation rejection'
        partial = collect(request('partial'))[-1].terminal
        require(partial.termination == stream.MODEL_STREAM_FAILED and partial.partial and
                partial.call.state == platform.OPERATION_BLOCKED and partial.may_have_billed and
                not partial.usage.known, 'early HTTP EOF stays UNKNOWN and partial')
        truncated = collect(request('length'))[-1].terminal
        require(truncated.termination == stream.MODEL_STREAM_TRUNCATED and truncated.partial and
                truncated.call.state == platform.OPERATION_SUCCEEDED, 'length is explicit truncation')
        rate = collect(request('429'))[-1].terminal
        require(rate.termination == stream.MODEL_STREAM_FAILED and not rate.may_have_billed and
                rate.usage.known and rate.failure.error.code == common.RATE_LIMITED and
                rate.failure.context.retry_after_ms == 1000, '429 known not billed')

        def denied(attempt, allowed):
            before = len(provider.requests)
            try:
                attempt()
            except grpc.RpcError as error:
                require(error.code() in allowed, 'stable denied transport status')
                payload = dict(error.trailing_metadata() or ()).get('grpc-status-details-bin')
                require(payload is not None, 'typed RpcFailure trailer')
                require(platform.RpcFailure.FromString(payload).HasField('context'), 'typed recovery')
            else:
                raise AssertionError('denied request succeeded')
            require(len(provider.requests) == before, 'denied request sends no HTTP POST')

        STATE['stage'] = 'strict caller and request boundary'
        malformed = request('malformed'); malformed.context.idempotency_key = 'forbidden'
        denied(lambda: list(runtime_model.ChatStream(malformed, timeout=5)), {grpc.StatusCode.INVALID_ARGUMENT})
        direct = request('Control'); direct.context.delegation = manage
        denied(lambda: list(admin_model.ChatStream(direct, timeout=5)), {grpc.StatusCode.PERMISSION_DENIED})
        native.write_leaf(tls, 'foreign-runtime', 'spiffe://kiarabot/runtime-test')
        foreign = model_grpc.ModelServiceStub(channel(endpoint, 'foreign-runtime'))
        denied(lambda: list(foreign.ChatStream(request('foreign'), timeout=5)), {grpc.StatusCode.UNAUTHENTICATED})

        STATE['stage'] = 'cancel and durable UNKNOWN'
        provider.release.clear()
        active = request('stall')
        running = runtime_model.ChatStream(active, timeout=10)
        require(next(running).HasField('started') and next(running).HasField('delta'), 'partial before cancel')
        running.cancel()
        # The Provider stays blocked until the durable settlement is observed; releasing earlier
        # would race a successful completion against the cancellation and mask a wrong state.
        sql = rt.postgres(root, pg_bin, port, 'p03_model')
        until = time.monotonic() + 4
        while time.monotonic() < until:
            row = rt.run_sql(sql, "SELECT state::text||':'||may_have_billed::text FROM kiarabot.model_intents "
                "WHERE logical_call_id='" + active.logical_call_id + "';")
            if row == '3:true':
                break
            time.sleep(.02)
        require(row == '3:true', 'hard cancellation settles UNKNOWN conservatively')
        provider.release.set()

        STATE['stage'] = 'Model operation journal current scope'
        read = issue([policy.PERMISSION_RESOURCE_READ])

        def observation(cursor=None, foreign_scope=False):
            value = observations.OperationChangesRequest(context=rt.context(5, read), scope=scope, page_size=10)
            if cursor is not None:
                value.cursor.CopyFrom(cursor)
            if foreign_scope:
                value.scope.resources[0].id = str(uuid.uuid4())
            return value

        head = admin_model.ListOperationChanges(observation(), timeout=3)
        require(not head.changes and head.HasField('next_cursor'), 'no cursor reads persistent head')
        admin_model.RotateProviderSecret(model.ModelRotateProviderSecretRequest(
            context=rt.context(15, manage), model_id=model_id, new_secret=b'str-local-fixture', expected_version=1), timeout=10)
        changes = admin_model.ListOperationChanges(observation(head.next_cursor), timeout=3)
        require(changes.changes and all(item.operation.ref.owner_service == 'model' and
                item.operation.ref.resource.id == model_id for item in changes.changes),
                'Model operation journal returns committed exact-scope management changes')
        denied(lambda: runtime_model.ListOperationChanges(observation(), timeout=3), {grpc.StatusCode.UNAUTHENTICATED, grpc.StatusCode.PERMISSION_DENIED})
        denied(lambda: admin_model.ListOperationChanges(observation(foreign_scope=True), timeout=3), {grpc.StatusCode.PERMISSION_DENIED})
        gap = observations.OperationChangesRequest(); gap.CopyFrom(observation(head.next_cursor))
        gap.cursor.epoch = 'foreign-epoch'
        denied(lambda: admin_model.ListOperationChanges(gap, timeout=3), {grpc.StatusCode.ABORTED})

        STATE['stage'] = 'current grant revocation'
        admin_identity.RevokeAssignment(identity.IdentityRevokeAssignmentRequest(
            context=rt.context(15), assignment_id=STATE['read_assignment'],
            expected_version=1), timeout=10)
        denied(lambda: admin_model.ListOperationChanges(observation(), timeout=3),
               {grpc.StatusCode.PERMISSION_DENIED, grpc.StatusCode.UNAUTHENTICATED})
        STATE['stage'] = 'Identity dependency outage'
        identity_child.send_signal(signal.SIGTERM); identity_child.communicate(timeout=10)
        denied(lambda: admin_model.ListOperationChanges(observation(), timeout=3),
               {grpc.StatusCode.UNAVAILABLE, grpc.StatusCode.DEADLINE_EXCEEDED})
        require(not STATE.get('provider_error'), 'local Provider fixture passed protocol assertions')
        print('STR06 Model ChatStream and operation journal: real TLS/Identity/PG/HTTP passed', flush=True)
    finally:
        provider.close()
        for connection in channels:
            connection.close()
        for process in reversed(children):
            chain.stop(process)


def main():
    if len(sys.argv) < 9:
        raise AssertionError('fixture backend directory mismatch')
    root = Path(sys.argv[1]).resolve()
    require(root.name.startswith('kiarabot-p03-') and root.stat().st_uid == os.getuid() and
            (root / 'p03-owned-marker').is_file(), 'fixture backend directory mismatch')
    result = subprocess.run([str(Path(sys.argv[6]) / 'psql'), '-X', '-qAt', '-c', 'SHOW data_directory;'],
                            env=rt.postgres(root, Path(sys.argv[6]), int(sys.argv[2]))[0],
                            capture_output=True, timeout=10)
    require(result.returncode == 0 and Path(result.stdout.decode().strip()).resolve() == root / 'data',
            'fixture backend directory mismatch')
    bootstrap.main(control_server_certificate=False, prepare=prepare, after_bootstrap=scenario)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        if str(error) == 'fixture backend directory mismatch':
            print('fixture backend directory mismatch', file=sys.stderr)
        else:
            lines = ','.join(str(frame.lineno) for frame in traceback.extract_tb(sys.exc_info()[2])[-3:])
            print(f'P03 assertion: STR06 {STATE["stage"]} lines {lines}; diagnostics redacted', file=sys.stderr)
        sys.exit(1)
