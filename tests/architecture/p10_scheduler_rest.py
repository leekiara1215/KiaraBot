#!/usr/bin/env python3
"""P10h Scheduler task REST target acceptance through the real Control HTTPS boundary.

Runs the shipped Control binary with real TLS and the generated Identity/Model/Scheduler
owner fixtures, then asserts the Scheduler task REST success chain (list page, get task,
cancel 200/operation-shaped response, repeat cancel, owner CONFLICT and the Control-level
NOT_FOUND route). This is the target-directory evidence for the scheduler REST operations;
it does not run a real Scheduler/PG process (projection fields are fixture-owned).
"""
from __future__ import annotations

import http.client
import json
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.parse
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'control/api/tests/architecture'))
sys.path.insert(0, str(REPO / 'core/tests/platform'))
from auth_https_test import ACCESS, KEY, SESSION, line, stop  # noqa: E402
from service_config_test import certificates  # noqa: E402


def main(control, identity_fixture, model_fixture, owner_fixture):
    with tempfile.TemporaryDirectory(prefix='p10-scheduler-rest-') as folder:
        root = Path(folder)
        config = certificates(root, 'ec')
        tls = config['tls']
        identity = subprocess.Popen(
            [identity_fixture, tls['ca_file'], tls['certificate_file'], tls['private_key_file']],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        model = None
        owner = None
        child = None
        logs = ''
        try:
            identity_port = int(line(identity))
            model = subprocess.Popen(
                [model_fixture, tls['ca_file'], tls['certificate_file'], tls['private_key_file']],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            model_port = int(line(model))
            owner = subprocess.Popen(
                [owner_fixture, tls['ca_file'], tls['certificate_file'], tls['private_key_file']],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            owner_port = int(line(owner))
            config['service'] = 'control'
            config.pop('grpc_listen', None)
            config['control_api'] = {
                'listen': '127.0.0.1:0',
                'identity_endpoint': f'localhost:{identity_port}',
                'runtime_endpoint': f'localhost:{owner_port}',
                'scheduler_endpoint': f'localhost:{owner_port}',
                'model_endpoint': f'localhost:{model_port}',
            }
            path = root / 'control.json'
            path.write_text(json.dumps(config))
            child = subprocess.Popen([control, '--config', str(path)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            first = line(child)
            assert first.startswith('control_https_port='), first
            port = int(first.split('=')[1])
            line(child)  # ready/degraded state; requests retry below

            context = ssl.create_default_context(cafile=tls['ca_file'])

            def request(method, target, body=None, token=None, raw=False):
                connection = http.client.HTTPSConnection('localhost', port, context=context,
                                                         timeout=5)
                headers = {}
                if token:
                    headers['Authorization'] = 'Bearer ' + token
                if body is not None:
                    if not raw:
                        body = json.dumps(body)
                    headers['Content-Type'] = 'application/json'
                    headers['Idempotency-Key'] = KEY
                try:
                    connection.request(method, target, body, headers)
                    response = connection.getresponse()
                    content = response.read()
                    parsed = json.loads(content) if content else None
                    return response.status, parsed
                finally:
                    connection.close()

            status, tokens = request('POST', '/api/v1/auth/login',
                                     {'username': 'alice', 'password': 'never-print-password'})
            assert status == 200 and tokens['access_token'] == ACCESS, (status, tokens)
            token = tokens['access_token']
            scope = urllib.parse.quote(json.dumps(
                {'resources': [{'kind': 'RESOURCE_ACCOUNT', 'id': SESSION}]}))
            deadline = time.monotonic() + 5
            while True:
                status, body = request('GET', '/api/v1/health', token=token)
                if status == 200:
                    break
                assert time.monotonic() < deadline, (status, body)
                time.sleep(0.05)
            assert {item['component'] for item in body['components']} >= {
                'identity', 'runtime', 'scheduler', 'model'}, body

            # 1) GET /tasks pagination forwarding: page_size is sent to the owner and echoed.
            status, body = request(
                'GET', '/api/v1/tasks?scope=' + scope + '&page_size=2', token=token)
            assert status == 200, (status, body)
            assert body['items'] and body['items'][0]['id'] == 'task-fixture', body
            assert str(body['page']['page_size']) == '2', body['page']
            # A filter parameter is accepted by the transport; owner-side filtering is owned by
            # Scheduler and not claimed by this fixture.
            status, body = request(
                'GET', '/api/v1/tasks?scope=' + scope + '&page_size=1&state=SCHEDULED',
                token=token)
            assert status == 200 and body['items'], (status, body)

            # 2) GET /tasks/{id}: task projection and owner scope forwarding.
            status, body = request(
                'GET', '/api/v1/tasks/task-fixture?scope=' + scope, token=token)
            assert status == 200 and body['task']['task_id'] == 'task-fixture', (status, body)

            # 3) POST cancel: 200 with owner task result, repeatable with the same key.
            status, body = request(
                'POST', '/api/v1/tasks/task-fixture/cancel?scope=' + scope, {}, token=token)
            assert status == 200 and body['task']['task_id'] == 'task-fixture', (status, body)
            assert body['task']['state'] == 'SCHEDULED_TASK_CANCELLED', body
            repeated = request(
                'POST', '/api/v1/tasks/task-fixture/cancel?scope=' + scope, {}, token=token)
            assert repeated[0] == 200 and repeated[1]['task']['state'] == \
                'SCHEDULED_TASK_CANCELLED', repeated

            # 4) Owner rejection and Control route NOT_FOUND are stable mapped errors.
            conflict = request(
                'POST', '/api/v1/tasks/conflict/cancel?scope=' + scope, {}, token=token)
            assert conflict[0] == 409 and conflict[1]['code'] == 'CONFLICT', conflict
            missing = request('GET', '/api/v1/tasks/?scope=' + scope, token=token)
            assert missing[0] == 404 and missing[1]['code'] == 'NOT_FOUND', missing

            # 5) GET /occurrences/{id}: owner GetOccurrence projection through Scheduler task.read.
            status, body = request(
                'GET', '/api/v1/occurrences/occ-fixture?scope=' + scope, token=token)
            assert status == 200, (status, body)
            occurrence = body['occurrence']
            assert occurrence['occurrence_id'] == 'occ-fixture', body
            assert occurrence['task_id'] == 'task-fixture', body
            assert occurrence['state'] == 'OPERATION_SUCCEEDED', body
            assert occurrence['accepted_event_id'] == 'event-fixture', body
            assert occurrence['attempts'] == 1, body
            # Explicit scope and authentication are mandatory; owner CONFLICT maps 409.
            no_scope = request('GET', '/api/v1/occurrences/occ-fixture', token=token)
            assert no_scope[0] == 400 and no_scope[1]['code'] == 'INVALID_ARGUMENT', no_scope
            anonymous = request('GET', '/api/v1/occurrences/occ-fixture?scope=' + scope)
            assert anonymous[0] == 401 and anonymous[1]['code'] == 'UNAUTHENTICATED', anonymous
            conflict_occurrence = request(
                'GET', '/api/v1/occurrences/conflict-occ?scope=' + scope, token=token)
            assert conflict_occurrence[0] == 409 and \
                conflict_occurrence[1]['code'] == 'CONFLICT', conflict_occurrence
        except BaseException:
            logs = ''
            if child is not None:
                logs = stop(child)[1] or ''
            raise AssertionError(f'P10 Scheduler REST acceptance failed: {logs[-2000:]}') from None
        finally:
            for process in (child, owner, model, identity):
                if process is not None:
                    try:
                        stop(process)
                    except Exception:  # noqa: BLE001 - best-effort fixture cleanup
                        pass
    print('P10 scheduler REST target acceptance passed')


if __name__ == '__main__':
    main(*sys.argv[1:])
