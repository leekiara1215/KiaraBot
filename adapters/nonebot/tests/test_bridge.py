"""Unit tests for target bridge value types, authorization lifecycle and deployment files."""
import os
import time
import uuid

import grpc
import pytest
from kiarabot.v1 import adapters_pb2 as adapters, common_pb2 as common, platform_pb2 as platform
from kiarabot_adapter_nonebot.bridge import (MAX_AUTHORIZATION_LIFETIME_SECONDS, BridgeConfig,
                                             BridgeFailure, ConnectionAuthorization, Fence,
                                             Registration, authorization_result, check_response_fence,
                                             error_code, frame, request_context, shutdown_notice,
                                             timestamp)

PEM = b'-----BEGIN CERTIFICATE-----\ntest-only\n-----END CERTIFICATE-----\n'


def bootstrap_file(tmp_path, credential='a' * 64, mode=0o600):
    path = tmp_path / 'bootstrap'
    path.write_text(credential + '\n')
    path.chmod(mode)
    return path


def ca_file(tmp_path, content=PEM, name='ca.pem'):
    path = tmp_path / name
    path.write_bytes(content)
    return path


def config(tmp_path, **overrides):
    values = dict(endpoint='127.0.0.1:50051', adapter_instance_id=str(uuid.uuid4()),
                  account_id='123456')
    if 'ca_file' not in overrides:
        values['ca_file'] = str(ca_file(tmp_path))
    if 'bootstrap_file' not in overrides:
        values['bootstrap_file'] = str(bootstrap_file(tmp_path))
    values.update(overrides)
    return BridgeConfig(**values)


def fence_proto(instance=None, execution=1, connection=1, epoch='runtime-epoch-1'):
    return platform.Fence(instance_id=instance or str(uuid.uuid4()),
                          execution_generation=execution, connection_generation=connection,
                          runtime_epoch=epoch)


def authorization_proto(fence, *, proof=b'p' * 32, refresh=b'r' * 32, generation=1,
                        issued=None, lifetime=56.0, principal=None):
    now = time.time() if issued is None else issued
    return platform.ConnectionAuthorization(
        fence=fence, proof=proof, issued_at=timestamp(now), expires_at=timestamp(now + lifetime),
        refresh_credential=refresh, refresh_generation=generation,
        principal_id=principal or fence.instance_id, credential_version=1,
        authorization_version=1)


def registration_response(instance, *, protocol='kiarabot.v1', heartbeat=5, lifetime=56.0):
    fence = fence_proto(instance)
    return adapters.RegisterAdapterResponse(
        accepted=True, assigned_instance_id=instance, required_protocol_version=protocol,
        heartbeat_interval_seconds=heartbeat, fence=fence,
        authorization=authorization_proto(fence, lifetime=lifetime),
        effective_capabilities=[common.Capability(name='event.message_received', version='1',
                                                  supported=True)])


def test_deployment_files_and_environment(tmp_path, monkeypatch):
    base = config(tmp_path)
    assert base.adapter_id == base.adapter_instance_id
    assert base.metadata() == (('authorization', 'Bearer ' + 'a' * 64),)
    assert 'a' * 64 not in repr(base)
    context = request_context(2.0)
    assert context.request_id != context.trace_id
    assert context.deadline.seconds > time.time()
    assert context.HasField('deadline')

    environ = dict(BRIDGE_ENDPOINT='127.0.0.1:50051', BRIDGE_CA_FILE=str(ca_file(tmp_path)),
                   BRIDGE_BOOTSTRAP_CREDENTIAL_FILE=str(bootstrap_file(tmp_path)),
                   ADAPTER_INSTANCE_ID=str(uuid.uuid4()), ACCOUNT_ID='123456')
    from_environment = BridgeConfig.from_environment(environ)
    assert from_environment.endpoint == '127.0.0.1:50051'
    for missing in ('BRIDGE_ENDPOINT', 'BRIDGE_CA_FILE', 'BRIDGE_BOOTSTRAP_CREDENTIAL_FILE',
                    'ADAPTER_INSTANCE_ID', 'ACCOUNT_ID'):
        invalid = {key: value for key, value in environ.items() if key != missing}
        with pytest.raises(BridgeFailure) as failure:
            BridgeConfig.from_environment(invalid)
        assert failure.value.code == common.INVALID_ARGUMENT
        assert failure.value.fatal is False


@pytest.mark.parametrize('mutate', [
    lambda values: values.update(endpoint='example.com'),
    lambda values: values.update(endpoint='127.0.0.1:0'),
    lambda values: values.update(endpoint='user@127.0.0.1:50051'),
    lambda values: values.update(adapter_instance_id='not-a-uuid'),
    lambda values: values.update(server_name='bad name'),
    lambda values: values.update(timeout=0),
])
def test_invalid_bridge_configuration(tmp_path, mutate):
    values = dict(endpoint='127.0.0.1:50051', ca_file=str(ca_file(tmp_path)),
                  bootstrap_file=str(bootstrap_file(tmp_path)),
                  adapter_instance_id=str(uuid.uuid4()), account_id='123456')
    mutate(values)
    with pytest.raises(BridgeFailure) as failure:
        BridgeConfig(**values)
    assert failure.value.code == common.INVALID_ARGUMENT


def test_untrusted_ca_and_secret_file_rules(tmp_path):
    with pytest.raises(BridgeFailure):
        config(tmp_path, ca_file=str(ca_file(tmp_path, b'not a certificate')))
    with pytest.raises(BridgeFailure):
        config(tmp_path, ca_file=str(tmp_path / 'missing.pem'))
    with pytest.raises(BridgeFailure):
        config(tmp_path, bootstrap_file=str(bootstrap_file(tmp_path, mode=0o644)))
    with pytest.raises(BridgeFailure):
        config(tmp_path, bootstrap_file=str(bootstrap_file(tmp_path, credential='short')))
    with pytest.raises(BridgeFailure):
        config(tmp_path, bootstrap_file=str(bootstrap_file(tmp_path, credential='a' * 4097)))
    target = tmp_path / 'victim.pem'
    target.write_bytes(PEM)
    link = tmp_path / 'link.pem'
    link.symlink_to(target)
    with pytest.raises(BridgeFailure):
        config(tmp_path, ca_file=str(link))
    with pytest.raises(BridgeFailure):
        config(tmp_path, bootstrap_file=str(link))


def test_fence_roundtrip_and_validation():
    instance = str(uuid.uuid4())
    value = Fence(instance, 2, 3, 'epoch-1')
    assert Fence.from_proto(value.to_proto()) == value
    with pytest.raises(BridgeFailure):
        Fence('', 1, 1, 'epoch')
    with pytest.raises(BridgeFailure):
        Fence(instance, 0, 1, 'epoch')
    with pytest.raises(BridgeFailure):
        Fence(instance, 1, 1, '')
    missing = platform.Fence(instance_id=instance, runtime_epoch='epoch')
    with pytest.raises(BridgeFailure):
        Fence.from_proto(missing)


def test_connection_authorization_validation_and_repr():
    instance = str(uuid.uuid4())
    fence = Fence(instance, 1, 1, 'epoch')
    proto = authorization_proto(fence.to_proto())
    value = ConnectionAuthorization.from_proto(proto, expected_fence=fence)
    assert value.principal_id == instance
    assert value.valid(time.time())
    assert value.refresh_lead == 10.0
    assert 'p' * 32 not in repr(value) and 'r' * 32 not in repr(value)
    short = ConnectionAuthorization.from_proto(
        authorization_proto(fence.to_proto(), lifetime=2.0), expected_fence=fence)
    assert short.refresh_lead == 1.0
    now = time.time()
    assert short.refresh_due(now + 1.2) and not short.refresh_due(now)
    with pytest.raises(BridgeFailure) as failure:
        ConnectionAuthorization.from_proto(
            authorization_proto(fence.to_proto(), lifetime=MAX_AUTHORIZATION_LIFETIME_SECONDS + 1),
            expected_fence=fence)
    assert failure.value.code == common.INVALID_ARGUMENT
    with pytest.raises(BridgeFailure) as failure:
        ConnectionAuthorization.from_proto(authorization_proto(fence.to_proto(), proof=b'x' * 8),
                                           expected_fence=fence)
    assert failure.value.code == common.INVALID_ARGUMENT
    other = Fence(str(uuid.uuid4()), 1, 1, 'epoch')
    with pytest.raises(BridgeFailure) as failure:
        ConnectionAuthorization.from_proto(authorization_proto(fence.to_proto()),
                                           expected_fence=other)
    assert failure.value.code == common.UNAUTHENTICATED


def test_registration_validation():
    instance = str(uuid.uuid4())
    registration = Registration.from_response(registration_response(instance),
                                              expected_instance=instance)
    assert registration.instance_id == instance
    assert registration.heartbeat_interval == 5.0
    assert registration.effective_capabilities == frozenset({'event.message_received'})
    assert registration.authorization.fence == registration.fence
    with pytest.raises(BridgeFailure):
        Registration.from_response(registration_response(instance, protocol='kiarabot.v0'),
                                   expected_instance=instance)
    with pytest.raises(BridgeFailure) as failure:
        Registration.from_response(registration_response(instance), expected_instance=str(uuid.uuid4()))
    assert failure.value.code == common.PERMISSION_DENIED
    with pytest.raises(BridgeFailure):
        Registration.from_response(registration_response(instance, heartbeat=0),
                                   expected_instance=instance)
    response = registration_response(instance)
    response.ClearField('authorization')
    with pytest.raises(BridgeFailure):
        Registration.from_response(response, expected_instance=instance)
    response = registration_response(instance)
    response.heartbeat_interval_seconds = 0
    with pytest.raises(BridgeFailure):
        Registration.from_response(response, expected_instance=instance)
    rejected = adapters.RegisterAdapterResponse(accepted=False, error=common.Error(
        code=common.PERMISSION_DENIED, message='PERMISSION_DENIED'))
    with pytest.raises(BridgeFailure):
        Registration.from_response(rejected, expected_instance=instance)


def test_frame_and_response_helpers():
    instance = str(uuid.uuid4())
    fence = Fence(instance, 1, 2, 'epoch')
    request = frame(fence, b'q' * 32, heartbeat=adapters.Heartbeat(instance_id=instance))
    assert request.HasField('fence') and request.connection_proof == b'q' * 32
    assert request.WhichOneof('body') == 'heartbeat'
    response = adapters.CoreToAdapter(fence=fence.to_proto(),
                                      heartbeat_ack=adapters.HeartbeatAck(received_at_unix_ms=1))
    check_response_fence(response, fence)
    with pytest.raises(BridgeFailure) as failure:
        check_response_fence(adapters.CoreToAdapter(heartbeat_ack=adapters.HeartbeatAck()), fence)
    assert failure.value.code == common.UNAUTHENTICATED
    other = Fence(instance, 1, 3, 'epoch')
    with pytest.raises(BridgeFailure) as failure:
        check_response_fence(adapters.CoreToAdapter(fence=other.to_proto(),
                                                    heartbeat_ack=adapters.HeartbeatAck()), fence)
    assert failure.value.code == common.UNAUTHENTICATED

    refreshed = platform.ConnectionAuthorizationResult(
        request_id='request-1', authorization=authorization_proto(fence.to_proto()))
    result = adapters.CoreToAdapter(fence=fence.to_proto(), authorization_result=refreshed)
    request_id, authorization, code = authorization_result(result)
    assert request_id == 'request-1' and authorization.proof and code is None
    failed = adapters.CoreToAdapter(fence=fence.to_proto(), authorization_result=(
        platform.ConnectionAuthorizationResult(
            request_id='request-2', error=common.Error(code=common.CONFLICT,
                                                      message='CONFLICT'))))
    assert authorization_result(failed) == ('request-2', None, common.CONFLICT)
    notice = adapters.CoreToAdapter(fence=fence.to_proto(), shutdown=common.ShutdownNotice(
        reason='drain', drain_timeout_ms=1500))
    assert shutdown_notice(notice) == 1500
    with pytest.raises(BridgeFailure):
        shutdown_notice(adapters.CoreToAdapter(shutdown=common.ShutdownNotice(
            drain_timeout_ms=600001)))


def test_stable_error_mapping():
    with pytest.raises(BridgeFailure) as failure:
        error_code(common.Error())
    assert failure.value.code == common.INTERNAL
    with pytest.raises(BridgeFailure) as failure:
        error_code(common.Error(code=99))
    assert failure.value.code == common.INTERNAL
    error = common.Error(code=common.UPSTREAM_UNAVAILABLE, message='UPSTREAM_UNAVAILABLE')
    assert error_code(error) == common.UPSTREAM_UNAVAILABLE


def test_tls_channel_options_and_registration_request(tmp_path):
    value = config(tmp_path, server_name='runtime.internal')
    options = dict(value.channel_options())
    assert options['grpc.ssl_target_name_override'] == 'runtime.internal'
    credentials = value.tls_credentials()
    assert isinstance(credentials, grpc.ChannelCredentials)
    request = value.registration_request(['event.message_received'])
    assert request.adapter_instance_id == value.adapter_instance_id
    assert request.protocol_version == 'kiarabot.v1'
    assert request.context.request_id
    assert request.accounts[0].account_id == '123456'
    assert [item.name for item in request.capabilities] == ['event.message_received']
    default_options = dict(config(tmp_path).channel_options())
    assert 'grpc.ssl_target_name_override' not in default_options
