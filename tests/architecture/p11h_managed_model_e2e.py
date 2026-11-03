#!/usr/bin/env python3
"""P11h managed-plugin -> Runtime -> Model -> real HTTP Provider e2e (dependency-gated).

PREPARATION ONLY while task-145/P05b-4 (Runtime instance-delegation provider for Model) is not
merged. The script reuses the P08e managed-host harness and the real loopback HTTP Provider
fixture; its positive chain is intentionally not runnable until the dependency lands. Do not
register or report this script as a passing positive chain before the dependency is available.

Dependency surface (P05b-8 already on main): `InstanceParentDelegationSource::acquire_model()`,
`RuntimeInstanceDelegationProvider::model_chat_delegation(context, model_id, permission)`, the
sealed model envelope `model_instance_delegation_sealed_file` and model-only RESOURCE_MODEL scope.
P05b-9 adds `RuntimeIdentityClientOptions::delegation_budget` (2s) /
`delegation_maximum_budget` (5s) so ExchangeDelegation no longer exhausts the CheckAuthorization
hot-path budget; no new config/proto fields are required.
Pending: P05b-4 wiring `RuntimeModelClient` to that provider, adding `host.model.v1` to the
PluginBridge supported features, and shipping the canonical `p05b_provider_fixture.py` plus
managed-host mode in `p05b_model_chain_e2e.py` (`p05b_model_chain_e2e`,
`p05b_model_chain_fail_closed_e2e`, `p05b_provider_fixture`). Until P05b-4 merges,
`context.model_chat` cannot reach Model on a real Runtime; this file is planning/preparation only
and must not be enabled as a passing CTest.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tests/architecture'))
import p08_managed_host_e2e as managed  # noqa: E402
# P05b-4 ships the canonical real HTTP provider fixture. Until it merges, keep the preparation
# fixture as a fallback so the non-dependent fixture/package checks remain runnable.
try:  # pragma: no cover - depends on the P05b-4 merge
    from p05b_provider_fixture import FixtureState, chat_completion as p05b_chat_completion
    PROVIDER_FIXTURE_SOURCE = 'p05b_provider_fixture (canonical P05b-4)'
except ImportError:  # preparation fallback only
    from p11_http_provider_fixture import (ProviderFixture, chat_completion
                                           as p11_chat_completion)
    PROVIDER_FIXTURE_SOURCE = 'p11_http_provider_fixture (preparation fallback)'

#: Positive upstream matrix (canonical P05b-4 provider fixture mode names). Every non-success
#: mode must map to the P11 stable error and must never manufacture a successful call record.
PROVIDER_MATRIX = (
    'success', 'success_no_usage', 'invalid_json', 'bad_choices', 'tool_calls', 'bad_usage',
    'status_400', 'status_401', 'status_404', 'status_429', 'status_500', 'status_503',
    'timeout', 'assert_stream_false',
)
P05B9_API = ('RuntimeIdentityClientOptions.delegation_budget=2s',
             'delegation_maximum_budget=5s', 'no new config/proto field')
POSITIVE_PLAN = (
    ('p05b4_dependencies', 'P05b-4 host.model.v1 + RuntimeModelClient sealed provider wiring'),
    ('managed_host_start', 'HostBridge start/apply command spawns the real managed plugin child'),
    ('plugin_registration', 'SDK child registers over the real TLS PluginBridge with host.model.v1'),
    ('model_chat', 'plugin context.model_chat reaches Runtime, Model and the real HTTP provider'),
    ('result_usage', 'plugin receives result text plus provider usage tokens'),
    ('call_record', 'Model/PG call record owner is the plugin instance; no secret/body leakage'),
    ('delegation_negatives', 'revoked/expired/cross-instance/wrong-cert parent delegation rejected'),
    ('provider_negatives', '4xx/429/5xx/timeout/non-JSON map to stable errors, no fake success'),
    ('unreachable_negatives', 'Runtime/Identity unreachable stays fail-closed; cross-instance call invisible'),
)
NEGATIVE_MATRIX = (
    'revoked_parent_delegation', 'expired_parent_delegation', 'cross_instance_delegation',
    'wrong_runtime_certificate', 'runtime_unreachable', 'identity_unreachable',
    'provider_4xx_429_5xx_timeout_non_json_mapping',
    'provider_failure_does_not_pollute_call_record', 'cross_instance_call_invisible',
    'no_secret_or_plugin_body_in_logs',
)


def write_model_package(directory: Path, repo: Path):
    """Managed plugin package whose on_start performs one model.chat and records the result."""
    package = directory / 'model-package'
    package.mkdir(parents=True, exist_ok=True)
    marker = directory / 'plugin.model.json'
    (package / 'subject.py').write_text(
        'import json\n'
        'from pathlib import Path\n'
        'from kiarabot_sdk import Plugin, PluginError\n'
        'class Subject(Plugin):\n'
        '    async def on_start(self, context):\n'
        '        try:\n'
        '            result = await context.model_chat(\n'
        '                "provider-model", [{"role": "user", "text": "p11h"}],\n'
        '                logical_call_id=None, timeout=3.0)\n'
        f'            Path({str(marker)!r}).write_text(json.dumps({{\n'
        '                "ok": True, "text": result.result.text,\n'
        '                "input_tokens": result.result.input_tokens,\n'
        '                "output_tokens": result.result.output_tokens}))\n'
        '        except PluginError as error:\n'
        f'            Path({str(marker)!r}).write_text(json.dumps({{\n'
        '                "ok": False, "code": int(error.code), "retryable": error.retryable}))\n'
        'def create():\n'
        '    return Subject()\n')
    manifest = json.loads((repo / 'plugins/event-observer/plugin.json').read_text())
    manifest['id'] = 'p11h.model.plugin'
    manifest['entrypoint'] = 'subject:create'
    manifest['permissions'] = ['message.read', 'model.chat']
    manifest['capabilities'] = ['event.message.received']
    manifest_path = package / 'plugin.json'
    manifest_path.write_text(json.dumps(manifest))
    return package, manifest_path, marker


def provider_contract_check():
    """Run the real HTTP fixture's own contract checks (not the P11 positive chain)."""
    if PROVIDER_FIXTURE_SOURCE.startswith('p05b_provider_fixture'):
        raise RuntimeError('P05b-4 fixture present: use its documented --self-test/owned-fixture '
                           'driver; this preparation check only exercises the fallback fixture')
    fixture = ProviderFixture().start()
    try:
        assert fixture.endpoint.startswith('http://127.0.0.1:')
        assert p11_chat_completion()['usage']['total_tokens'] == 12
    finally:
        fixture.close()

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare-only', action='store_true',
                        help='validate fixture/package scaffolding without running the chain')
    parser.add_argument('command', nargs='?', help='reserved for --runtime/--provider arguments')
    args = parser.parse_args()
    if not args.prepare_only:
        raise SystemExit(
            'BLOCKED: P11h positive chain requires task-145/P05b-4 (Runtime instance delegation '
            'provider) and host.model.v1 bridge feature; dependency not merged. '
            'Use --prepare-only for non-dependent checks.')
    provider_contract_check()
    package, manifest, marker = write_model_package(Path('/tmp/p11h-prepare'), REPO)
    assert package.is_dir() and manifest.is_file()
    print(f'P11h preparation checks passed; provider fixture source={PROVIDER_FIXTURE_SOURCE}')
    print('positive plan=' + ','.join(name for name, _ in POSITIVE_PLAN))
    print('provider matrix=' + ','.join(PROVIDER_MATRIX))
    print('negative matrix=' + ','.join(NEGATIVE_MATRIX))
    print('P05b-8 API available: acquire_model/model_chat_delegation + model sealed file; '
          'P05b-9 budgets: ' + ', '.join(P05B9_API) + '; '
          'P05b-4 RuntimeModelClient wiring + host.model.v1 + canonical provider fixture pending')
    print('BLOCKED positive chain: P05b-4 provider/model wiring not merged')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
