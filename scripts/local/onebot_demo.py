#!/usr/bin/env python3
"""Loopback-only OneBot V11 demo; never connects to QQ, Core or a model provider."""
from __future__ import annotations

import argparse
import asyncio
from collections import deque
import hmac
import json
import logging
import os
from pathlib import Path
import re
import signal
import stat
import sys
import time

REQUEST_LIMIT = 8192
RESPONSE_LIMIT = 131072


class DemoError(Exception):
    pass


def positive(value):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 < value < 2**63:
        raise DemoError('invalid_id')
    return value


def token_file(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o600:
            raise DemoError('unsafe_token_file')
        token = os.read(descriptor, 130).decode('ascii').removesuffix('\n')
        if info.st_size > 129 or not re.fullmatch(r'[A-Za-z0-9_-]{32,128}', token):
            raise DemoError('invalid_token_file')
        return token
    finally:
        os.close(descriptor)


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode('utf-8') + b'\n'


def decode_json(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise DemoError('invalid_request')
            result[key] = value
        return result

    def reject_constant(_value):
        raise DemoError('invalid_request')

    return json.loads(data, object_pairs_hook=unique, parse_constant=reject_constant)


def text_message(message):
    if isinstance(message, str):
        text = message
    elif isinstance(message, list) and 0 < len(message) <= 16:
        parts = []
        for segment in message:
            if (not isinstance(segment, dict) or set(segment) != {'type', 'data'} or
                    segment['type'] != 'text' or not isinstance(segment['data'], dict) or
                    set(segment['data']) != {'text'} or not isinstance(segment['data']['text'], str)):
                raise DemoError('unsupported_message')
            parts.append(segment['data']['text'])
        text = ''.join(parts)
    else:
        raise DemoError('invalid_message')
    if not text or '\x00' in text or len(text.encode('utf-8')) > 4096:
        raise DemoError('invalid_message')
    return text


class Demo:
    def __init__(self, account, token):
        self.account = positive(account)
        self.token = token
        self.socket = None
        self.replies = deque(maxlen=16)
        self.sequence = 0
        self.reply_sequence = 100000
        self.pending = None
        self.control_tasks = set()

    def authenticate(self, connection, request):
        authorization = request.headers.get_all('Authorization')
        if len(authorization) != 1 or not hmac.compare_digest(authorization[0].encode('utf-8'), ('Bearer ' + self.token).encode('ascii')):
            return connection.respond(401, 'unauthorized\n')
        if self.socket is not None:
            return connection.respond(409, 'already_connected\n')
        return None

    async def websocket(self, socket):
        if self.socket is not None:
            await socket.close(1013, 'already_connected')
            return
        self.socket = socket
        try:
            await socket.send(encoded(dict(time=int(time.time()), self_id=self.account,
                post_type='meta_event', meta_event_type='lifecycle', sub_type='connect')).decode())
            async for raw in socket:
                call = None
                try:
                    call = decode_json(raw)
                    if not isinstance(call, dict):
                        raise DemoError('invalid_request')
                    action = call.get('action')
                    params = call.get('params', {})
                    if not isinstance(params, dict):
                        raise DemoError('invalid_params')
                    if action in ('get_login_info', 'get_version_info', 'get_status') and params:
                        raise DemoError('invalid_params')
                    if action == 'get_login_info':
                        data = dict(user_id=self.account, nickname='KiaraBot local demo')
                    elif action == 'get_version_info':
                        data = dict(app_name='kiarabot-local-demo', app_version='1', protocol_version='v11')
                    elif action == 'get_status':
                        data = dict(online=True, good=True)
                    elif action in ('send_group_msg', 'send_private_msg'):
                        target = 'group_id' if action == 'send_group_msg' else 'user_id'
                        if not set(params) <= {target, 'message', 'auto_escape'} or params.get('auto_escape', False) is not False:
                            raise DemoError('invalid_params')
                        positive(params.get(target))
                        text_message(params.get('message'))
                        self.reply_sequence += 1
                        reply = dict(action=action, params=params, message_id=self.reply_sequence)
                        self.replies.append(reply)
                        data = dict(message_id=self.reply_sequence)
                        if self.pending:
                            expected_action, expected_id, future = self.pending
                            if action == expected_action and params[target] == expected_id and not future.done():
                                future.set_result(reply)
                    else:
                        raise DemoError('unsupported_action')
                    response = dict(status='ok', retcode=0, data=data, echo=call.get('echo'))
                except (DemoError, ValueError, TypeError, UnicodeError, RecursionError) as error:
                    code = str(error) if isinstance(error, DemoError) else 'invalid_request'
                    response = dict(status='failed', retcode=1404 if code == 'unsupported_action' else 1400,
                                    data=None, message=code,
                                    echo=call.get('echo') if isinstance(locals().get('call'), dict) else None)
                await socket.send(encoded(response).decode())
        finally:
            self.socket = None
            if self.pending and not self.pending[2].done():
                self.pending[2].set_exception(DemoError('platform_disconnected'))

    async def command(self, request):
        if not isinstance(request, dict) or not isinstance(request.get('token'), str):
            raise DemoError('unauthorized')
        if not hmac.compare_digest(request['token'].encode('utf-8'), self.token.encode('ascii')):
            raise DemoError('unauthorized')
        if request.get('command') == 'status' and set(request) == {'command', 'token'}:
            return dict(ok=True, connected=self.socket is not None, account_id=str(self.account), replies=list(self.replies))
        if request.get('command') != 'message' or not set(request) <= {
                'command', 'token', 'message_type', 'user_id', 'group_id', 'text', 'timeout'}:
            raise DemoError('invalid_request')
        kind = request.get('message_type')
        if kind not in ('group', 'private'):
            raise DemoError('invalid_message_type')
        user = positive(request.get('user_id'))
        if user == self.account:
            raise DemoError('invalid_user')
        target = positive(request.get('group_id')) if kind == 'group' else user
        text = request.get('text')
        if not isinstance(text, str) or len(text.encode('utf-8')) > 2048:
            raise DemoError('invalid_message')
        text_message(text)
        timeout = request.get('timeout', 10)
        if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not 0 < timeout <= 30:
            raise DemoError('invalid_timeout')
        if self.pending:
            raise DemoError('busy')
        if self.socket is None:
            raise DemoError('not_connected')
        self.sequence += 1
        event = dict(time=int(time.time()), self_id=self.account, post_type='message', message_type=kind,
                     sub_type='normal' if kind == 'group' else 'friend', user_id=user,
                     message_id=self.sequence, message=[dict(type='text', data=dict(text=text))],
                     raw_message=text, font=0, sender=dict(user_id=user, nickname=f'demo-{user}'))
        if kind == 'group':
            event['group_id'] = target
            event['sender']['role'] = 'member'
        future = asyncio.get_running_loop().create_future()
        self.pending = ('send_group_msg' if kind == 'group' else 'send_private_msg', target, future)
        try:
            await self.socket.send(encoded(event).decode())
            reply = await asyncio.wait_for(future, timeout)
            return dict(ok=True, event_message_id=event['message_id'], reply=reply)
        except TimeoutError:
            raise DemoError('reply_timeout') from None
        finally:
            self.pending = None

    async def control(self, reader, writer):
        task = asyncio.current_task()
        if len(self.control_tasks) >= 8:
            writer.close()
            await writer.wait_closed()
            return
        self.control_tasks.add(task)
        try:
            line = await asyncio.wait_for(reader.readline(), 3)
            if not line.endswith(b'\n') or len(line) > REQUEST_LIMIT:
                raise DemoError('invalid_request')
            result = await self.command(decode_json(line))
        except asyncio.CancelledError:
            writer.close()
            await writer.wait_closed()
            raise
        except (DemoError, ValueError, UnicodeError, TimeoutError, RecursionError):
            error = sys.exception()
            result = dict(ok=False, error=str(error) if isinstance(error, DemoError) else 'invalid_request')
        except Exception:
            result = dict(ok=False, error='platform_unavailable')
        finally:
            self.control_tasks.discard(task)
        try:
            writer.write(encoded(result))
            await asyncio.wait_for(writer.drain(), 2)
        finally:
            writer.close()
            await writer.wait_closed()


async def serve_demo(args, token):
    from websockets.asyncio.server import serve
    path = Path(args.control_socket).absolute()
    if len(os.fsencode(path)) > 100:
        raise DemoError('control_path_too_long')
    parent = path.parent.stat()
    if parent.st_uid != os.getuid() or stat.S_IMODE(parent.st_mode) != 0o700 or path.exists() or path.is_symlink():
        raise DemoError('unsafe_control_socket')
    demo = Demo(args.account_id, token)
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for signum in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(signum, stop.set)
    logger = logging.getLogger('onebot_demo_transport')
    logger.addHandler(logging.NullHandler())
    logger.propagate = False
    async with serve(demo.websocket, '127.0.0.1', args.port, process_request=demo.authenticate,
                     max_size=16384, max_queue=8, open_timeout=3, close_timeout=1,
                     compression=None, logger=logger) as websocket:
        control = await asyncio.start_unix_server(demo.control, path=str(path), limit=REQUEST_LIMIT)
        os.chmod(path, 0o600)
        inode = path.stat().st_ino
        try:
            async with control:
                print(encoded(dict(event='ready', host='127.0.0.1', port=websocket.sockets[0].getsockname()[1],
                                   account_id=str(demo.account), control_socket=str(path))).decode().strip(), flush=True)
                await stop.wait()
        finally:
            for task in tuple(demo.control_tasks):
                task.cancel()
            await asyncio.gather(*demo.control_tasks, return_exceptions=True)
            if path.exists() and path.stat().st_ino == inode:
                path.unlink()


async def client(args, token):
    reader, writer = await asyncio.open_unix_connection(args.control_socket, limit=RESPONSE_LIMIT)
    try:
        request = dict(command=args.command, token=token)
        if args.command == 'message':
            request.update(message_type=args.type, user_id=args.user_id, group_id=args.group_id,
                           text=args.text, timeout=args.timeout)
        wire = encoded(request)
        if len(wire) > REQUEST_LIMIT:
            raise DemoError('invalid_request')
        writer.write(wire)
        await writer.drain()
        line = await asyncio.wait_for(reader.readline(), getattr(args, 'timeout', 10) + 4)
        if not line or len(line) > RESPONSE_LIMIT:
            raise DemoError('invalid_response')
        return json.loads(line)
    finally:
        writer.close()
        await writer.wait_closed()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('serve', 'status', 'message'):
        sub = commands.add_parser(name)
        sub.add_argument('--token-file', required=True)
        sub.add_argument('--control-socket', required=True)
        if name == 'serve':
            sub.add_argument('--port', type=int, default=0)
            sub.add_argument('--account-id', type=int, default=123)
        if name == 'message':
            sub.add_argument('--type', choices=('group', 'private'), default='group')
            sub.add_argument('--user-id', type=int, default=789)
            sub.add_argument('--group-id', type=int, default=456)
            sub.add_argument('--text', required=True)
            sub.add_argument('--timeout', type=float, default=10)
    args = parser.parse_args()
    try:
        token = token_file(args.token_file)
        if args.command == 'serve':
            if not 0 <= args.port <= 65535:
                raise DemoError('invalid_port')
            asyncio.run(serve_demo(args, token))
            return 0
        if args.command == 'message' and not 0 < args.timeout <= 30:
            raise DemoError('invalid_timeout')
        result = asyncio.run(client(args, token))
        print(encoded(result).decode().strip())
        return 0 if result.get('ok') is True else 1
    except (DemoError, OSError, ValueError, UnicodeError):
        error = sys.exception()
        print(encoded(dict(ok=False, error=str(error) if isinstance(error, DemoError) else 'local_demo_unavailable')).decode().strip())
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
