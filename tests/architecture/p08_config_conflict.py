#!/usr/bin/env python3
"""P08f SDK config apply version conflict: stale/above-desired deliveries never overwrite applied."""
from __future__ import annotations

import asyncio
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile


def load_sdk_host_test(repo: Path):
    sys.path.insert(0, str(repo / 'sdk/python/src'))
    sys.path.insert(0, str(repo / 'sdk/python/build/generated'))
    sys.path.insert(0, str(repo / 'sdk/python/tests'))
    path = repo / 'sdk/python/tests/test_host_managed.py'
    spec = importlib.util.spec_from_file_location('p08_sdk_host_test', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


async def wait_for(predicate, label, timeout=10.0):
    deadline = asyncio.get_running_loop().time() + timeout
    while asyncio.get_running_loop().time() < deadline:
        if predicate():
            return
        await asyncio.sleep(0.02)
    raise RuntimeError(f'{label}: timed out')


async def main_async(repo: Path) -> None:
    base = load_sdk_host_test(repo)
    with tempfile.TemporaryDirectory(prefix='kiarabot-p08f-config-') as directory:
        root = Path(directory)
        fixture = await base._start_managed_host(root, name='config-conflict')
        try:
            runtime = fixture['runtime']
            applied_path = root / 'applied.jsonl'
            await runtime.send_config('v1', {'greeting': 'one'}, config_version=1,
                                      delivery_id='delivery-1')
            ack1 = await asyncio.wait_for(runtime.config_acknowledgements.get(), 10)
            assert ack1.accepted and ack1.config_version == 1, ack1
            await wait_for(applied_path.exists, 'first config apply')

            # An above-desired delivery is rejected in-band and never reaches on_config.
            await runtime.send_config('v2', {'greeting': 'two'}, config_version=2,
                                      delivery_id='delivery-2')
            ack2 = await asyncio.wait_for(runtime.config_acknowledgements.get(), 10)
            assert not ack2.accepted and ack2.error.code, ack2
            lines = applied_path.read_text().splitlines()
            assert len(lines) == 1, lines

            # A new execution generation re-registers with desired_config_version=2; then a lower
            # version is acknowledged but must not replace the applied v2 configuration.
            import sqlite3
            database = sqlite3.connect(fixture['database'])
            try:
                installation_id = database.execute(
                    'SELECT installation_id FROM managed_executions LIMIT 1').fetchone()[0]
            finally:
                database.close()
            await fixture['bridge'].send(base.stop_request(
                host_id=fixture['host_id'], instance_id=fixture['instance_id'],
                generation=1, command_id='stop-1'))
            stopped = await asyncio.wait_for(fixture['bridge'].receipts.get(), 10)
            assert stopped.persisted
            runtime.desired_config_version = 2
            manifest = root / 'plugin.json'
            package_sha = bytes.fromhex('a' * 64)
            await fixture['bridge'].send(base.apply_request(
                host_id=fixture['host_id'], instance_id=fixture['instance_id'],
                installation_id=installation_id, generation=2, desired_version=2,
                package_sha256=package_sha, manifest=manifest, command_id='apply-2'))
            second_receipt = await asyncio.wait_for(fixture['bridge'].receipts.get(), 10)
            assert second_receipt.persisted
            await asyncio.wait_for(runtime.registrations.get(), 10)
            await runtime.send_config('v2', {'greeting': 'two'}, config_version=2,
                                      delivery_id='delivery-3')
            ack3 = await asyncio.wait_for(runtime.config_acknowledgements.get(), 10)
            assert ack3.accepted and ack3.config_version == 2, ack3
            await wait_for(lambda: len(applied_path.read_text().splitlines()) == 2,
                           'second config apply')
            await runtime.send_config('v1-old', {'greeting': 'old'}, config_version=1,
                                      delivery_id='delivery-4')
            ack4 = await asyncio.wait_for(runtime.config_acknowledgements.get(), 10)
            assert ack4.accepted and ack4.config_version == 1, ack4
            await asyncio.sleep(0.3)
            lines = [json.loads(line) for line in applied_path.read_text().splitlines()]
            assert len(lines) == 2 and [item['config_version'] for item in lines] == [1, 2], lines
            print('P08 SDK config conflict: PASS')
        finally:
            await base._stop_fixture(fixture)


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    try:
        asyncio.run(main_async(repo))
        return 0
    except Exception as error:  # noqa: BLE001
        print(f'P08 config conflict FAILED: {type(error).__name__}: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
