"""Loopback protocol fake for plugin tests; never a substitute for Core authorization."""
import asyncio
from contextlib import suppress

import grpc
from kiarabot.v1 import plugins_pb2 as plugins, services_pb2_grpc as services


class FakeHost(services.PluginBridgeServicer):
    def __init__(self, token='t' * 32):
        self.token = token
        self.registrations = asyncio.Queue()
        self.acknowledgements = asyncio.Queue()
        self.config_acknowledgements = asyncio.Queue()
        self.outgoing = asyncio.Queue()
        self.grants = None
        self.features = []
        self.actions = asyncio.Queue()
        self.host_requests = asyncio.Queue()
        self.rejection = None
        self.server = grpc.aio.server()
        services.add_PluginBridgeServicer_to_server(self, self.server)
        port = self.server.add_insecure_port('127.0.0.1:0')
        self.endpoint = f'127.0.0.1:{port}'

    async def __aenter__(self):
        await self.server.start()
        return self

    async def __aexit__(self, *_):
        await self.server.stop(0)

    async def Connect(self, requests, context):
        if dict(context.invocation_metadata()).get('authorization') != 'Bearer ' + self.token:
            await context.abort(grpc.StatusCode.UNAUTHENTICATED, 'PERMISSION_DENIED')
        first = await anext(requests)
        if first.WhichOneof('body') != 'register':
            await context.abort(grpc.StatusCode.INVALID_ARGUMENT, 'INVALID_ARGUMENT')
        registration = first.register
        await self.registrations.put(registration)
        if self.rejection:
            yield plugins.CoreToPlugin(register_result=plugins.RegisterPluginResponse(error=self.rejection))
            return
        yield plugins.CoreToPlugin(register_result=plugins.RegisterPluginResponse(
            accepted=True, required_api_version='kiarabot.plugin.v1',
            granted_permissions=registration.manifest.permissions if self.grants is None else self.grants,
            enabled_features=self.features))
        responses = asyncio.Queue()

        async def read():
            async for request in requests:
                if request.WhichOneof('body') == 'event_ack':
                    await self.acknowledgements.put(request.event_ack)
                elif request.WhichOneof('body') == 'config_ack':
                    await self.config_acknowledgements.put(request.config_ack)
                elif request.WhichOneof('body') in ('action', 'action_query', 'action_cancel'):
                    await self.actions.put(request)
                elif request.WhichOneof('body') == 'host_request':
                    await self.host_requests.put(request.host_request)
                elif request.WhichOneof('body') == 'heartbeat':
                    await responses.put(plugins.CoreToPlugin(heartbeat_ack=plugins.PluginHeartbeatAck()))
        async def forward():
            while True:
                await responses.put(await self.outgoing.get())
        reader, sender = asyncio.create_task(read()), asyncio.create_task(forward())
        try:
            while True:
                frame = await responses.get()
                if frame is None:
                    return
                yield frame
        finally:
            for task in (reader, sender):
                task.cancel()
                with suppress(asyncio.CancelledError, Exception):
                    await task
