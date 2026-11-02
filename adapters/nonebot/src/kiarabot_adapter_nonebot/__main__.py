from __future__ import annotations

import asyncio
import logging
import os
import signal
import sys
from contextlib import suppress
from urllib.parse import urlparse

import nonebot
from nonebot.adapters.onebot.v11 import Adapter, Bot, MessageEvent, RequestEvent
from kiarabot.v1 import common_pb2 as common

from .actions import ActionExecutor
from .bridge import BridgeConfig
from .client import BridgeClient
from .health import HealthServer
from .mapper import AdapterError, map_message, map_request
from .outbox import Outbox
from .platform_errors import classify_retcode
from .status import AdapterStatusReporter


def platform_url(value: str) -> str:
    parsed = urlparse(value)
    if (parsed.scheme not in ('ws', 'wss') or not parsed.hostname or parsed.username
            or parsed.password or parsed.query or parsed.fragment):
        raise AdapterError(common.INVALID_ARGUMENT)
    return value


def configure():
    config = BridgeConfig.from_environment(os.environ)
    platform_url(os.environ['ONEBOT_WS_URL'])
    token = os.environ['ONEBOT_ACCESS_TOKEN']
    if not token:
        raise AdapterError(common.INVALID_ARGUMENT)
    # Runtime account ids are canonical UUIDs, while the OneBot bot key is the platform self_id
    # (a decimal QQ id). Deployments with a UUID ACCOUNT_ID must provide the platform self id
    # separately; otherwise NoneBot's get_bot lookup can never find the connected bot.
    platform_self_id = os.environ.get('ONEBOT_SELF_ID') or config.account_id
    if not platform_self_id or len(platform_self_id) > 128:
        raise AdapterError(common.INVALID_ARGUMENT)
    # Disable NoneBot's INFO event descriptions; never log platform payloads.
    nonebot.init(driver='~websockets', onebot_ws_urls=[os.environ['ONEBOT_WS_URL']],
                 onebot_access_token=token, log_level='WARNING')
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
            bot = nonebot.get_bot(platform_self_id)
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
    executor = ActionExecutor(os.environ['OUTBOX_PATH'] + '.actions', config.adapter_id,
                              config.account_id, call_platform)

    def stop_process():
        # A terminal bridge failure (revoked/expired bootstrap, rejected registration) must end
        # the adapter process with a non-zero status after the normal graceful shutdown path.
        signal.raise_signal(signal.SIGTERM)

    client = BridgeClient(config, outbox, executor, status, on_fatal=stop_process)
    stop = asyncio.Event()
    tasks = []
    matcher = nonebot.on_message(priority=1, block=False)

    def ready_snapshot():
        authorized = client.authorized
        platform_connected = bool(nonebot.get_bots())
        return {
            'ready': authorized and platform_connected,
            'components': {
                'core': 'HEALTHY' if authorized else 'UNHEALTHY',
                'authorization': 'HEALTHY' if authorized else 'UNHEALTHY',
                'platform': 'HEALTHY' if platform_connected else 'DEGRADED',
            },
        }

    health_port = int(os.environ.get('HEALTH_PORT', '8081'))
    if not 0 <= health_port <= 65535:
        raise AdapterError(common.INVALID_ARGUMENT)
    health = HealthServer(ready_snapshot, host='127.0.0.1', port=health_port)

    async def serve_health():
        try:
            await health.start()
        except OSError:
            # A busy port must not kill the adapter; health is reported by absence.
            logging.getLogger(__name__).error('operation=health_listen error_code=UPSTREAM_UNAVAILABLE')

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
            outbox.enqueue(map_message(raw, config.adapter_id, config.account_id, platform_self_id))
        except AdapterError as error:
            logging.getLogger(__name__).warning('operation=event_enqueue error_code=%s',
                                               common.ErrorCode.Name(error.code))

    request_matcher = nonebot.on_request(priority=1, block=False)

    @request_matcher.handle()
    async def receive_request(bot: Bot, event: RequestEvent):
        try:
            raw = event.model_dump(mode='json')
            mapped = map_request(raw, config.adapter_id, config.account_id, platform_self_id)
            executor.remember_request(mapped.friend_request.request_id, raw['flag'])
            outbox.enqueue(mapped)
        except AdapterError as error:
            logging.getLogger(__name__).warning('operation=request_enqueue error_code=%s',
                                               common.ErrorCode.Name(error.code))

    @driver.on_startup
    async def startup():
        tasks.append(asyncio.create_task(client.run(stop)))
        tasks.append(asyncio.create_task(serve_health()))

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
    return driver, client


def main():
    client = None
    try:
        driver, client = configure()
        nonebot.run()
    except Exception:
        logging.getLogger(__name__).error('operation=adapter_process error_code=INTERNAL')
        raise SystemExit(1) from None
    if client is not None and client.fatal:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
