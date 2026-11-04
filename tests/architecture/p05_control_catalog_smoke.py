#!/usr/bin/env python3
"""Exercise approved P05 REST paths against a running isolated Control deployment.

Credentials stay in local files and are never printed. This does not provision Identity grants,
change assignments, or prove external QQ/Provider connectivity. Exact Runtime Get authorization
and fresh-resource post-creation grant provisioning require separate owner acceptance.

Example:
  python p05_control_catalog_smoke.py --base-url https://localhost:6443 \
    --ca-file /local/ca.pem --token-file /local/access-token \
    --catalogs-file /local/catalogs.json --expect-host-gap --create-account

catalogs.json: five keys accounts/installations/instances/adapters/hosts with catalog UUID values.
Optional selections.json: the same family keys with arrays of exact child UUIDs (missing = empty).
Each selection is a separate snapshot, never a promise to discover ungranted or unknown children.
"""
import argparse
import json
from pathlib import Path
import ssl
import urllib.error
import urllib.parse
import urllib.request
import uuid

FAMILIES = {
    'accounts': ('accounts', 'RESOURCE_ACCOUNT'),
    'installations': ('installations', 'RESOURCE_INSTALLATION'),
    'instances': ('instances', 'RESOURCE_INSTANCE'),
    'adapters': ('adapter-instances', 'RESOURCE_ADAPTER'),
    'hosts': ('hosts', 'RESOURCE_HOST'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-url', required=True)
    parser.add_argument('--ca-file', required=True, type=Path)
    parser.add_argument('--token-file', required=True, type=Path)
    parser.add_argument('--catalogs-file', required=True, type=Path)
    parser.add_argument('--selections-file', type=Path)
    parser.add_argument('--expect-host-gap', action='store_true')
    parser.add_argument('--create-account', action='store_true')
    args = parser.parse_args()
    if urllib.parse.urlsplit(args.base_url).scheme != 'https':
        parser.error('--base-url must use HTTPS')
    token = args.token_file.read_text().strip()
    if not token or '\n' in token or '\r' in token:
        parser.error('token file must contain one non-empty access token')
    catalogs = json.loads(args.catalogs_file.read_text())
    if set(catalogs) != set(FAMILIES):
        parser.error('catalog file must contain exactly the five resource families')
    for value in catalogs.values():
        if str(uuid.UUID(value)) != value:
            parser.error('catalog IDs must be canonical UUIDs')
    if len(set(catalogs.values())) != 5:
        parser.error('resource families must use distinct catalog IDs')
    selections = json.loads(args.selections_file.read_text()) if args.selections_file else None
    context = ssl.create_default_context(cafile=str(args.ca_file))
    checked = []

    def call(method, path, body=None, key=None):
        headers = {'Authorization': 'Bearer ' + token, 'X-Request-ID': str(uuid.uuid4())}
        if key:
            headers['Idempotency-Key'] = key
        data = None
        if body is not None:
            headers['Content-Type'] = 'application/json'
            data = json.dumps(body).encode()
        request = urllib.request.Request(args.base_url.rstrip('/') + '/api/v1/' + path,
                                         data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, context=context, timeout=15) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)

    def expect(result, status, label, code=None):
        actual_status, value = result
        # Print only stable codes, never upstream response bodies or credentials.
        if actual_status != status or (code and value.get('code') != code):
            raise RuntimeError(f'{label}: expected {status}/{code or "success"}; '
                               f'got {actual_status}/{value.get("code", "response")}')
        checked.append(label)
        return value

    for family, (route, kind) in FAMILIES.items():
        query = {'catalog': json.dumps({'kind': 'RESOURCE_CATALOG', 'id': catalogs[family]}),
                 'page_size': 1}
        if selections is not None:
            children = selections.get(family, [])
            if len(children) > 127:
                parser.error('each selected child batch must contain at most 127 IDs')
            query['scope'] = json.dumps({'resources': [{'kind': kind, 'id': item}
                                                       for item in children]})
        target = route + '?' + urllib.parse.urlencode(query)
        result = call('GET', target)
        if family == 'hosts' and args.expect_host_gap:
            expect(result, 503, 'hosts capability gap', 'CAPABILITY_NOT_SUPPORTED')
            continue
        value = expect(result, 200, family + ' first page')
        if not isinstance(value.get(family), list) or not isinstance(value.get('snapshot'), dict):
            raise RuntimeError(f'{family}: missing typed list or snapshot')
        cursor = value['snapshot'].get('page', {}).get('next_page_token')
        if cursor:
            query['page_token'] = cursor
            page_target = route + '?' + urllib.parse.urlencode(query)
            page = expect(call('GET', page_target), 200, family + ' next page')
            replay = expect(call('GET', page_target), 200, family + ' cursor replay')
            if replay != page:
                raise RuntimeError(f'{family}: replay changed the persisted page')
        query.pop('page_token', None)
        query['scope'] = json.dumps({'resources': []})
        empty = expect(call('GET', route + '?' + urllib.parse.urlencode(query)), 200,
                       family + ' catalog-only empty selection')
        if empty.get(family) != []:
            raise RuntimeError(f'{family}: empty selected child scope expanded visibility')
    expect(call('GET', 'accounts'), 400, 'missing catalog rejected', 'INVALID_ARGUMENT')
    wrong_family = urllib.parse.urlencode({'catalog': json.dumps(
        {'kind': 'RESOURCE_CATALOG', 'id': catalogs['installations']})})
    expect(call('GET', 'accounts?' + wrong_family), 403, 'cross-family catalog rejected',
           'PERMISSION_DENIED')
    if args.create_account:
        body = {'platform': 'qq', 'platform_account_id': 'p05-accept-' + str(uuid.uuid4()),
                'display_name': 'isolated P05 acceptance',
                'catalog': {'kind': 'RESOURCE_CATALOG', 'id': catalogs['accounts']}}
        key = 'p05-create-' + str(uuid.uuid4())
        created = expect(call('POST', 'accounts', body, key), 200, 'account created')
        replay = expect(call('POST', 'accounts', body, key), 200,
                        'account replay with new request ID')
        if replay != created:
            raise RuntimeError('same-key account creation changed its committed response')
        # Resource IDs are public; the full response remains local and is never printed.
        checked.append('created account ID: ' + created['account']['account_id'])
    print(json.dumps({'checks': checked, 'result': 'passed'}, ensure_ascii=False))


if __name__ == '__main__':
    main()
