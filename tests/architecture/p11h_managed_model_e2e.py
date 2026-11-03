#!/usr/bin/env python3
"""P11h managed-plugin -> Runtime -> Model -> real HTTP Provider e2e (dependency-gated).

PREPARATION ONLY while task-145/P05b-4 (Runtime instance-delegation provider for Model) is not
merged. The script reuses the P08e managed-host harness and the real loopback HTTP Provider
fixture; its positive chain is intentionally not runnable until the dependency lands. Do not
register or report this script as a passing positive chain before the dependency is available.

Dependency gate: P05b-4 must provide Runtime's production instance parent delegation and the
Runtime PluginBridge must advertise `host.model.v1` (current composition's supported features do
not include it), otherwise `context.model_chat` cannot reach Model on a real Runtime.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tests/architecture'))
import p08_managed_host_e2e as managed  # noqa: E402
from p11_http_provider_fixture import ProviderFixture, chat_completion  # noqa: E402

#: Response modes exercised by the P11h matrix. Stable mapping is asserted by the eventual e2e;
#: 4xx/429/5xx/non-JSON/timeout/invalid-usage must not become a fabricated successful call.
PROVIDER_MATRIX = (
    'success', 'unauthorized', 'bad_request', 'not_found', 'rate_limited',
    'server_error', 'timeout', 'non_json', 'invalid_usage',
)
NEGATIVE_MATRIX = (
    'revoked_parent_delegation', 'expired_parent_delegation', 'cross_instance_delegation',
    'wrong_runtime_certificate', 'runtime_unreachable', 'identity_unreachable',
    'provider_failure_does_not_pollute_call_record', 'cross_instance_call_invisible',
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
    fixture = ProviderFixture().start()
    try:
        assert fixture.endpoint.startswith('http://127.0.0.1:')
        assert chat_completion()['usage']['total_tokens'] == 12
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
    print('P11h preparation checks passed: real HTTP fixture, managed package/manifest, '
          f'provider matrix={",".join(PROVIDER_MATRIX)}; negatives={len(NEGATIVE_MATRIX)}')
    print('BLOCKED positive chain: P05b-4 provider + host.model.v1 bridge feature not merged')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
