import asyncio
import os
from pathlib import Path
import re
import signal
from contextlib import suppress

import pytest
from kiarabot.v1 import common_pb2 as common
from kiarabot_adapter_nonebot.client import ClientConfig, CoreClient
from kiarabot_adapter_nonebot.mapper import map_message
from kiarabot_adapter_nonebot.outbox import Outbox
from kiarabot_adapter_nonebot.status import AdapterStatusReporter
from test_client import until
from test_mapping_outbox import raw


@pytest.mark.skipif(not os.environ.get('CORE_TEST_BINARY'), reason='Set CORE_TEST_BINARY for C++ integration')
async def test_cpp_persistence_and_python_protocol(tmp_path):
    binary = Path(os.environ['CORE_TEST_BINARY']).resolve()
    environment = {key: value for key, value in os.environ.items() if not key.startswith('KIARABOT_')}
    environment.update(KIARABOT_ADAPTER_ID='adapter', KIARABOT_ADAPTER_TOKEN='x' * 32,
                       KIARABOT_ADAPTER_ACCOUNTS='123', KIARABOT_RUNTIME_DB=str(tmp_path / 'core.db'))
    original = map_message(raw(), 'adapter', '123')
    for attempt in range(2):
        process = await asyncio.create_subprocess_exec(str(binary), '--grpc-listen', '127.0.0.1:0',
                    env=environment, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        store, task = None, None
        try:
            line = await asyncio.wait_for(process.stdout.readline(), 5)
            assert b'adapter_ingress_ready=true' in line
            port = int(re.search(rb'port=(\d+)', line)[1])
            store = Outbox(tmp_path / f'outbox-{attempt}.db')
            store.enqueue(original)
            # Core persists per-session sequence; only the first attempt owns the
            # status event because the second reuses the same Core database.
            reporter = AdapterStatusReporter(store, 'adapter', '123') if attempt == 0 else None
            status_id = reporter.report(common.HEALTHY, 'core_connected') if reporter else None
            client = CoreClient(ClientConfig(f'127.0.0.1:{port}', 'x' * 32, 'adapter', '123',
                                             retry_base=0.01, idle_interval=0.01), store, None, reporter)
            stop = asyncio.Event()
            task = asyncio.create_task(client.run(stop))
            await until(lambda: store.status(original.event_id) == ('acked', 0)
                        and (status_id is None or store.status(status_id) == ('acked', 0)))
        finally:
            if task:
                task.cancel()
                with suppress(asyncio.CancelledError):
                    await task
            if store:
                store.close()
            if process.returncode is None:
                process.send_signal(signal.SIGTERM)
                try:
                    output, error = await asyncio.wait_for(process.communicate(), 5)
                except asyncio.TimeoutError:
                    process.kill()
                    await process.wait()
                    raise
                assert process.returncode == 0
                assert b'x' * 32 not in output + error
