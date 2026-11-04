#!/usr/bin/env python3
"""WS-05/06 v1.2 feed-channel producer acceptance.

Real Control binary + real TLS identity/owner fixtures. The v1.2 feed sources are the WS fixture
owners (not real owner processes): each channel is authenticated through the real Identity channel,
then the Control producer queries the owner List*Changes RPC over real mTLS and the frozen AsyncAPI
envelopes are asserted. No fixture ever returns an empty successful first page; the not-yet-ready
host/health/adapters/plugins channels must fail closed with 1011 instead of an empty stream.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import subprocess
import tempfile
import time

from target_websocket_test import (  # noqa: E402
    ACCESS, Harness, SECRETS, expect_closed, line, recv_json, scope, stop, upgrade_request)

CATALOG_SCOPE = {'resources': [{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]}
ACCOUNT = 'aaaaaaaa-aaaa-4aaa-8aaa-000000000001'
ACCOUNT_SCOPE = scope(ACCOUNT)
TASK = '55555555-5555-4555-8555-0000000000aa'
TASK_SCOPE = {'resources': [{'kind': 'RESOURCE_TASK', 'id': TASK}]}
GAP_TASK = '00000000-0000-4000-8000-0000000000ff'
GAP_SCOPE = {'resources': [{'kind': 'RESOURCE_TASK', 'id': GAP_TASK}]}


def hello(resources):
    return json.dumps({'type': 'hello', 'access_token': ACCESS, 'last_sequence': 0,
                       'scope': {'resources': resources}})


def resume(session_id, last_sequence, resources):
    return json.dumps({'type': 'resume', 'access_token': ACCESS, 'session_id': session_id,
                       'last_sequence': last_sequence,
                       'scope': {'resources': resources}})


def ready(ws):
    message = recv_json(ws, timeout=5)
    assert message['type'] == 'SESSION_READY', message
    assert 'owner_cursor' not in message, 'scoped channels must not invent an owner cursor'
    assert isinstance(message['session_id'], str) and 32 <= len(message['session_id']) <= 256
    assert message['last_sequence'] == 0
    return message


def event(ws, expected_type):
    message = recv_json(ws, timeout=5)
    assert message['schema_version'] == 'kiarabot.stream.v1', message
    assert message['event_type'] == expected_type, message
    assert isinstance(message['sequence'], int) and 1 <= message['sequence'] <= 9007199254740991
    assert message['occurred_at'].endswith('Z'), message
    return message


def wait_event(ws, expected_type, predicate, timeout=8.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        message = recv_json(ws, timeout=max(0.05, deadline - time.monotonic()))
        if message is None:
            continue
        if message.get('event_type') == expected_type and predicate(message['payload']):
            return message
    raise AssertionError(f'timed out waiting for {expected_type}')


def no_event(ws, expected_type, timeout=1.2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        message = recv_json(ws, timeout=max(0.05, deadline - time.monotonic()))
        if message is None:
            continue
        assert message.get('event_type') != expected_type, f'unexpected duplicate {message}'


def health_case(harness):
    ws = harness.websocket(path='/ws/health')
    ws.send(hello([{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]))
    ready(ws)
    message = wait_event(ws, 'health.updated',
                         lambda payload: payload['state'] == 'HEALTHY')
    components = {item['component']: item['state'] for item in message['payload']['components']}
    assert components == {'identity': 'HEALTHY', 'runtime': 'HEALTHY',
                          'scheduler': 'HEALTHY', 'model': 'HEALTHY'}, components
    # identical readiness must not re-emit health events.
    no_event(ws, 'health.updated')
    ws.close()


def degraded_health_case(harness):
    # A single dead owner channel must surface DEGRADED (never a fabricated HEALTHY), while the
    # confirmed identity component stays visible.
    import json
    config = json.loads(harness.config_path.read_text())
    config['control_api']['runtime_endpoint'] = 'localhost:1'
    path = harness.root / 'control-degraded.json'
    path.write_text(json.dumps(config))
    log_path = harness.root / f'control-degraded-{time.monotonic_ns()}.log'
    harness.control_log_paths.append(log_path)
    child = subprocess.Popen([harness.control_binary, '--config', str(path)],
                             stdout=subprocess.PIPE, stderr=open(log_path, 'w+'), text=True)
    try:
        first = line(child)
        assert first.startswith('control_https_port='), first
        port = int(first.split('=')[1])
        ws = harness.websocket(port=port, path='/ws/health')
        ws.send(hello([{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]))
        ready(ws)
        message = wait_event(ws, 'health.updated',
                             lambda payload: payload['state'] == 'DEGRADED')
        components = {item['component']: item['state']
                      for item in message['payload']['components']}
        assert components['runtime'] == 'DEGRADED', components
        assert components['identity'] == 'HEALTHY', components
        ws.close()
    finally:
        stop(child)


def adapters_case(harness):
    constant = '77777777-7777-4777-8777-000000000a01'
    ws = harness.websocket(path='/ws/adapters')
    ws.send(hello([{'kind': 'RESOURCE_ADAPTER', 'id': constant}]))
    ready(ws)
    message = wait_event(ws, 'adapter.updated', lambda payload: payload['id'] == constant)
    assert message['payload']['target']['online'] is True
    assert int(message['payload']['version']) == 1
    assert message['payload']['target']['lifecycle'] == 'LIFECYCLE_ACTIVE'
    no_event(ws, 'adapter.updated')
    ws.close()

    changing = '77777777-7777-4777-8777-000000000a02'
    ws = harness.websocket(path='/ws/adapters')
    ws.send(hello([{'kind': 'RESOURCE_ADAPTER', 'id': changing}]))
    ready(ws)
    first = wait_event(ws, 'adapter.updated', lambda payload: payload['id'] == changing)
    second = wait_event(ws, 'adapter.updated',
                        lambda payload: int(payload['version']) > int(first['payload']['version']))
    assert int(second['payload']['version']) > int(first['payload']['version'])
    ws.close()

    # A transient owner failure stays unknown/stale; the first emitted value is the real record.
    error_id = '77777777-7777-4777-8777-000000000e01'
    ws = harness.websocket(path='/ws/adapters')
    ws.send(hello([{'kind': 'RESOURCE_ADAPTER', 'id': error_id}]))
    ready(ws)
    recovered = wait_event(ws, 'adapter.updated', lambda payload: payload['id'] == error_id)
    assert int(recovered['payload']['version']) == 1
    ws.close()


def plugins_case(harness):
    constant = '66666666-6666-4666-8666-000000000b01'
    ws = harness.websocket(path='/ws/plugins')
    ws.send(hello([{'kind': 'RESOURCE_INSTANCE', 'id': constant}]))
    ready(ws)
    message = wait_event(ws, 'plugin.updated', lambda payload: payload['id'] == constant)
    target = message['payload']['target']
    assert target['instance_id'] == constant
    assert target['state']['lifecycle'] == 'LIFECYCLE_ACTIVE'
    no_event(ws, 'plugin.updated')
    ws.close()

    changing = '66666666-6666-4666-8666-000000000b02'
    ws = harness.websocket(path='/ws/plugins')
    ws.send(hello([{'kind': 'RESOURCE_INSTANCE', 'id': changing}]))
    ready(ws)
    first = wait_event(ws, 'plugin.updated', lambda payload: payload['id'] == changing)
    second = wait_event(ws, 'plugin.updated',
                        lambda payload: int(payload['version']) > int(first['payload']['version']))
    assert int(second['payload']['version']) > int(first['payload']['version'])
    ws.close()

    error_id = '66666666-6666-4666-8666-000000000f01'
    ws = harness.websocket(path='/ws/plugins')
    ws.send(hello([{'kind': 'RESOURCE_INSTANCE', 'id': error_id}]))
    ready(ws)
    recovered = wait_event(ws, 'plugin.updated', lambda payload: payload['id'] == error_id)
    assert int(recovered['payload']['version']) == 1
    ws.close()


ADAPTER_CATALOG = 'c1111111-1111-4111-8111-111111111111'
ADAPTER_FAIL_CATALOG = 'd1111111-1111-4111-8111-111111111111'
ADAPTER_MISMATCH_CATALOG = 'e1111111-1111-4111-8111-111111111111'
INSTANCE_CATALOG = 'f1111111-1111-4111-8111-111111111111'
INSTANCE_FAIL_CATALOG = 'a2111111-1111-4111-8111-111111111111'
INSTANCE_MISMATCH_CATALOG = 'b2111111-1111-4111-8111-111111111111'
ADAPTER_CONSTANT = '77777777-7777-4777-8777-000000000a01'
ADAPTER_CHANGING = '77777777-7777-4777-8777-000000000a02'
ADAPTER_PAGE_TWO = '77777777-7777-4777-8777-000000000a03'
INSTANCE_CONSTANT = '66666666-6666-4666-8666-000000000b01'
INSTANCE_CHANGING = '66666666-6666-4666-8666-000000000b02'
INSTANCE_PAGE_TWO = '66666666-6666-4666-8666-000000000b03'


def collect_catalog_events(ws, event_type, expected_ids, timeout=8.0):
    """Collect the first immutable-snapshot page events until every child id was emitted once."""
    seen = {}
    deadline = time.monotonic() + timeout
    while set(seen) != set(expected_ids) and time.monotonic() < deadline:
        message = recv_json(ws, timeout=max(0.05, deadline - time.monotonic()))
        if message is None or message.get('event_type') != event_type:
            continue
        payload = message['payload']
        seen.setdefault(payload['id'], []).append(payload)
    assert set(seen) == set(expected_ids), (set(seen), expected_ids)
    return seen


def no_repeat_for_id(ws, event_type, resource_id, timeout=1.2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        message = recv_json(ws, timeout=max(0.05, deadline - time.monotonic()))
        if message is None:
            continue
        if message.get('event_type') == event_type:
            assert message['payload']['id'] != resource_id, \
                f'unexpected unchanged {resource_id} re-emission'


def catalog_scan_case(harness):
    # Directory mode: one deployment catalog in scope selects the immutable-page owner scan.
    ws = harness.websocket(path='/ws/adapters')
    ws.send(hello([{'kind': 'RESOURCE_CATALOG', 'id': ADAPTER_CATALOG}]))
    ready(ws)
    seen = collect_catalog_events(
        ws, 'adapter.updated', {ADAPTER_CONSTANT, ADAPTER_CHANGING, ADAPTER_PAGE_TWO})
    assert int(seen[ADAPTER_CONSTANT][0]['version']) == 1
    assert int(seen[ADAPTER_PAGE_TWO][0]['version']) == 1
    # later pages are included: page two child appears in the same snapshot scan.
    changed = wait_event(
        ws, 'adapter.updated',
        lambda payload: payload['id'] == ADAPTER_CHANGING and
        int(payload['version']) > int(seen[ADAPTER_CHANGING][0]['version']))
    assert int(changed['payload']['version']) > int(seen[ADAPTER_CHANGING][0]['version'])
    # The constant child must not be re-emitted by the periodic re-scan.
    no_repeat_for_id(ws, 'adapter.updated', ADAPTER_CONSTANT)
    ws.close()

    # Fail closed: page-2 authorization denial must close instead of producing a partial success.
    ws = harness.websocket(path='/ws/adapters')
    ws.send(hello([{'kind': 'RESOURCE_CATALOG', 'id': ADAPTER_FAIL_CATALOG}]))
    expect_closed(ws, 4403)
    # Fail closed: a snapshot change mid-scan is an internal inconsistency, never mixed pages.
    ws = harness.websocket(path='/ws/adapters')
    ws.send(hello([{'kind': 'RESOURCE_CATALOG', 'id': ADAPTER_MISMATCH_CATALOG}]))
    expect_closed(ws, 1011)

    # Plugins catalog scan: same immutable-page contract over ListInstances.
    ws = harness.websocket(path='/ws/plugins')
    ws.send(hello([{'kind': 'RESOURCE_CATALOG', 'id': INSTANCE_CATALOG}]))
    ready(ws)
    seen = collect_catalog_events(
        ws, 'plugin.updated', {INSTANCE_CONSTANT, INSTANCE_CHANGING, INSTANCE_PAGE_TWO})
    assert int(seen[INSTANCE_CONSTANT][0]['version']) == 1
    assert int(seen[INSTANCE_PAGE_TWO][0]['version']) == 1
    ws.close()
    ws = harness.websocket(path='/ws/plugins')
    ws.send(hello([{'kind': 'RESOURCE_CATALOG', 'id': INSTANCE_FAIL_CATALOG}]))
    expect_closed(ws, 4403)
    ws = harness.websocket(path='/ws/plugins')
    ws.send(hello([{'kind': 'RESOURCE_CATALOG', 'id': INSTANCE_MISMATCH_CATALOG}]))
    expect_closed(ws, 1011)


def logs_case(harness):
    path = '/ws/logs?owner_service=identity'
    ws = harness.websocket(path=path)
    ws.send(hello([{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]))
    ready_message = ready(ws)
    message = event(ws, 'log.updated')
    assert message['sequence'] == 1, message
    payload = message['payload']
    assert payload['component'] == 'identity', payload
    assert payload['level'] == 'INFO', payload
    assert payload['outcome'] == 'request.recorded', payload
    assert payload['operation'] == 'kiarabot.v1.IdentityService/ListLogs', payload
    assert payload['id'] == 'identity-log-1', payload
    assert isinstance(payload.get('trace_id'), str) and payload['trace_id']
    # 10-minute retained replay: resume with last_sequence 0 re-delivers exactly that committed
    # event and no fabricated empty page is needed on reconnect.
    ws.close()
    replay_ws = harness.websocket(path=path)
    replay_ws.send(resume(ready_message['session_id'], 0,
                          [{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]))
    replay_ready = ready(replay_ws)
    assert replay_ready['session_id'] == ready_message['session_id']
    replayed = event(replay_ws, 'log.updated')
    assert replayed['sequence'] == 1 and replayed['payload'] == payload, replayed
    replay_ws.close()


def tasks_case(harness):
    ws = harness.websocket(path='/ws/tasks')
    ws.send(hello([{'kind': 'RESOURCE_TASK', 'id': TASK}]))
    ready(ws)
    message = event(ws, 'task.updated')
    payload = message['payload']
    assert payload['id'] == '55555555-5555-4555-8555-000000000001', payload
    assert int(payload['version']) == 1, payload
    target = payload['target']
    assert target['owner_instance_id'] == '66666666-6666-4666-8666-000000000001', target
    assert target['state'] == 'SCHEDULED_TASK_PENDING', target
    assert target['cancel_requested'] is False
    assert target['updated_at'].endswith('Z')
    ws.close()


def messages_case(harness):
    ws = harness.websocket(path='/ws/messages')
    ws.send(hello([{'kind': 'RESOURCE_ACCOUNT', 'id': ACCOUNT}]))
    ready(ws)
    message = event(ws, 'message.updated')
    payload = message['payload']
    assert payload['message_id'] == '33333333-3333-4333-8333-000000000001', payload
    assert payload['conversation_id'] == '44444444-4444-4444-8444-000000000001', payload
    assert payload['direction'] == 'received', payload
    assert 'account_id' not in payload  # frozen MessagePayload is metadata-only
    ws.close()


def typed_gap_case(harness):
    ws = harness.websocket(path='/ws/tasks')
    ws.send(hello([{'kind': 'RESOURCE_TASK', 'id': GAP_TASK}]))
    ready(ws)  # the captured head is delivered first; the continuation then exposes the gap
    message = recv_json(ws, timeout=5)
    assert message['type'] == 'RESYNC_REQUIRED', message
    assert message['reason'] == 'CURSOR_EXPIRED', message
    expect_closed(ws, 1000)


def fail_closed_case(harness):
    # Host source is not ready: authenticated hello must terminate 1011, never an empty stream.
    ws = harness.websocket(path='/ws/operations?owner_service=host')
    ws.send(hello([{'kind': 'RESOURCE_HOST', 'id': '88888888-8888-4888-8888-000000000001'}]))
    expect_closed(ws, 1011)
    # Unknown routes stay closed at the Upgrade boundary (never an accepted empty stream).
    status, _ = harness.raw_https(upgrade_request('/ws/unsupported'))
    assert status == 404, status


def main(control, identity_fixture, owner_fixture):
    harness = Harness(control, identity_fixture, owner_fixture)
    try:
        logs_case(harness)
        tasks_case(harness)
        messages_case(harness)
        typed_gap_case(harness)
        health_case(harness)
        degraded_health_case(harness)
        adapters_case(harness)
        plugins_case(harness)
        catalog_scan_case(harness)
        fail_closed_case(harness)
    except BaseException:
        harness.cleanup()
        print('---- control/fixture logs ----', file=sys.stderr)
        print(harness.logs, file=sys.stderr)
        raise
    else:
        harness.cleanup()
    assert not any(secret in harness.logs for secret in SECRETS), \
        'Control/fixture logs leaked a credential'
    print('WS target feed producers: ok')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        print('usage: target_websocket_producers_test.py <kiarabot-control> <identity-fixture> '
              '<owner-fixture>', file=sys.stderr)
        sys.exit(2)
    try:
        main(*sys.argv[1:])
    except Exception as error:  # noqa: BLE001
        print(f'WS target feed producers failed: {error}', file=sys.stderr)
        raise
