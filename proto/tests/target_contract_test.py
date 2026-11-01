#!/usr/bin/env python3
"""Target descriptor/policy and Python -> C++ -> Python wire checks, not a server test."""
import importlib
import json
import subprocess
import sys
from pathlib import Path

from google.protobuf import descriptor_pb2, message_factory
from kiarabot.v1 import platform_pb2, policy_pb2, runtime_pb2

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
        assert rpc.options.Extensions[policy_pb2.requires_idempotency] == method['mutating']
        assert rpc.options.Extensions[policy_pb2.default_deadline_ms] == method['deadline_ms']
        assert rpc.options.Extensions[policy_pb2.max_deadline_ms] == method['max_deadline_ms']
        assert rpc.options.Extensions[policy_pb2.requires_online_authorization] == (method['risk'] == 'online')
        module = importlib.import_module('kiarabot.v1.' + Path(method['file']).stem + '_pb2')
        request = getattr(module, method['request'])()
        assert request.DESCRIPTOR.fields_by_name['context'].message_type.full_name == 'kiarabot.v1.RequestContext'
        assert 'operation_name' not in request.DESCRIPTOR.fields_by_name
        checked += 1
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
    print(f'{checked} typed RPC descriptors/policies and {count} Python/C++ message round-trips passed')


if __name__ == '__main__':
    main()
