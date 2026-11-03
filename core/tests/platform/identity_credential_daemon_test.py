#!/usr/bin/env python3
"""P04 credential delivery against an owned PG16.4 and the production mTLS Identity daemon."""
import json
from pathlib import Path
import subprocess
import sys
import traceback
import uuid

import grpc
from kiarabot.v1 import identity_pb2 as identity, identity_pb2_grpc, platform_pb2

import identity_bootstrap_test as bootstrap
import runtime_identity_e2e_test as fixture


INSTANCE = str(uuid.uuid4())


def prepare(_root, enrollment):
    for kind, value in [('RESOURCE_INSTANCE', INSTANCE),
                        ('RESOURCE_PRINCIPAL', enrollment['control_id']),
                        ('RESOURCE_PRINCIPAL', enrollment['administrator_id'])]:
        enrollment['control_assignments'].append({
            'assignment_id': str(uuid.uuid4()),
            'permissions': ['identity.principal.manage', 'identity.principal.read'],
            'scope': [{'kind': kind, 'id': value}]})


def rejected(call, request, expected):
    try:
        call(request, timeout=5)
    except grpc.RpcError as error:
        assert error.code() == expected, f'unexpected gRPC rejection {error.code()}'
    else:
        raise AssertionError('request unexpectedly allowed')


def human_password(root, enrollment, api, secrets):
    credential = enrollment['password_credential_id']
    original = (root / 'admin-password').read_text().strip()

    def login(password):
        return identity.IdentityLoginRequest(context=fixture.context(),
                                            username=enrollment['username'], password=password)

    old_session = api.Login(login(original), timeout=5).tokens
    rotate = identity.IdentityRotateCredentialRequest(
        context=fixture.context(), credential_id=credential, expected_version=1)
    delivery = api.RotateCredential(rotate, timeout=5).delivery
    assert delivery.version == 2 and api.RotateCredential(rotate, timeout=5).delivery == delivery
    secrets.extend([original.encode(), delivery.new_secret, old_session.access_token.encode(),
                    old_session.refresh_token.encode()])
    rejected(api.Login, login(original), grpc.StatusCode.UNAUTHENTICATED)
    current = api.Login(login(delivery.new_secret.decode()), timeout=5).tokens
    validate = identity.IdentityValidateSessionRequest(
        context=fixture.context(), access_token=current.access_token)
    assert api.ValidateSession(validate, timeout=5).principal.principal_id == enrollment['administrator_id']
    secrets.extend([current.access_token.encode(), current.refresh_token.encode()])
    revoke = identity.IdentityRevokeCredentialRequest(
        context=fixture.context(), credential_id=credential, expected_version=delivery.version)
    assert api.RevokeCredential(revoke, timeout=5).version == 3
    rejected(api.Login, login(delivery.new_secret.decode()), grpc.StatusCode.UNAUTHENTICATED)
    rejected(api.ValidateSession, validate, grpc.StatusCode.UNAUTHENTICATED)


def scenario(root):
    binary = str(Path(sys.argv[7]).resolve())
    db = fixture.postgres(root, Path(sys.argv[6]), sys.argv[2], 'p03_identity')
    admin_db = fixture.postgres(root, Path(sys.argv[6]), sys.argv[2])
    config = root / 'identity.json'
    tls = root / 'bootstrap-tls'
    enrollment = json.loads((root / 'enrollment.json').read_text())
    fixture.identity_security_material(root)
    secrets = []

    def start():
        process, state = fixture.daemon(binary, config)
        if state['ready'] != 'true':
            process.terminate()
            process.communicate(timeout=10)
            raise AssertionError('Identity did not acquire writer ownership')
        channel = fixture.channel('127.0.0.1:' + state['grpc_port'], tls)
        return process, state, channel, identity_pb2_grpc.IdentityServiceStub(channel)

    def stop():
        channel.close()
        process.terminate()
        output, errors = process.communicate(timeout=10)
        assert not any(value in output + errors for value in secrets), 'Identity logged delivered material'

    register = identity.IdentityRegisterPrincipalRequest(
        context=fixture.context(), principal_kind='plugin',
        binding=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE, id=INSTANCE),
        allowed_services=['runtime'])
    process, state, channel, api = start()
    try:
        created = api.RegisterPrincipal(register, timeout=5)
        credential = created.initial_credential
        assert credential.state == identity.CREDENTIAL_PENDING
        assert credential.principal_id == created.principal.principal_id
        rotate = identity.IdentityRotateCredentialRequest(
            context=fixture.context(), credential_id=credential.credential_id,
            expected_version=credential.version)
        first = api.RotateCredential(rotate, timeout=5).delivery
        assert first.new_secret and first.delivery_receipt_id and first.version == credential.version + 1
        secrets.append(first.new_secret)
        row = fixture.run_sql(db, "SELECT encode(ciphertext,'hex') FROM kiarabot.secret_receipts "
                             f"WHERE credential_id='{credential.credential_id}';")
        assert row and first.new_secret not in bytes.fromhex(row)
        status = identity.IdentityGetCredentialStatusRequest(
            context=fixture.context(), credential_id=credential.credential_id)
        assert api.GetCredentialStatus(status, timeout=5).credential.state == identity.CREDENTIAL_ACTIVE
    finally:
        stop()

    process, state, channel, api = start()
    try:
        # New transport IDs and deadline, same idempotency scope after the first delivery was lost.
        for request in [register, rotate, status]:
            key = request.context.idempotency_key
            request.context.CopyFrom(fixture.context())
            request.context.idempotency_key = key
        assert api.RegisterPrincipal(register, timeout=5) == created
        assert api.RotateCredential(rotate, timeout=5).delivery == first
        principal = api.GetPrincipalStatus(identity.IdentityGetPrincipalStatusRequest(
            context=fixture.context(), principal_id=created.principal.principal_id), timeout=5)
        assert principal.principal.version > created.principal.version
        different = identity.IdentityRegisterPrincipalRequest()
        different.CopyFrom(register)
        different.allowed_services[:] = ['model']
        rejected(api.RegisterPrincipal, different, grpc.StatusCode.ABORTED)
        certificate_rotation = identity.IdentityRotateCredentialRequest(
            context=fixture.context(), credential_id=enrollment['control_credential_id'], expected_version=1)
        rejected(api.RotateCredential, certificate_rotation, grpc.StatusCode.UNIMPLEMENTED)
        with fixture.channel('127.0.0.1:' + state['grpc_port'], tls, 'runtime.pem', 'runtime.key') as other:
            denied = identity_pb2_grpc.IdentityServiceStub(other)
            rejected(denied.RotateCredential, rotate, grpc.StatusCode.NOT_FOUND)
        revoke = identity.IdentityRevokeCredentialRequest(context=fixture.context(),
            credential_id=credential.credential_id, expected_version=first.version)
        end = api.RevokeCredential(revoke, timeout=5)
        assert end.version == first.version + 1 and api.RevokeCredential(revoke, timeout=5) == end
        assert api.GetCredentialStatus(status, timeout=5).credential.state == identity.CREDENTIAL_REVOKED
        rejected(api.RotateCredential, rotate, grpc.StatusCode.ABORTED)
        human_password(root, enrollment, api, secrets)
        fixture.run_sql(admin_db, "ALTER DATABASE p03_identity ALLOW_CONNECTIONS false; "
                        "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname='p03_identity';")
        try:
            # Losing the writer session invalidates its ownership lock. The runner exits
            # and the deployment supervisor must start a fresh owner.
            assert process.wait(timeout=12) == 1
            assert not fixture.probe_ready(int(state['probe_port']))
            refused = subprocess.run([binary, '--config', str(config)], capture_output=True, timeout=10)
            assert refused.returncode == 1 and b'ready=true' not in refused.stdout
        finally:
            fixture.run_sql(admin_db, 'ALTER DATABASE p03_identity ALLOW_CONNECTIONS true;')
    finally:
        stop()
    process, state, channel, api = start()
    try:
        status.context.CopyFrom(fixture.context())
        assert api.GetCredentialStatus(status, timeout=5).credential.state == identity.CREDENTIAL_REVOKED
    finally:
        stop()
    print('P04 real credential lifecycle, receipt replay across restart, and PG readiness passed')


if __name__ == '__main__':
    try:
        bootstrap.main(prepare=prepare, after_bootstrap=scenario)
    except Exception as error:
        last = traceback.extract_tb(error.__traceback__)[-1]
        code = error.code().name if isinstance(error, grpc.RpcError) else type(error).__name__
        print(f'P04 credential check failed at {Path(last.filename).name}:{last.lineno}: {code}',
              file=sys.stderr)
        sys.exit(1)
