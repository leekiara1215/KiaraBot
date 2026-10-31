from __future__ import annotations

import asyncio
import json


class HealthServer:
    """Minimal HTTP probe endpoint for compose/systemd health checks.

    ``/health/live`` reports process liveness only; ``/health/ready`` asks the
    caller-supplied snapshot function whether the adapter can accept work. The
    state function must return ``{"ready": bool, "components": {name: state}}``.
    """

    def __init__(self, state, host: str = '127.0.0.1', port: int = 8081):
        if not isinstance(port, int) or not 0 <= port <= 65535:
            raise ValueError('invalid health port')
        self.state, self.host, self.port = state, host, port
        self.server = None
        self.bound_port = None

    async def start(self):
        self.server = await asyncio.start_server(self._handle, self.host, self.port)
        sockets = self.server.sockets or []
        self.bound_port = sockets[0].getsockname()[1] if sockets else self.port
        return self

    async def close(self) -> None:
        if self.server is None:
            return
        self.server.close()
        await self.server.wait_closed()
        self.server = None

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            request = await asyncio.wait_for(reader.readline(), 5)
            parts = request.decode('ascii', 'replace').split()
            path = parts[1] if len(parts) >= 2 else ''
            while True:
                line = await asyncio.wait_for(reader.readline(), 5)
                if not line or line in (b'\r\n', b'\n'):
                    break
            if path == '/health/live':
                status, body = 200, {'state': 'HEALTHY'}
            elif path == '/health/ready':
                snapshot = self.state()
                ready = bool(snapshot.get('ready'))
                status = 200 if ready else 503
                body = {'state': 'READY' if ready else 'NOT_READY',
                        'components': snapshot.get('components', {})}
            else:
                status, body = 404, {'state': 'NOT_FOUND'}
            payload = json.dumps(body, separators=(',', ':'), sort_keys=True).encode()
            reason = {200: 'OK', 503: 'Service Unavailable', 404: 'Not Found'}[status]
            head = (f'HTTP/1.1 {status} {reason}\r\n'
                    'Content-Type: application/json\r\n'
                    f'Content-Length: {len(payload)}\r\n'
                    'Connection: close\r\n\r\n').encode()
            writer.write(head + payload)
            await writer.drain()
        except (asyncio.TimeoutError, ConnectionError, ValueError, OSError):
            pass
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError):
                pass
