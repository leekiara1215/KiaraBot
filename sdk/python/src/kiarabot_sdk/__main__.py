import argparse
import asyncio
import importlib
import logging
import os
import signal

from kiarabot.v1 import common_pb2 as common
from .runtime import Options, Plugin, PluginError, Runner, UncooperativeHook, load_manifest


async def run(manifest_path):
    try:
        manifest = load_manifest(manifest_path)
        module, factory = manifest.entrypoint.split(':')
        plugin = getattr(importlib.import_module(module), factory)()
        if not isinstance(plugin, Plugin):
            raise PluginError(common.INVALID_ARGUMENT)
        options = Options(os.environ['PLUGIN_CORE_ENDPOINT'], os.environ['PLUGIN_AUTH_TOKEN'])
        stop = asyncio.Event()
        loop = asyncio.get_running_loop()
        for signum in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(signum, stop.set)
        await Runner(manifest, plugin, options).run(stop)
        return 0
    except UncooperativeHook:
        # Only the executable exits forcibly. Library callers receive the error.
        logging.error('operation=plugin_process error_code=TIMEOUT')
        os._exit(1)
    except Exception as failure:
        code = failure.code if isinstance(failure, PluginError) else common.INTERNAL
        logging.error('operation=plugin_process error_code=%s', common.ErrorCode.Name(code))
        return 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--manifest', required=True)
    arguments = parser.parse_args()
    raise SystemExit(asyncio.run(run(arguments.manifest)))


if __name__ == '__main__':
    main()
