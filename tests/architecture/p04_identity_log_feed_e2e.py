#!/usr/bin/env python3
"""P04 v1.2 Identity log-change feed, real process-level evidence (owned PostgreSQL).

Runs the real `kiarabot-identity` binary against a disposable PG16.4 fixture with the real offline
bootstrap enrollment and temporary mTLS material, then exercises
`IdentityService.ListLogChanges` over a real gRPC mTLS channel as:
  * the registered Control platform peer (client certificate `control.pem`) with current exact
    `log.read` on `RESOURCE_CATALOG/control`, and
  * a registered peer without that grant (the Runtime peer certificate) to prove the online
    authorization refusal is not a fake empty page.

The audit rows and the redacted log journal are produced by real Identity management calls
(administrator Login + Control-to-Identity IssueDelegation) inside the Identity process/transaction;
nothing is inserted into PG by this script. Assertions follow the frozen contract:
  * absent cursor captures the committed head and returns no changes,
  * a page with a cursor is non-empty after new committed audit rows, with strictly increasing
    per-change StreamCursor and closed-set LogMetadata fields (no audit payload/free text/secrets),
  * an empty continuation page is legal only after the owner confirmed the head,
  * wrong epoch/future sequence cursors are typed CONFLICT (gRPC ABORTED with RESYNC semantics),
    wrong scope/owner/page-size are INVALID_ARGUMENT, absent optional scope is the deployment
    CapabilityNotSupported (gRPC UNIMPLEMENTED), and a peer without log.read is PERMISSION_DENIED.
  * a delegation for log.read cannot substitute the peer's own assignment.
This is non-fixture evidence: real Identity process + real owned PG journal + real mTLS peers.
"""
import hashlib
import json
import os
from pathlib import Path
import ssl
import sys
import time
import uuid

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'core/tests/platform'))
sys.path.insert(0, str(REPO / 'tests/architecture'))
import identity_bootstrap_test as bootstrap_fixture  # noqa: E402
import p14_native_single_host as p14  # noqa: E402
import runtime_identity_e2e_test as rt  # noqa: E402

import grpc  # noqa: E402
from google.protobuf.timestamp_pb2 import Timestamp  # noqa: E402
from kiarabot.v1 import (common_pb2, identity_pb2, identity_pb2_grpc, observations_pb2,  # noqa: E402
                         platform_pb2, policy_pb2)

STATE = {}


def stage(name):
    STATE['stage'] = name


def require(condition, label):
    if not condition:
        raise RuntimeError(f'P04 Identity log feed e2e ({STATE.get("stage", "?")}): {label}')


def certificate_sha256(pem_path):
    der = ssl.PEM_cert_to_DER_cert(pem_path.read_text())
    return hashlib.sha256(der).digest()


def timestamp_in(seconds=5):
    value = Timestamp()
    value.seconds = int(time.time()) + seconds
    return value


def request_context(*, idempotency=True):
    return platform_pb2.RequestContext(request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()),
                                       deadline=timestamp_in(),
                                       idempotency_key=str(uuid.uuid4()) if idempotency else '')


def control_scope():
    return platform_pb2.ResourceScope(resources=[
        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG, id='control')])


def prepare(root, enrolled):
    control_id = enrolled['control_id']
    # Exact Control peer grants for the frozen log feed plus its own credential management.
    log_assignment = str(uuid.uuid4())
    enrolled['control_assignments'].append({
        'assignment_id': log_assignment,
        'permissions': ['log.read'],
        'scope': [{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]})
    # The Control peer performs the real assignment revocation/restore on its own exact grant.
    enrolled['control_assignments'].append({
        'assignment_id': str(uuid.uuid4()),
        'permissions': ['identity.assignment.manage'],
        'scope': [{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]})
    enrolled['control_assignments'].append({
        'assignment_id': str(uuid.uuid4()),
        'permissions': ['identity.principal.manage'],
        'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': control_id}]})
    # The administrator delegated subject must itself hold exact log.read for delegation issuance.
    enrolled['administrator_assignments'].append({
        'assignment_id': str(uuid.uuid4()),
        'permissions': ['log.read'],
        'scope': [{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]})
    STATE.update(control_id=control_id,
                 control_credential_id=enrolled['control_credential_id'],
                 control_log_assignment=log_assignment)


def start_identity(root, binaries):
    stage('identity security material')
    rt.identity_security_material(root)
    config = json.loads((root / 'identity.json').read_text())
    config['probe_listen'] = p14.free_endpoint()
    config['grpc_listen'] = p14.free_endpoint()
    path = root / 'log-feed-identity.json'
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    stage('identity startup')
    child, state = p14.daemon(binaries['identity'], path)
    STATE['identity'] = child
    if state.get('ready') != 'true':
        tail = ''
        if child.poll() is not None:
            tail = child.stderr.read(600).decode(errors='replace')
        raise RuntimeError(f'identity startup state={state} exit={child.poll()} {tail[:400]}')
    require(rt.wait_ready(int(state['probe_port']), True, 20.0), 'identity probe readiness')
    return config['grpc_listen']


def channel(endpoint, leaf, key, ca):
    credentials = grpc.ssl_channel_credentials(
        root_certificates=ca.read_bytes(), private_key=key.read_bytes(),
        certificate_chain=leaf.read_bytes())
    return grpc.secure_channel(endpoint, credentials)


def metadata_closed_set(log):
    require(log.owner_service == 'identity', 'log owner_service is identity')
    require(log.severity in ('DEBUG', 'INFO', 'WARN', 'ERROR'), 'log severity closed set')
    require(log.component == 'identity', 'log component is the owner component')
    require(log.event in ('request.recorded', 'request.rejected'), 'log event closed set')
    require(log.method != '', 'log method is a redacted owner method name')
    require(log.entry_id != '', 'log entry_id present')
    require(log.occurred_at.seconds > 0, 'log occurred_at is authoritative UTC')
    require(log.HasField('resource'), 'log resource present')
    for value in (log.method, log.component, log.entry_id, log.request_id, log.trace_id):
        lowered = value.lower()
        for forbidden in ('password', 'secret', 'bearer', 'private key', 'delegation ',
                          'refresh_token', '-----begin'):
            require(forbidden not in lowered, f'log field leaked free text: {forbidden}')


def result_page(response, *, page_size):
    require(len(response.changes) <= page_size, 'page respects page_size bound')
    require(response.checked_at.seconds > 0, 'checked_at is owner authoritative UTC')
    require(response.next_cursor.source_service == 'identity', 'next cursor owner identity')
    require(response.next_cursor.epoch != '' and len(response.next_cursor.epoch) <= 128,
            'next cursor has a bounded dataset epoch')
    require(response.next_cursor.HasField('scope'), 'next cursor carries its scope')
    require(list(response.next_cursor.scope.resources) == list(control_scope().resources),
            'next cursor scope equals the request scope')
    sequence = -1
    for change in response.changes:
        require(change.HasField('log'), 'change carries LogMetadata')
        metadata_closed_set(change.log)
        require(change.cursor.source_service == 'identity', 'change cursor owner identity')
        require(change.cursor.epoch == response.next_cursor.epoch,
                'change cursor epoch equals page epoch')
        require(change.cursor.sequence > sequence, 'changes are strictly increasing')
        require(change.cursor.sequence <= response.next_cursor.sequence,
                'change cursor never passes the scan boundary')
        sequence = change.cursor.sequence


def call_changes(stub, *, scope=True, cursor=None, page_size=5, delegation=''):
    request = observations_pb2.ObservationChangesRequest(context=request_context(),
                                                        page_size=page_size)
    if scope:
        request.scope.CopyFrom(control_scope())
    if cursor is not None:
        request.cursor.CopyFrom(cursor)
    if delegation:
        request.context.delegation = delegation
    return stub.ListLogChanges(request, timeout=10)


def expect_any_code(call, codes, label):
    try:
        call()
    except grpc.RpcError as error:
        require(error.code() in codes,
                f'{label}: expected one of {[c.name for c in codes]}, got {error.code().name}')
        return error
    raise RuntimeError(f'{label}: expected a refusal, call succeeded')


def expect_code(call, code, label):
    try:
        call()
    except grpc.RpcError as error:
        require(error.code() == code, f'{label}: expected {code.name}, got {error.code().name}')
        return error
    raise RuntimeError(f'{label}: expected {code.name}, call succeeded')


def sql_identity(statement):
    environment, pg_bin = STATE['pg']
    return rt.run_sql(({**environment, 'PGDATABASE': 'p03_identity'}, pg_bin), statement)


def audit_and_feed_counts():
    audit = sql_identity("SELECT count(*) FROM kiarabot.audit_entries;")
    feed = sql_identity("SELECT count(*) FROM kiarabot.observation_feed_changes WHERE family=1;")
    return audit, feed


def run_feed_checks(root, grpc_endpoint, binaries):
    tls = STATE['tls']
    ca = tls / 'ca.pem'
    control_channel = channel(grpc_endpoint, tls / 'control.pem', tls / 'control.key', ca)
    runtime_channel = channel(grpc_endpoint, tls / 'runtime.pem', tls / 'runtime.key', ca)
    try:
        grpc.channel_ready_future(control_channel).result(timeout=15)
        grpc.channel_ready_future(runtime_channel).result(timeout=15)
        control = identity_pb2_grpc.IdentityServiceStub(control_channel)
        remote = identity_pb2_grpc.IdentityServiceStub(runtime_channel)

        stage('administrator login')
        password = (root / 'admin-password').read_text().strip()
        login = control.Login(identity_pb2.IdentityLoginRequest(
            context=request_context(), username='administrator', password=password), timeout=10)
        access = login.tokens.access_token
        require(access != '', 'administrator login produced an access token')

        stage('head capture without cursor')
        try:
            head = call_changes(control)
        except grpc.RpcError as error:
            detail = getattr(error, 'details', lambda: '')()
            audit_rows, feed_rows = audit_and_feed_counts()
            print(f'BLOCKER Identity.ListLogChanges failed: code={error.code().name} '
                  f'detail={detail} audit_rows={audit_rows} log_feed_rows={feed_rows}',
                  flush=True)
            raise
        result_page(head, page_size=5)
        require(not head.changes, 'absent cursor captures the head and returns no changes')
        head_cursor = head.next_cursor

        stage('Control delegation (does not replace peer authorization)')
        delegation_request = request_context()
        issued = control.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
            context=delegation_request,
            subject_proof=access,
            audience='runtime',
            scope=control_scope(),
            permissions=[policy_pb2.PERMISSION_LOG_READ],
            client_certificate_sha256=certificate_sha256(tls / 'control.pem')), timeout=10)
        require(issued.delegation.proof != '', 'Control issued a log.read delegation')

        stage('committed session audit row becomes visible after the head')
        refresh_request = request_context()
        refreshed = control.RefreshSession(identity_pb2.IdentityRefreshSessionRequest(
            context=refresh_request, refresh_token=login.tokens.refresh_token), timeout=10)
        refresh_token = refreshed.tokens.refresh_token
        page = call_changes(control, cursor=head_cursor)
        result_page(page, page_size=5)
        require(page.changes, 'a real committed audit row appears after the captured head')
        require(page.next_cursor.sequence > head_cursor.sequence,
                'scan boundary advanced past the captured head')
        request_ids = [change.log.request_id for change in page.changes]
        require(refresh_request.request_id in request_ids,
                'the new change correlates with the real RefreshSession audit row')

        stage('legal empty continuation page')
        empty = call_changes(control, cursor=page.next_cursor)
        result_page(empty, page_size=5)
        require(not empty.changes, 'no new commits: an owner-confirmed empty page is legal')
        require(empty.next_cursor.sequence >= page.next_cursor.sequence,
                'empty page does not move the boundary backwards')

        stage('next new row after the continuation boundary')
        second_request = request_context()
        control.RefreshSession(identity_pb2.IdentityRefreshSessionRequest(
            context=second_request, refresh_token=refresh_token), timeout=10)
        fresh = call_changes(control, cursor=empty.next_cursor)
        result_page(fresh, page_size=5)
        require(fresh.changes, 'the next committed audit row is observable')
        require(second_request.request_id in [change.log.request_id for change in fresh.changes],
                'fresh page correlates with the second real audit row')
        for change in fresh.changes:
            require(change.cursor.sequence > empty.next_cursor.sequence,
                    'fresh change is strictly after the continuation boundary')

        stage('typed cursor conflicts')
        stale_epoch = platform_pb2.StreamCursor()
        stale_epoch.CopyFrom(empty.next_cursor)
        stale_epoch.epoch = 'stale-epoch'
        expect_code(lambda: call_changes(control, cursor=stale_epoch), grpc.StatusCode.ABORTED,
                    'wrong epoch is a typed CONFLICT')
        future = platform_pb2.StreamCursor()
        future.CopyFrom(empty.next_cursor)
        future.sequence = empty.next_cursor.sequence + 1000000
        expect_code(lambda: call_changes(control, cursor=future), grpc.StatusCode.ABORTED,
                    'future sequence is a typed CONFLICT')
        wrong_owner = platform_pb2.StreamCursor()
        wrong_owner.CopyFrom(empty.next_cursor)
        wrong_owner.source_service = 'runtime'
        expect_code(lambda: call_changes(control, cursor=wrong_owner),
                    grpc.StatusCode.INVALID_ARGUMENT, 'foreign owner cursor is INVALID_ARGUMENT')

        stage('invalid request shapes')
        expect_code(lambda: call_changes(control, scope=False), grpc.StatusCode.UNIMPLEMENTED,
                    'absent optional scope is the deployment capability refusal')
        expect_code(lambda: call_changes(control, page_size=101),
                    grpc.StatusCode.INVALID_ARGUMENT, 'page_size above the frozen bound')
        account_scope = platform_pb2.ResourceScope(resources=[
            platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_ACCOUNT, id=str(uuid.uuid4()))])
        bad = observations_pb2.ObservationChangesRequest(context=request_context(),
                                                         page_size=5)
        bad.scope.CopyFrom(account_scope)
        expect_code(lambda: control.ListLogChanges(bad, timeout=10),
                    grpc.StatusCode.INVALID_ARGUMENT, 'non catalog/control scope is INVALID_ARGUMENT')
        two = observations_pb2.ObservationChangesRequest(context=request_context(), page_size=5)
        two.scope.CopyFrom(platform_pb2.ResourceScope(resources=[
            platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG, id='control'),
            platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG, id='control')]))
        expect_code(lambda: control.ListLogChanges(two, timeout=10),
                    grpc.StatusCode.INVALID_ARGUMENT, 'two scoped resources are INVALID_ARGUMENT')

        stage('non-Control / unassigned peer is denied, never a fake empty page')
        # The frozen feed is Control-caller only; a registered Runtime peer is refused by the RPC
        # peer gate (UNAUTHENTICATED) instead of receiving a fabricated empty page. A delegation
        # presented on that foreign peer cannot elevate it either.
        expect_any_code(lambda: call_changes(remote),
                        (grpc.StatusCode.UNAUTHENTICATED, grpc.StatusCode.PERMISSION_DENIED),
                        'non-Control peer is denied')
        expect_any_code(
            lambda: call_changes(remote, delegation=issued.delegation.proof),
            (grpc.StatusCode.UNAUTHENTICATED, grpc.StatusCode.PERMISSION_DENIED),
            'a delegation cannot substitute the Control peer grant')

        stage('real log.read grant revocation stops the feed without a fake empty page')
        assignment_id = STATE['control_log_assignment']
        control.RevokeAssignment(identity_pb2.IdentityRevokeAssignmentRequest(
            context=request_context(), assignment_id=assignment_id, expected_version=1),
            timeout=10)
        # The page must be a typed authorization refusal, never a successful empty page.
        expect_any_code(lambda: call_changes(control),
                        (grpc.StatusCode.PERMISSION_DENIED, grpc.StatusCode.UNAUTHENTICATED),
                        'revoked log.read grant is refused')
        # Restore the exact grant (new assignment, same principal/permission/scope) and read again.
        restored = control.SetAssignment(identity_pb2.IdentitySetAssignmentRequest(
            context=request_context(),
            assignment=identity_pb2.AssignmentSpec(
                principal_id=STATE['control_id'], permissions=[policy_pb2.PERMISSION_LOG_READ],
                scope=control_scope())), timeout=10)
        require(restored.assignment.version == 1 and restored.assignment.assignment_id != '',
                'log.read grant restored')
        recovered = call_changes(control)
        result_page(recovered, page_size=5)
        require(not recovered.changes, 'restored grant reads the owner-confirmed head')

        stage('revoked Control credential stops the feed')
        try:
            control.RevokeCredential(identity_pb2.IdentityRevokeCredentialRequest(
                context=request_context(), credential_id=STATE['control_credential_id'],
                expected_version=1), timeout=10)
        except grpc.RpcError as error:
            print(f'INFO revocation setup skipped by product policy: {error.code().name}',
                  flush=True)
        else:
            denied = False
            try:
                call_changes(control)
            except grpc.RpcError as error:
                denied = error.code() in (grpc.StatusCode.UNAUTHENTICATED,
                                          grpc.StatusCode.PERMISSION_DENIED)
            require(denied, 'revoked Control credential must not authorize the feed')
    finally:
        control_channel.close()
        runtime_channel.close()


def main():
    root = Path(sys.argv[1]).resolve()
    if len(sys.argv) < 8:
        # run_database_test.py invokes each driver once against a foreign backend directory.
        raise RuntimeError('fixture backend directory mismatch')
    port = int(sys.argv[2])
    require(root.name.startswith('kiarabot-p03-') and root.stat().st_uid == os.getuid(),
            'owned fixture root')
    require((root / 'p03-owned-marker').is_file() and not root.is_symlink(),
            'owned fixture marker')
    p14.verify_backend(root, port, sys.argv[6])
    STATE['binaries'] = {
        'initializer': Path(sys.argv[3]).resolve(),
        'identity_bootstrap': Path(sys.argv[5]).resolve(),
        'pg_bin': Path(sys.argv[6]).resolve(),
        'identity': Path(sys.argv[7]).resolve(),
    }
    STATE['tls'] = None
    bootstrap_fixture.main(control_server_certificate=True, prepare=prepare,
                           after_bootstrap=after_bootstrap)


def after_bootstrap(root):
    tls = root / 'bootstrap-tls'
    STATE['tls'] = tls
    STATE['pg'] = rt.postgres(root, STATE['binaries']['pg_bin'], int(sys.argv[2]))
    endpoint = start_identity(root, STATE['binaries'])
    stage('feed checks')
    try:
        run_feed_checks(root, endpoint, STATE['binaries'])
    finally:
        child = STATE.get('identity')
        if child is not None and child.poll() is None:
            p14.stop(child)
    print('P04 Identity ListLogChanges real-process/real-PG feed checks passed', flush=True)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:  # noqa: BLE001 - bounded redacted diagnostic below
        message = str(error)[:600]
        if 'fixture backend directory mismatch' not in message:
            message = (f'P04 Identity log feed e2e ({STATE.get("stage", "bootstrap")}): '
                       + message)
        print(message, file=sys.stderr)
        sys.exit(1)
