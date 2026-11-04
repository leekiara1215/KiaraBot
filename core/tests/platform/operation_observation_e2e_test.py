#!/usr/bin/env python3
"""WS03 real owner journal/Identity/Control mTLS on the disposable owned PG fixture."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import traceback
import uuid

import identity_bootstrap_test as bootstrap
import p10_timer_e2e_test as timer
import runtime_identity_e2e_test as rt

STATE = {'stage': 'ownership'}


def require(condition, label):
    if not condition:
        raise AssertionError(label)


def prepare(root, enrolled):
    timer.write_scheduler_certificate(root / 'bootstrap-tls')
    principal = str(uuid.uuid4())
    enrolled['platform_peers'].append({
        'service': 'scheduler', 'principal_id': principal, 'credential_id': str(uuid.uuid4()),
        'certificate_chain_file': str(root / 'bootstrap-tls/scheduler.pem'), 'san': timer.SAN,
        'assignments': [{'assignment_id': str(uuid.uuid4()),
                         'permissions': ['identity.authorization.check'],
                         'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': principal}]}]})
    for owner, kind, permission in [('runtime', 'RESOURCE_ACCOUNT', 'resource.read'),
                                    ('scheduler', 'RESOURCE_TASK', 'task.read')]:
        resource, assignment = str(uuid.uuid4()), str(uuid.uuid4())
        STATE[owner] = {'resource': resource, 'assignment': assignment,
                        'operation': str(uuid.uuid4()), 'kind': kind}
        enrolled['administrator_assignments'].append({
            'assignment_id': assignment, 'permissions': [permission],
            'scope': [{'kind': kind, 'id': resource}]})
        enrolled['control_assignments'].append({
            'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
            'scope': [{'kind': kind, 'id': resource}]})


def verify_backend(root, port, pg_bin):
    # Verify the real server before ANY DDL, including bootstrap's own checks.
    query = subprocess.run([str(pg_bin / 'psql'), '-X', '-qAt', '-c', 'SHOW data_directory;'],
                           env=rt.postgres(root, pg_bin, port)[0], capture_output=True, timeout=10)
    require(query.returncode == 0 and Path(query.stdout.decode().strip()).resolve() == root / 'data',
            'fixture backend directory mismatch')


def scenario(root):
    STATE['stage'] = 'generated bindings'
    import grpc
    from kiarabot.v1 import (common_pb2 as common, identity_pb2 as identity,
                             identity_pb2_grpc as identity_grpc, observations_pb2 as observations,
                             platform_pb2 as platform, policy_pb2 as policy,
                             runtime_pb2_grpc as runtime_grpc, scheduler_pb2_grpc as scheduler_grpc)
    port, initializer = int(sys.argv[2]), sys.argv[3]
    pg_bin = Path(sys.argv[6])
    identity_bin, runtime_bin, scheduler_bin, seed_bin = sys.argv[7:11]
    tls = root / 'bootstrap-tls'
    STATE['stage'] = 'database initialization'
    rt.identity_security_material(root)
    runtime_db, _ = rt.runtime_database(root, pg_bin, port, initializer)
    scheduler_db = timer.scheduler_database(root, pg_bin, port, initializer)
    for owner in ('runtime', 'scheduler'):
        item = STATE[owner]
        seeded = subprocess.run([seed_bin, str(root), str(port), owner, item['resource'],
                                 item['operation']], capture_output=True, timeout=15,
                                env={k: v for k, v in os.environ.items() if not k.startswith('PG')})
        require(seeded.returncode == 0, 'production UoW seed')
    children, channels = [], []
    STATE['stage'] = 'process composition'
    try:
        identity_child, identity_state = rt.daemon(identity_bin, root / 'identity.json')
        children.append(identity_child)
        endpoint = '127.0.0.1:' + identity_state['grpc_port']
        stubs = {}
        peer_stubs = {}
        for owner, binary, database, stub in [
                ('runtime', runtime_bin, runtime_db, runtime_grpc.RuntimeServiceStub),
                ('scheduler', scheduler_bin, scheduler_db, scheduler_grpc.SchedulerServiceStub)]:
            config = {'schema_version': 1, 'service': owner, 'probe_listen': '127.0.0.1:0',
                      'grpc_listen': '127.0.0.1:0', 'database': database,
                      'tls': {'certificate_file': str(tls / f'{owner}.pem'),
                              'private_key_file': str(tls / f'{owner}.key'),
                              'ca_file': str(tls / 'ca.pem')}, 'identity_endpoint': endpoint}
            path = root / f'ws03-{owner}.json'
            path.write_text(json.dumps(config))
            path.chmod(0o600)
            child, state = rt.daemon(binary, path)
            children.append(child)
            owner_endpoint = '127.0.0.1:' + state['grpc_port']
            connection = rt.channel(owner_endpoint, tls)
            channels.append(connection)
            stubs[owner] = stub(connection)
            connection = rt.channel(owner_endpoint, tls, 'runtime.pem', 'runtime.key')
            channels.append(connection)
            peer_stubs[owner] = stub(connection)
        connection = rt.channel(endpoint, tls)
        channels.append(connection)
        identity_stub = identity_grpc.IdentityServiceStub(connection)
        access = identity_stub.Login(identity.IdentityLoginRequest(
            context=rt.context(), username='administrator',
            password=(root / 'admin-password').read_text().strip()), timeout=10).tokens.access_token

        def resources(owner, foreign=False):
            return platform.ResourceScope(resources=[platform.ResourceRef(
                kind=getattr(platform, STATE[owner]['kind']),
                id=str(uuid.uuid4()) if foreign else STATE[owner]['resource'])])

        def issue(owner):
            return identity_stub.IssueDelegation(identity.IdentityIssueDelegationRequest(
                context=rt.context(), subject_proof=access, audience=owner, scope=resources(owner),
                permissions=[policy.PERMISSION_RESOURCE_READ if owner == 'runtime' else
                             policy.PERMISSION_TASK_READ],
                client_certificate_sha256=rt.leaf_fingerprint(tls)), timeout=10).delegation.proof

        def request(owner, cursor=None, foreign=False, seconds=30):
            result = observations.OperationChangesRequest(
                context=rt.context(seconds=seconds, delegation=STATE[owner]['proof']),
                scope=resources(owner, foreign), page_size=1)
            if cursor is not None:
                result.cursor.CopyFrom(cursor)
            return result

        def denied(stub, value, allowed):
            try:
                stub.ListOperationChanges(value, timeout=2)
            except grpc.RpcError as error:
                require(error.code() in allowed, 'stable gRPC rejection')
                details = error.trailing_metadata() or ()
                payload = next((value for key, value in details if key == 'grpc-status-details-bin'), None)
                require(payload is not None, 'typed failure trailer')
                failure = platform.RpcFailure.FromString(payload)
                require(failure.error.code != common.ERROR_CODE_UNSPECIFIED and
                        failure.error.request_id == value.context.request_id and
                        failure.error.trace_id == value.context.trace_id,
                        'typed failure correlation')
                return failure
            raise AssertionError('rejected request unexpectedly succeeded')

        for owner in ('runtime', 'scheduler'):
            STATE['stage'] = owner + ' current authorization and persisted snapshot'
            STATE[owner]['proof'] = issue(owner)
            initial = stubs[owner].ListOperationChanges(request(owner), timeout=2)
            require(not initial.changes and initial.next_cursor.source_service == owner and
                    initial.next_cursor.sequence == 1 and initial.HasField('checked_at'),
                    'head-only first query')
            replay = platform.StreamCursor()
            replay.CopyFrom(initial.next_cursor)
            replay.sequence = 0
            page = stubs[owner].ListOperationChanges(request(owner, replay), timeout=2)
            require(len(page.changes) == 1 and not page.has_more and
                    page.changes[0].operation.ref.operation_id == STATE[owner]['operation'] and
                    page.changes[0].cursor.sequence == 1 and
                    page.changes[0].operation.state == platform.OPERATION_BLOCKED and
                    page.changes[0].operation.reason.code == common.UPSTREAM_UNAVAILABLE and
                    page.changes[0].operation.HasField('observed_at') and
                    len(page.changes[0].operation.participants) == 1 and
                    page.changes[0].operation.participants[0].reason.code == common.UPSTREAM_UNAVAILABLE,
                    'retained safe blocked participant projection')
            next_page = stubs[owner].ListOperationChanges(request(owner, page.next_cursor), timeout=2)
            require(not next_page.changes and next_page.next_cursor == page.next_cursor,
                    'resume has no duplicates')
            denied(stubs[owner], request(owner, foreign=True), {grpc.StatusCode.PERMISSION_DENIED})
            denied(peer_stubs[owner], request(owner), {grpc.StatusCode.UNAUTHENTICATED})
            denied(stubs[owner], request(owner, seconds=-1), {grpc.StatusCode.DEADLINE_EXCEEDED})
            replay.epoch = str(uuid.uuid4())
            gap = denied(stubs[owner], request(owner, replay), {grpc.StatusCode.ABORTED})
            require(gap.error.code == common.CONFLICT, 'epoch change needs resync')

        STATE['stage'] = 'current grant revocation'
        identity_stub.RevokeAssignment(identity.IdentityRevokeAssignmentRequest(
            context=rt.context(), assignment_id=STATE['scheduler']['assignment'], expected_version=1),
            timeout=10)
        # Identity invalidates a delegation whose authorization version is stale.
        # Then independently prove the CURRENT exact grant is gone at issuance.
        stale = denied(stubs['scheduler'], request('scheduler'), {grpc.StatusCode.UNAUTHENTICATED})
        require(stale.error.code == common.UNAUTHENTICATED, 'stale revoked proof is invalid')
        current = identity_stub.ValidateSession(identity.IdentityValidateSessionRequest(
            context=rt.context(), access_token=access), timeout=10)
        enrollment = json.loads((root / 'enrollment.json').read_text())
        require(current.principal.principal_id == enrollment['administrator_id'] and
                current.session.principal_id == enrollment['administrator_id'] and
                not current.session.revoked and current.authorization_version > 0,
                'fresh issuance uses a live human session over actual Control mTLS')
        try:
            issue('scheduler')
        except grpc.RpcError as error:
            require(error.code() == grpc.StatusCode.PERMISSION_DENIED,
                    'revoked exact scope cannot obtain a fresh proof')
            payload = dict(error.trailing_metadata() or ()).get('grpc-status-details-bin')
            require(payload is not None and
                    platform.RpcFailure.FromString(payload).error.code == common.PERMISSION_DENIED,
                    'current grant denial is typed')
        else:
            raise AssertionError('revoked exact grant issued a fresh proof')
        STATE['stage'] = 'identity dependency outage'
        identity_child.send_signal(signal.SIGTERM)
        identity_child.communicate(timeout=10)
        for owner in ('runtime', 'scheduler'):
            denied(stubs[owner], request(owner),
                   {grpc.StatusCode.UNAVAILABLE, grpc.StatusCode.DEADLINE_EXCEEDED})
        print('WS03 real R/S owner PG journal, Control mTLS, exact current grants, typed gap, '
              'revocation and Identity outage passed', flush=True)
    finally:
        for connection in channels:
            connection.close()
        for child in reversed(children):
            if child.poll() is None:
                child.send_signal(signal.SIGTERM)
                try:
                    child.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.communicate(timeout=5)


def main():
    if len(sys.argv) < 11:
        raise AssertionError('fixture backend directory mismatch')
    root = Path(sys.argv[1]).resolve()
    require(root.name.startswith('kiarabot-p03-') and not root.is_symlink() and
            root.stat().st_uid == os.getuid() and (root / 'p03-owned-marker').is_file(),
            'fixture backend directory mismatch')
    verify_backend(root, int(sys.argv[2]), Path(sys.argv[6]))
    STATE['stage'] = 'offline enrollment'
    bootstrap.main(prepare=prepare, after_bootstrap=scenario)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        if str(error) == 'fixture backend directory mismatch':
            print('fixture backend directory mismatch', file=sys.stderr)
        else:
            frames = traceback.extract_tb(sys.exc_info()[2])
            lines = ','.join(str(frame.lineno) for frame in frames[-2:])
            print(f'P03 assertion: WS03 {STATE["stage"]} lines {lines}; diagnostics redacted',
                  file=sys.stderr)
        sys.exit(1)
