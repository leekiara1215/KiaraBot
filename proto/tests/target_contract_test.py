#!/usr/bin/env python3
"""Target descriptor/policy and Python -> C++ -> Python wire checks, not a server test."""
import importlib
import json
import subprocess
import sys
from pathlib import Path

from google.protobuf import descriptor_pb2, message_factory
from google.protobuf.json_format import ParseDict
from kiarabot.v1 import platform_pb2, policy_pb2, runtime_pb2, identity_pb2
from resource_query_rules import runtime_query_valid
from identity_projection_rules import allows, validate_current_projection, session_state, access_seconds_remaining, assignment_command_valid

ROOT = Path(__file__).resolve().parents[2]
SPEC = json.loads((ROOT / 'proto/tests/target_spec.json').read_text())


def validate_fence(fence):
    """Executable target fixture oracle; production adapters must implement this check."""
    return (bool(fence.instance_id) and bool(fence.runtime_epoch)
            and fence.HasField('execution_generation') and fence.execution_generation > 0
            and fence.HasField('connection_generation') and fence.connection_generation > 0)


def populate(message, depth=0):
    if depth > 3:
        return
    for field in message.DESCRIPTOR.fields:
        if field.type == field.TYPE_MESSAGE:
            if field.message_type.GetOptions().map_entry:
                continue
            child = getattr(message, field.name).add() if field.is_repeated else getattr(message, field.name)
            child.SetInParent()
            populate(child, depth + 1)
            continue
        value = ('value' if field.type == field.TYPE_STRING else b'bytes'
                 if field.type == field.TYPE_BYTES else True if field.type == field.TYPE_BOOL
                 else field.enum_type.values[-1].number if field.type == field.TYPE_ENUM else 1)
        if field.is_repeated:
            getattr(message, field.name).append(value)
        else:
            setattr(message, field.name, value)


def main():
    descriptor = descriptor_pb2.FileDescriptorSet.FromString(Path(sys.argv[1]).read_bytes())
    cpp = sys.argv[2]
    files = {f.name: f for f in descriptor.file if f.package == 'kiarabot.v1'}
    expected_methods = {(m['service'], m['method']) for m in SPEC['methods']}
    actual_methods = {(service.name, rpc.name) for file in files.values()
                      for service in file.service if service.name in
                      {'IdentityService', 'RuntimeService', 'HostService', 'SchedulerService', 'ModelService'}
                      for rpc in service.method}
    assert actual_methods == expected_methods, (actual_methods - expected_methods, expected_methods - actual_methods)
    checked = 0
    for method in SPEC['methods']:
        file = files['kiarabot/v1/' + method['file']]
        service = next(s for s in file.service if s.name == method['service'])
        rpc = next(m for m in service.method if m.name == method['method'])
        assert rpc.input_type == '.kiarabot.v1.' + method['request']
        assert rpc.output_type == '.kiarabot.v1.' + method['response']
        assert rpc.server_streaming == method['server_streaming']
        assert rpc.options.Extensions[policy_pb2.required_permission] == getattr(
            policy_pb2, 'PERMISSION_' + method['permission'].upper().replace('.', '_'))
        assert rpc.options.Extensions[policy_pb2.requires_idempotency] == method['requires_idempotency_key']
        assert rpc.options.Extensions[policy_pb2.default_deadline_ms] == method['deadline_ms']
        assert rpc.options.Extensions[policy_pb2.max_deadline_ms] == method['max_deadline_ms']
        assert rpc.options.Extensions[policy_pb2.requires_online_authorization] == (method['risk'] == 'online')
        module = importlib.import_module('kiarabot.v1.' + Path(method['file']).stem + '_pb2')
        request = getattr(module, method['request'])()
        assert request.DESCRIPTOR.fields_by_name['context'].message_type.full_name == 'kiarabot.v1.RequestContext'
        assert 'operation_name' not in request.DESCRIPTOR.fields_by_name
        checked += 1
    # Typed Runtime reads never treat an absent/empty/wrong-kind catalog as global scope.
    resource_id = '11111111-1111-4111-8111-111111111111'
    kinds = {'Account': platform_pb2.RESOURCE_ACCOUNT, 'Installation': platform_pb2.RESOURCE_INSTALLATION,
             'Instance': platform_pb2.RESOURCE_INSTANCE, 'Adapter': platform_pb2.RESOURCE_ADAPTER,
             'Host': platform_pb2.RESOURCE_HOST}
    for noun, kind in kinds.items():
        request = getattr(runtime_pb2, 'RuntimeGet' + noun + 'Request')()
        assert not runtime_query_valid(request)
        request.resource.kind = kind
        request.resource.id = resource_id
        assert runtime_query_valid(request)
        request.resource.kind = platform_pb2.RESOURCE_CATALOG
        assert not runtime_query_valid(request)
        listed = getattr(runtime_pb2, 'RuntimeList' + noun + 'sRequest')()
        assert not runtime_query_valid(listed)
        listed.catalog.kind = platform_pb2.RESOURCE_CATALOG
        listed.catalog.id = resource_id
        listed.page.page_size = 20
        assert runtime_query_valid(listed)
        listed.catalog.id = 'all'
        assert not runtime_query_valid(listed)
        listed.catalog.id = resource_id
        listed.catalog.kind = kind
        assert not runtime_query_valid(listed)
        listed.catalog.kind = platform_pb2.RESOURCE_CATALOG
        listed.page.page_size = 101
        assert not runtime_query_valid(listed)
    config_query = runtime_pb2.RuntimeGetInstanceConfigRequest()
    config_query.resource.kind = platform_pb2.RESOURCE_INSTANCE
    config_query.resource.id = resource_id
    assert runtime_query_valid(config_query)
    # Role bindings stay attached to their exact resource; display unions never authorize.
    known_permissions = set(policy_pb2.Permission.values()) - {0}
    roles = {platform_pb2.ROLE_VIEWER: {policy_pb2.PERMISSION_RESOURCE_READ},
             platform_pb2.ROLE_ADMIN: {policy_pb2.PERMISSION_ACCOUNT_MANAGE, policy_pb2.PERMISSION_RESOURCE_READ,
                                      policy_pb2.PERMISSION_ACTION_INSPECT}}
    sensitive = {policy_pb2.PERMISSION_MESSAGE_READ, policy_pb2.PERMISSION_ACTION_INSPECT}
    projection = identity_pb2.IdentityValidateSessionResponse(authorization_version=1)
    projection.checked_at.FromJsonString('2026-11-01T00:10:00Z')
    projection.principal.principal_id = resource_id
    projection.principal.version = 1
    projection.principal.principal_kind = 'human'
    projection.session.session_id = resource_id
    projection.session.principal_id = resource_id
    projection.session.rotation = 1
    projection.session.created_at.FromJsonString('2026-11-01T00:00:00Z')
    projection.session.expires_at.FromJsonString('2026-11-01T00:15:00Z')
    projection.principal.roles.extend([platform_pb2.ROLE_VIEWER, platform_pb2.ROLE_ADMIN])
    projection.principal.display_permissions.extend([policy_pb2.PERMISSION_RESOURCE_READ, policy_pb2.PERMISSION_ACCOUNT_MANAGE])
    for role, identifier in [(platform_pb2.ROLE_VIEWER, resource_id), (platform_pb2.ROLE_ADMIN, '22222222-2222-4222-8222-222222222222')]:
        assignment = projection.assignments.add(principal_id=resource_id, role=role, assignment_id=identifier, version=1)
        assignment.scope.resources.add(kind=platform_pb2.RESOURCE_ACCOUNT, id=identifier)
    validate_current_projection(projection, roles, known_permissions, sensitive)
    first_resource = projection.assignments[0].scope.resources[0]
    assert not allows(projection.assignments, first_resource, policy_pb2.PERMISSION_ACCOUNT_MANAGE, roles, known_permissions, sensitive)
    assert not allows(projection.assignments, projection.assignments[1].scope.resources[0], policy_pb2.PERMISSION_ACTION_INSPECT, roles, known_permissions, sensitive)
    explicit = projection.assignments.add(principal_id=resource_id, assignment_id='44444444-4444-4444-8444-444444444444', version=1)
    explicit.permissions.append(policy_pb2.PERMISSION_MESSAGE_READ)
    explicit.scope.resources.add().CopyFrom(first_resource)
    projection.principal.display_permissions.append(policy_pb2.PERMISSION_MESSAGE_READ)
    validate_current_projection(projection, roles, known_permissions, sensitive)
    assert allows(projection.assignments, first_resource, policy_pb2.PERMISSION_MESSAGE_READ, roles, known_permissions, sensitive)
    explicit.role = platform_pb2.ROLE_ADMIN
    try: validate_current_projection(projection, roles, known_permissions, sensitive)
    except ValueError: pass
    else: raise AssertionError('mixed role/permission basis accepted')
    explicit.role = 0
    command = identity_pb2.IdentitySetAssignmentRequest()
    command.assignment.principal_id = resource_id
    command.assignment.role = platform_pb2.ROLE_VIEWER
    command.assignment.scope.resources.add().CopyFrom(first_resource)
    assert assignment_command_valid(command, roles, known_permissions, sensitive)
    command.expected_version = 1
    assert not assignment_command_valid(command, roles, known_permissions, sensitive)
    command.assignment_id = resource_id
    assert assignment_command_valid(command, roles, known_permissions, sensitive)
    command.expected_version = 0
    assert not assignment_command_valid(command, roles, known_permissions, sensitive)
    projection.ClearField('checked_at')
    try: validate_current_projection(projection, roles, known_permissions, sensitive)
    except ValueError: pass
    else: raise AssertionError('missing current Identity time accepted')
    session = identity_pb2.SessionRecord(rotation=1)
    session.created_at.FromJsonString('2026-11-01T00:00:00Z')
    session.expires_at.FromJsonString('2026-11-01T00:15:00Z')
    identity_checked_at = session.created_at.__class__()
    identity_checked_at.FromJsonString('2026-11-01T00:10:00Z')
    assert session_state(session, identity_checked_at) == 'ACTIVE'
    identity_checked_at.FromJsonString('2026-11-01T00:15:00Z')
    assert session_state(session, identity_checked_at) == 'EXPIRED'
    session.revoked = True
    assert session_state(session, identity_checked_at) == 'REVOKED'
    session.ClearField('created_at')
    try: session_state(session, identity_checked_at)
    except ValueError: pass
    else: raise AssertionError('missing creation time accepted')
    tokens = identity_pb2.TokenPair()
    tokens.access_expires_at.FromJsonString('2026-11-01T00:15:00Z')
    identity_checked_at.FromJsonString('2026-11-01T00:10:00Z')
    assert access_seconds_remaining(tokens, identity_checked_at) == 300
    # Missing/zero generation cannot be silently admitted by the target fixture.
    fence = platform_pb2.Fence(instance_id='instance', runtime_epoch='epoch')
    assert not validate_fence(fence)
    fence.execution_generation = 1
    fence.connection_generation = 0
    assert not validate_fence(fence)
    fence.connection_generation = 1
    assert validate_fence(fence)
    fence.runtime_epoch = ''
    assert not validate_fence(fence)
    condition = runtime_pb2.StorageCondition(absent=True)
    assert condition.WhichOneof('expected') == 'absent'
    condition.version = 2
    assert condition.WhichOneof('expected') == 'version'
    # Normal, rejected, timeout and dependency-failure fixtures retain acceptance/recovery.
    for code, acceptance, recovery in [(0, 2, 4), (3, 1, 3), (8, 3, 4), (9, 3, 4)]:
        failure = platform_pb2.RpcFailure()
        failure.error.code = code
        failure.context.acceptance = acceptance
        failure.context.recovery = recovery
        assert platform_pb2.RpcFailure.FromString(failure.SerializeToString()) == failure
    connection_fence = platform_pb2.Fence(instance_id='instance', execution_generation=1, connection_generation=1, runtime_epoch='epoch')
    # Connection proof secrets and stream/fence binding are mandatory, with no fallback.
    from connection_authorization_rules import valid_bundle, frame_allowed, refresh, registration_valid, refresh_frame_consistent
    from kiarabot.v1 import adapters_pb2, plugins_pb2
    from copy import deepcopy
    connection = platform_pb2.ConnectionAuthorization(
        proof=b'p' * 32, refresh_credential=b'r' * 32, refresh_generation=1,
        principal_id='principal', credential_version=1, authorization_version=1)
    connection.fence.CopyFrom(connection_fence)
    connection.issued_at.FromJsonString('2026-11-01T00:00:00Z')
    connection.expires_at.FromJsonString('2026-11-01T00:00:56Z')
    assert valid_bundle(connection)
    too_long = deepcopy(connection)
    too_long.expires_at.seconds += 1
    assert not valid_bundle(too_long)
    too_long.expires_at.seconds += 3
    assert not valid_bundle(too_long)  # 60 seconds is the total budget, never nominal lifetime.
    empty_proof = deepcopy(connection)
    empty_proof.proof = b''
    assert not valid_bundle(empty_proof)
    args = dict(stream='trusted-stream', trusted_stream='trusted-stream', utc_ns=connection.issued_at.ToNanoseconds()+1,
                monotonic_ns=1, monotonic_deadline=56_000_000_000, credential_version=1)
    assert frame_allowed(connection, connection_fence, connection.proof, **args)
    for override in [dict(stream='another-stream'), dict(revoked=True), dict(gate=False), dict(credential_version=2),
                     dict(utc_ns=connection.expires_at.ToNanoseconds()), dict(monotonic_ns=56_000_000_000)]:
        assert not frame_allowed(connection, connection_fence, connection.proof, **dict(args, **override))
    assert not frame_allowed(connection, connection_fence, b'', **args)
    stale_fence = deepcopy(connection_fence)
    stale_fence.connection_generation += 1
    assert not frame_allowed(connection, stale_fence, connection.proof, **args)
    renewal = platform_pb2.RefreshConnectionAuthorization(request_id='refresh', proof=connection.proof,
        refresh_credential=connection.refresh_credential, refresh_generation=1)
    renewal.fence.CopyFrom(connection_fence)
    replacement = deepcopy(connection)
    replacement.proof = b'q' * 32
    replacement.refresh_credential = b's' * 32
    replacement.refresh_generation = 2
    replacement.authorization_version = 2
    wrong_generation = deepcopy(renewal)
    wrong_generation.refresh_generation = 2
    assert refresh(connection, wrong_generation, replacement, online=True, allowed=True, **args) == 'CONFLICT'
    assert refresh(connection, renewal, replacement, online=False, allowed=True, **args) == 'UPSTREAM_UNAVAILABLE'
    assert connection.refresh_generation == 1
    assert refresh(connection, renewal, replacement, online=True, allowed=False, **args) == 'PERMISSION_DENIED'
    assert refresh(connection, renewal, replacement, online=True, allowed=True,
                   **dict(args, utc_ns=connection.expires_at.ToNanoseconds())) == 'UNAUTHENTICATED'
    lease = platform_pb2.DeliveryAttempt()
    lease.lease_until.FromJsonString('2026-11-01T00:00:30Z')
    previous_lease = lease.SerializeToString()
    assert refresh(connection, renewal, replacement, online=True, allowed=True, **args) == 'OK'
    assert refresh(connection, renewal, replacement, online=True, allowed=True, **args) == 'UNAUTHENTICATED'
    assert not frame_allowed(connection, connection_fence, renewal.proof, **args)
    assert lease.SerializeToString() == previous_lease
    # Lost refresh response: close old stream, bootstrap anew with a new connection generation.
    new_connection = deepcopy(replacement)
    new_connection.fence.connection_generation += 1
    new_connection.proof = b'n' * 32
    assert not frame_allowed(new_connection, connection_fence, replacement.proof, **dict(args, stream='new', trusted_stream='new'))
    for incoming in [adapters_pb2.AdapterToCore, plugins_pb2.PluginToCore]:
        frame = incoming(connection_proof=connection.proof)
        frame.fence.CopyFrom(connection_fence)
        frame.refresh_authorization.CopyFrom(renewal)
        assert frame.WhichOneof('body') == 'refresh_authorization'
        frame.refresh_authorization.proof = connection.proof
        assert refresh_frame_consistent(frame)
        frame.connection_proof = b''
        assert not refresh_frame_consistent(frame)
    for response_type in [adapters_pb2.RegisterAdapterResponse, plugins_pb2.RegisterPluginResponse]:
        response = response_type(accepted=True)
        assert not registration_valid(response)
        response.fence.CopyFrom(connection.fence)
        response.authorization.CopyFrom(connection)
        assert registration_valid(response)
        response.fence.connection_generation += 1
        assert not registration_valid(response)
    for outgoing in [adapters_pb2.CoreToAdapter, plugins_pb2.CoreToPlugin]:
        frame = outgoing()
        frame.authorization_result.request_id = 'refresh'
        frame.authorization_result.authorization.CopyFrom(connection)
        assert frame.WhichOneof('body') == 'authorization_result'
        frame.authorization_result.error.code = 1
        assert frame.authorization_result.WhichOneof('outcome') == 'error'
    goldens = json.loads((ROOT / 'proto/tests/target_wire_goldens.json').read_text())
    assert goldens['schema_version'] == 1
    for fixture in goldens['fixtures']:
        module = importlib.import_module('kiarabot.v1.' + Path(fixture['file']).stem + '_pb2')
        value = ParseDict(fixture['json'], getattr(module, fixture['message'])())
        assert value.SerializeToString(deterministic=True).hex() == fixture['wire_hex'], fixture['message']
        returned = subprocess.run([cpp, value.DESCRIPTOR.full_name], input=bytes.fromhex(fixture['wire_hex']),
                                  stdout=subprocess.PIPE, check=True, cwd=ROOT).stdout
        assert value.__class__.FromString(returned) == value
    count = 0
    for filename in files:
        module = importlib.import_module('kiarabot.v1.' + Path(filename).stem + '_pb2')
        for desc in module.DESCRIPTOR.message_types_by_name.values():
            instance = message_factory.GetMessageClass(desc)()
            populate(instance)
            binary = subprocess.run([cpp, desc.full_name], input=instance.SerializeToString(),
                                    stdout=subprocess.PIPE, check=True, cwd=ROOT).stdout
            recovered = instance.__class__.FromString(binary)
            assert recovered == instance, desc.full_name
            count += 1
    print(f'{checked} typed RPC descriptors/policies and {count} Python/C++ message round-trips and {len(goldens["fixtures"])} wire goldens passed')


if __name__ == '__main__':
    main()
