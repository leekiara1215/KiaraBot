"""v1.1 descriptor/wire oracle only; never production authorization evidence."""
import json
from pathlib import Path
from kiarabot.v1 import identity_pb2, platform_pb2, policy_pb2


def check_host_gate_authorization_contract():
    request = identity_pb2.IdentityCheckHostGateAuthorizationRequest
    response = identity_pb2.IdentityCheckHostGateAuthorizationResponse
    assert {f.name: f.number for f in request.DESCRIPTOR.fields} == {
        'context': 1, 'host': 2, 'host_certificate_sha256': 3}
    assert {f.name: f.number for f in response.DESCRIPTOR.fields} == {
        'decision': 1, 'host_credential_version': 2, 'host_principal_version': 3}
    assert request.DESCRIPTOR.fields_by_name['host'].message_type.full_name == 'kiarabot.v1.ResourceRef'
    # No client-selected principal/method/permission/proof may enter the narrow check.
    assert not {'principal_id', 'subject_proof', 'full_method', 'permission'} & set(
        request.DESCRIPTOR.fields_by_name)
    service = identity_pb2.DESCRIPTOR.services_by_name['IdentityService']
    method = service.methods_by_name['CheckHostGateAuthorization']
    assert not method.client_streaming and not method.server_streaming
    options = method.GetOptions()
    assert options.Extensions[policy_pb2.required_permission] == policy_pb2.PERMISSION_IDENTITY_AUTHORIZATION_CHECK
    assert list(options.Extensions[policy_pb2.scope_kinds]) == [platform_pb2.RESOURCE_HOST]
    assert options.Extensions[policy_pb2.requires_online_authorization]
    assert not options.Extensions[policy_pb2.requires_idempotency]
    assert options.Extensions[policy_pb2.default_deadline_ms] == 500
    assert options.Extensions[policy_pb2.max_deadline_ms] == 1000
    # Existing delegation entry point retains its proof field/tag, distinct from the new call.
    assert identity_pb2.IdentityCheckAuthorizationRequest.DESCRIPTOR.fields_by_name['subject_proof'].number == 2
    value = request()
    value.host.kind = platform_pb2.RESOURCE_HOST
    value.host.id = 'host-independent-of-principal'
    value.host_certificate_sha256 = bytes(range(32))
    assert request.FromString(value.SerializeToString()) == value
    # Protobuf itself accepts these shapes: production must fail closed at its business boundary.
    def shape(v):
        return (v.HasField('host') and v.host.kind == platform_pb2.RESOURCE_HOST
                and bool(v.host.id) and len(v.host_certificate_sha256) == 32)
    assert shape(value)
    for kind in [platform_pb2.RESOURCE_KIND_UNSPECIFIED, platform_pb2.RESOURCE_PRINCIPAL,
                 platform_pb2.RESOURCE_INSTANCE]:
        bad = request.FromString(value.SerializeToString())
        bad.host.kind = kind
        assert not shape(bad)
    for count in [0, 31, 33]:
        bad = request.FromString(value.SerializeToString())
        bad.host_certificate_sha256 = b'x' * count
        assert not shape(bad)
    bad = request.FromString(value.SerializeToString())
    bad.host.ClearField('id')
    assert not shape(bad)
    root = Path(__file__).resolve().parents[2]
    spec = json.loads((root / 'proto/tests/target_spec.json').read_text())
    row = next(m for m in spec['methods'] if m['service'] == 'IdentityService'
               and m['method'] == 'CheckHostGateAuthorization')
    assert row['scope_kinds'] == ['RESOURCE_HOST'] and row['risk'] == 'online'
    assert row['caller'].startswith('current registered Runtime mTLS')
    assert row['permission'] == 'identity.authorization.check' and not row['mutating']
    assert row['status'] == 'not_implemented'
    print('Host gate v1.1 shape/policy/no-proof contract oracle passed (not production evidence)')


if __name__ == '__main__':
    check_host_gate_authorization_contract()
