from __future__ import annotations

import asyncio
import logging
import os
import sys
from contextlib import suppress
from urllib.parse import urlparse

import nonebot
from nonebot.adapters.onebot.v11 import Adapter, Bot, MessageEvent
from kiarabot.v1 import common_pb2 as common

from .client import ClientConfig, CoreClient
from .actions import ActionExecutor
from .health import HealthServer
from .mapper import AdapterError, map_message
from .outbox import Outbox
from .platform_errors import classify_retcode
from .status import AdapterStatusReporter


def configure():
    config = ClientConfig(os.environ['CORE_GRPC_ENDPOINT'], os.environ['CORE_AUTH_TOKEN'],
                          os.environ['ADAPTER_ID'], os.environ['ACCOUNT_ID'])
    url = os.environ['ONEBOT_WS_URL']
    parsed = urlparse(url)
    if parsed.scheme not in ('ws', 'wss') or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise AdapterError(common.INVALID_ARGUMENT)
    token = os.environ['ONEBOT_ACCESS_TOKEN']
    if not token:
        raise AdapterError(common.INVALID_ARGUMENT)
    # Disable NoneBot's INFO event descriptions; never log platform payloads.
    nonebot.init(driver='~websockets', onebot_ws_urls=[url], onebot_access_token=token,
                 log_level='WARNING')
    # Framework failures can include platform bodies/URLs in exception text.
    # Keep severity, but strip free text and traceback from the shared Loguru sink.
    from nonebot.log import logger
    def sanitize(record):
        record['message'] = ''
        record['exception'] = None
        return True
    logger.remove()
    logger.add(sys.stderr, level='WARNING', filter=sanitize,
               format='operation=nonebot_log level={level}', backtrace=False, diagnose=False)
    driver = nonebot.get_driver()
    driver.register_adapter(Adapter)
    outbox = Outbox(os.environ['OUTBOX_PATH'])
    status = AdapterStatusReporter(outbox, config.adapter_id, config.account_id)
    async def call_platform(api, params):
        from nonebot.adapters.onebot.v11.exception import ActionFailed
        try:
            bot = nonebot.get_bot(config.account_id)
        except KeyError:
            raise AdapterError(common.UPSTREAM_UNAVAILABLE) from None
        try:
            return await bot.call_api(api, **params)
        except ActionFailed as failure:
            # Platform answered and rejected the call: reachable but degraded.
            retcode = failure.info.get('retcode') if isinstance(failure.info, dict) else None
            code = classify_retcode(retcode)
            if code in (common.PERMISSION_DENIED, common.RATE_LIMITED):
                status.report(common.DEGRADED, 'platform_action_failed')
            raise AdapterError(code) from None
    executor = ActionExecutor(os.environ['OUTBOX_PATH'] + '.actions', config.adapter_id, config.account_id, call_platform)
    client = CoreClient(config, outbox, executor, status)
    stop = asyncio.Event()
    tasks = []
    matcher = nonebot.on_message(priority=1, block=False)

    def ready_snapshot():
        core_connected = client.instance is not None
        platform_connected = bool(nonebot.get_bots())
        return {
            'ready': core_connected,
            'components': {
                'core': 'HEALTHY' if core_connected else 'UNHEALTHY',
                'platform': 'HEALTHY' if platform_connected else 'DEGRADED',
            },
        }

    health_port = int(os.environ.get('HEALTH_PORT', '8081'))
    if not 0 <= health_port <= 65535:
        raise AdapterError(common.INVALID_ARGUMENT)
    health = HealthServer(ready_snapshot, host='127.0.0.1', port=health_port)

    @driver.on_bot_connect
    async def bot_connected(bot: Bot):
        status.report(common.HEALTHY, 'platform_connected')

    @driver.on_bot_disconnect
    async def bot_disconnected(bot: Bot):
        status.report(common.DEGRADED, 'platform_disconnected')

    @matcher.handle()
    async def receive(bot: Bot, event: MessageEvent):
        try:
            raw = event.model_dump(mode='json')
            outbox.enqueue(map_message(raw, config.adapter_id, config.account_id))
        except AdapterError as error:
            logging.getLogger(__name__).warning('operation=event_enqueue error_code=%s',
                                               common.ErrorCode.Name(error.code))

    @driver.on_startup
    async def startup():
        tasks.append(asyncio.create_task(client.run(stop)))
        tasks.append(asyncio.create_task(health.start()))

    @driver.on_shutdown
    async def shutdown():
        stop.set()
        for task in tasks:
            task.cancel()
        for task in tasks:
            with suppress(asyncio.CancelledError):
                await task
        await health.close()
        executor.close()
        outbox.close()
    return driver


def main():
    try:
        configure()
        nonebot.run()
    except Exception:
        logging.getLogger(__name__).error('operation=adapter_process error_code=INTERNAL')
        raise SystemExit(1) from None


if __name__ == '__main__':
    main()
