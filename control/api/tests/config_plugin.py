"""Acceptance subprocess: a real Python SDK, never imported by Core."""
import asyncio
import json
import os
import signal
from pathlib import Path

from kiarabot.v1 import common_pb2 as common, plugins_pb2 as plugins
from google.protobuf.json_format import MessageToDict
from kiarabot_sdk import Options, Plugin, PluginError, Runner


class Configurable(Plugin):
    async def on_config(self, config, context):
        values = MessageToDict(config.values)
        if values.get('mode') == 'reject':
            raise PluginError(common.INVALID_ARGUMENT)
        if values.get('mode') == 'timeout':
            await asyncio.Event().wait()
        # Proves that reading RPC responses is independent of configuration hooks.
        await context.storage_set('applied-config', config.version.encode(), config.delivery_id)
        with Path(os.environ['CONFIG_OBSERVATIONS']).open('a') as stream:
            stream.write(json.dumps({'version': config.version, 'values': values}) + '\n')


async def main():
    stop = asyncio.Event()
    asyncio.get_running_loop().add_signal_handler(signal.SIGTERM, stop.set)
    manifest = plugins.PluginManifest(id='test.plugin', name='Test', version='1.0.0',
        api_version='kiarabot.plugin.v1', runtime='python-rpc', isolation='process',
        entrypoint='example:create', permissions=['storage.namespace'])
    await Runner(manifest, Configurable(), Options(os.environ['PLUGIN_CORE_ENDPOINT'],
        os.environ['PLUGIN_AUTH_TOKEN'], hook_timeout=.3, heartbeat_interval=.05, retry_delay=.02)).run(stop)


asyncio.run(main())
