"""P05 existing account and adapter operations, exercised over production Runtime + Identity mTLS."""
import time
import uuid

import grpc
from kiarabot.v1 import platform_pb2 as platform, policy_pb2 as policy, runtime_pb2 as runtime

import runtime_identity_e2e_test as fixture


def prepare(enrollment, account):
    enrollment['administrator_assignments'].append({
        'assignment_id': str(uuid.uuid4()), 'permissions': ['account.read', 'account.manage'],
        'scope': [{'kind': 'RESOURCE_ACCOUNT', 'id': account}]})
    adapter = str(uuid.uuid4())
    enrollment['administrator_assignments'].append({
        'assignment_id': str(uuid.uuid4()), 'permissions': ['adapter.read'],
        'scope': [{'kind': 'RESOURCE_ADAPTER', 'id': adapter}]})
    return adapter


def rejected(call, request, code):
    try:
        call(request, timeout=5)
    except grpc.RpcError as error:
        assert error.code() == code, f'{call} expected {code}, got {error.code()}'
    else:
        raise AssertionError(f'{call} unexpectedly allowed')


def check(api, issue, database, account, adapter):
    ctx = fixture.context
    instance = str(uuid.uuid4())
    # Account and adapter registration predate this test. Binding, unbinding, updating and
    # disabling are performed solely through the real owner RPCs. No real platform is contacted.
    fixture.run_sql(database, f"""
        INSERT INTO kiarabot.adapters
          (id,implementation,implementation_version,protocol_version,declared_capabilities,
           effective_capabilities,version,state,registered_at,heartbeat_at,online_until,
           instance,runtime_epoch,connection_id,connection_generation)
        VALUES ('{adapter}','p05-local','1','kiarabot.v1','','',1,0,100,101,102,
                '{instance}','{uuid.uuid4()}','{uuid.uuid4()}',1);""")
    account_ref = platform.ResourceRef(kind=platform.RESOURCE_ACCOUNT, id=account)
    adapter_ref = platform.ResourceRef(kind=platform.RESOURCE_ADAPTER, id=adapter)
    read = issue([policy.PERMISSION_ACCOUNT_READ], [account_ref]).delegation.proof
    manage = issue([policy.PERMISSION_ACCOUNT_MANAGE], [account_ref]).delegation.proof
    adapter_read = issue([policy.PERMISSION_ADAPTER_READ], [adapter_ref]).delegation.proof

    def get_account():
        before = time.time_ns()
        reply = api.GetAccount(runtime.RuntimeGetAccountRequest(
            context=ctx(20, read), resource=account_ref), timeout=5)
        assert before <= reply.observed_at.ToNanoseconds() <= time.time_ns()
        return reply.account

    def update(proof, version, name):
        return runtime.RuntimeUpdateAccountRequest(context=ctx(20, proof), account_id=account,
                                                  expected_version=version, display_name=name)

    def disable(version):
        return runtime.RuntimeDisableAccountRequest(context=ctx(20, manage), account_id=account,
                                                   expected_version=version)

    old = get_account()
    assert old.account_id == account and old.platform == 'qq' and old.platform_account_id == '10001'
    assert old.state == platform.LIFECYCLE_ACTIVE and old.version == 1
    rejected(api.GetAccount, runtime.RuntimeGetAccountRequest(context=ctx(20, read),
             resource=platform.ResourceRef(kind=platform.RESOURCE_ADAPTER, id=account)),
             grpc.StatusCode.INVALID_ARGUMENT)
    rejected(api.GetAccount, runtime.RuntimeGetAccountRequest(context=ctx(20, read),
             resource=platform.ResourceRef(kind=platform.RESOURCE_ACCOUNT, id=str(uuid.uuid4()))),
             grpc.StatusCode.PERMISSION_DENIED)
    rejected(api.UpdateAccount, update(read, old.version, 'denied'), grpc.StatusCode.PERMISSION_DENIED)
    change = update(manage, old.version, 'Revised')
    updated = api.UpdateAccount(change, timeout=5).account
    assert updated.display_name == 'Revised' and updated.version == old.version + 1
    retry = update(manage, old.version, 'Revised')
    retry.context.idempotency_key = change.context.idempotency_key
    assert api.UpdateAccount(retry, timeout=5).account == updated and get_account() == updated
    changed_body = update(manage, old.version, 'different')
    changed_body.context.idempotency_key = change.context.idempotency_key
    rejected(api.UpdateAccount, changed_body, grpc.StatusCode.ABORTED)
    rejected(api.UpdateAccount, update(manage, old.version, 'stale'), grpc.StatusCode.ABORTED)

    before = time.time_ns()
    response = api.GetAdapter(runtime.RuntimeGetAdapterRequest(
        context=ctx(20, adapter_read), resource=adapter_ref), timeout=5)
    found = response.adapter
    assert before <= response.observed_at.ToNanoseconds() <= time.time_ns()
    assert found.observed_at == response.observed_at and found.last_registered_at.ToNanoseconds() == 100
    assert found.last_registration.implementation_version == '1' and found.last_registration.protocol_version == 'kiarabot.v1'
    assert not found.online and found.current_fence.instance_id == instance
    rejected(api.GetAdapter, runtime.RuntimeGetAdapterRequest(context=ctx(20, adapter_read),
             resource=platform.ResourceRef(kind=platform.RESOURCE_ACCOUNT, id=adapter)),
             grpc.StatusCode.INVALID_ARGUMENT)
    assert found.resource == adapter_ref and found.last_registration.implementation == 'p05-local'
    assert found.version == 1 and found.lifecycle == platform.LIFECYCLE_ACTIVE
    rejected(api.GetAdapter, runtime.RuntimeGetAdapterRequest(context=ctx(20, read), resource=adapter_ref),
             grpc.StatusCode.PERMISSION_DENIED)

    binding = runtime.AdapterBinding(account_id=account, adapter_instance_id=instance)
    def bind(proof, **condition):
        return runtime.RuntimeBindAdapterRequest(context=ctx(20, proof), binding=binding, **condition)

    rejected(api.BindAdapter, bind(read), grpc.StatusCode.PERMISSION_DENIED)
    rejected(api.BindAdapter, bind(manage, expected_version=1), grpc.StatusCode.ABORTED)
    create = bind(manage)
    first = api.BindAdapter(create, timeout=5)
    assert first.binding.account_id == account and first.binding.adapter_instance_id == instance
    assert first.binding.version == 1 and first.operation.operation_id
    replay = api.BindAdapter(create, timeout=5)
    assert replay.binding == first.binding and replay.operation == first.operation and not replay.bootstrap_credential
    rejected(api.DisableAccount, disable(updated.version), grpc.StatusCode.ABORTED)
    def unbind(proof, version):
        return runtime.RuntimeUnbindAdapterRequest(context=ctx(20, proof), binding=binding,
                                                  expected_version=version)
    rejected(api.UnbindAdapter, unbind(read, 1), grpc.StatusCode.PERMISSION_DENIED)
    rejected(api.UnbindAdapter, unbind(manage, 100), grpc.StatusCode.ABORTED)
    wrong_instance = unbind(manage, first.binding.version)
    wrong_instance.binding.adapter_instance_id = str(uuid.uuid4())
    rejected(api.UnbindAdapter, wrong_instance, grpc.StatusCode.ABORTED)
    remove = unbind(manage, first.binding.version)
    removed = api.UnbindAdapter(remove, timeout=5)
    assert removed.binding.version == 2 and api.UnbindAdapter(remove, timeout=5) == removed
    rejected(api.BindAdapter, bind(manage, expected_version=first.binding.version), grpc.StatusCode.ABORTED)
    rebound = api.BindAdapter(bind(manage, expected_version=removed.binding.version), timeout=5)
    assert rebound.binding.version == 3
    assert api.UnbindAdapter(unbind(manage, rebound.binding.version), timeout=5).binding.version == 4

    denied_disable = runtime.RuntimeDisableAccountRequest(context=ctx(20, read), account_id=account,
                                                         expected_version=updated.version)
    rejected(api.DisableAccount, denied_disable, grpc.StatusCode.PERMISSION_DENIED)
    rejected(api.DisableAccount, disable(old.version), grpc.StatusCode.ABORTED)
    retire = disable(updated.version)
    retired = api.DisableAccount(retire, timeout=5)
    assert retired.account.state == platform.LIFECYCLE_DISABLED and retired.account.version == updated.version + 1
    assert api.DisableAccount(retire, timeout=5) == retired and get_account() == retired.account
    rejected(api.UpdateAccount, update(manage, retired.account.version, 'terminal'), grpc.StatusCode.ABORTED)
    rejected(api.BindAdapter, bind(manage), grpc.StatusCode.ABORTED)
