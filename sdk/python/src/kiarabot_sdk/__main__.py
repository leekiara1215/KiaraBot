import argparse
import asyncio
import importlib
import logging
import math
import os
import signal

from kiarabot.v1 import common_pb2 as common
from .runtime import Options, Plugin, PluginError, Runner, UncooperativeHook, load_manifest


def options_from_environment(environ=None):
    values = os.environ if environ is None else environ
    try:
        endpoint = values['PLUGIN_CORE_ENDPOINT']
        token = values['PLUGIN_AUTH_TOKEN']
    except KeyError:
        raise PluginError(common.INVALID_ARGUMENT) from None

    def seconds(name, default):
        raw = values.get(name)
        if raw is None:
            return default
        try:
            value = float(raw)
        except ValueError:
            raise PluginError(common.INVALID_ARGUMENT) from None
        if not math.isfinite(value) or value <= 0:
            raise PluginError(common.INVALID_ARGUMENT)
        return value

    return Options(endpoint, token,
                   io_timeout=seconds('PLUGIN_IO_TIMEOUT', 5.0),
                   hook_timeout=seconds('PLUGIN_HOOK_TIMEOUT', 5.0),
                   heartbeat_interval=seconds('PLUGIN_HEARTBEAT_INTERVAL', 1.0),
                   retry_delay=seconds('PLUGIN_RETRY_DELAY', 1.0))


async def run(manifest_path):
    try:
        manifest = load_manifest(manifest_path)
        module, factory = manifest.entrypoint.split(':')
        plugin = getattr(importlib.import_module(module), factory)()
        if not isinstance(plugin, Plugin):
            raise PluginError(common.INVALID_ARGUMENT)
        options = options_from_environment()
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
