"""Cross-contract refusal/compatibility checks, not production execution evidence."""
import json
from pathlib import Path

from kiarabot.v1 import host_pb2, observations_pb2, plugins_pb2, runtime_pb2
from capability_expansion_rules import host_cancel_allowed, host_operation_classification

ROOT = Path(__file__).resolve().parents[2]


def varint(value):
    out = bytearray()
    while value > 127:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def field(number, wire_type=2, payload=b''):
    tag = varint((number << 3) | wire_type)
    return tag + (varint(len(payload)) + payload if wire_type == 2 else varint(1))


def check_expansion_contract():
    # Decode real protobuf wire, preserving unknown fields; do not invent duplicate detection.
    request = plugins_pb2.HostRequest()
    assert host_operation_classification(request) == 'INVALID_ARGUMENT'
    known_tags = {f.number for f in request.DESCRIPTOR.oneofs_by_name['operation'].fields}
    future = next(number for number in range(10, 128) if number not in known_tags)
    unknown = plugins_pb2.HostRequest.FromString(field(future))
    assert unknown.WhichOneof('operation') is None
    assert unknown.SerializeToString() == field(future)
    assert host_operation_classification(unknown) == 'CAPABILITY_NOT_SUPPORTED'
    for raw in [field(128), field(200, 0)]:
        assert host_operation_classification(plugins_pb2.HostRequest.FromString(raw)) == 'INVALID_ARGUMENT'
    for raw in [field(future, 0), field(future) + field(future),
                field(future) + field(127), field(11) + field(future)]:
        assert host_operation_classification(plugins_pb2.HostRequest.FromString(raw)) == 'INVALID_ARGUMENT'
    parsed = plugins_pb2.HostRequest.FromString(field(11) + field(12))
    assert parsed.WhichOneof('operation') == 'storage_set'
    assert host_operation_classification(parsed) == 'KNOWN_OPERATION'
    parsed = plugins_pb2.HostRequest.FromString(field(12) + field(128))
    assert host_operation_classification(parsed) == 'KNOWN_OPERATION'
    # Adding an opt-in field does not reinterpret legacy request bytes or make it present.
    for message in [runtime_pb2.RuntimeBindAdapterRequest, runtime_pb2.RuntimeCreateInstanceRequest]:
        legacy = message.FromString(b'')
        assert legacy.bootstrap_recipient_id == ''
        assert legacy.SerializeToString() == b''
    # No-secret metadata can never serialize a bootstrap/digest/path property by accident.
    assert set(runtime_pb2.BootstrapDelivery.DESCRIPTOR.fields_by_name) == {
        'delivery_id', 'resource', 'recipient_id', 'credential_version', 'state', 'operation', 'observed_at'}
    assert runtime_pb2.RuntimeReissueBootstrapResponse.DESCRIPTOR.fields_by_name['bootstrap_credential'].number == 2
    assert host_pb2.HostCancelOperationRequest.DESCRIPTOR.fields_by_name['expected_version'].has_presence
    for closed, requested, expected in [(False, False, True), (True, False, False),
                                       (False, True, False), (True, True, False)]:
        assert host_cancel_allowed(admission_changed=closed, runtime_gate_requested=requested) == expected
    spec = json.loads((ROOT / 'proto/tests/target_spec.json').read_text())
    methods = {(m['service'], m['method']): m for m in spec['methods']}
    expected = {
        'ReissueAdapterBootstrap': ('account.manage', ['RESOURCE_ACCOUNT']),
        'ReportAdapterBootstrapDelivery': ('account.manage', ['RESOURCE_ACCOUNT']),
        'ReissuePluginBootstrap': ('plugin.manage', ['RESOURCE_INSTANCE']),
        'ReportPluginBootstrapDelivery': ('plugin.manage', ['RESOURCE_INSTANCE']),
        'GetBootstrapDelivery': ('resource.read', ['RESOURCE_ACCOUNT', 'RESOURCE_INSTANCE']),
        'SetHostAdmissionGate': ('host.execute', ['RESOURCE_HOST'])}
    for name, (permission, kinds) in expected.items():
        method = methods['RuntimeService', name]
        assert (method['permission'], method['scope_kinds']) == (permission, kinds)
        assert method['risk'] == 'online'
    for name in ['GetMaintenance', 'GetOperation', 'ListOperationChanges',
                 'BeginMaintenance', 'ResumeAdmission', 'CancelOperation']:
        method = methods['HostManagementService', name]
        assert method['scope_kinds'] == ['RESOURCE_HOST']
        assert method['permission'] == ('host.read' if name in {'GetMaintenance', 'GetOperation',
                                                              'ListOperationChanges'} else 'host.execute')
    for service in ['RuntimeService', 'SchedulerService', 'ModelService', 'HostManagementService']:
        method = methods[service, 'ListOperationChanges']
        assert method['request'] == 'OperationChangesRequest'
        assert method['response'] == 'OperationChangesResponse'
        assert method['deadline_ms'] == method['max_deadline_ms'] == 500
        assert not method['mutating'] and not method['requires_idempotency_key']
    assert observations_pb2.OperationChangesRequest.DESCRIPTOR.fields_by_name['cursor'].has_presence
    print('Host raw-wire classification, opt-in bootstrap, exact permission/scope and owner changes contract checks passed')


if __name__ == '__main__':
    check_expansion_contract()
