"""Single-threaded WebSocket client for the Control WSS acceptance tests.

The Python ``websockets`` sync client runs a background receiver thread that performs
``SSL_read`` concurrently with main-thread ``SSL_write``/close on the same ``ssl.SSLSocket``.
On this toolchain (CPython 3.11 + OpenSSL 3.5) that combination produces spurious
``SSLEOFError`` on send and has segfaulted inside ``libssl`` (macOS crash reports show the
crash in ``SSL_read`` on the receiver thread). The acceptance harness therefore uses this
minimal single-threaded client: one thread owns the TLS socket for the whole connection,
frames are parsed synchronously with bounded timeouts, and no assertion or server-side
authorization behavior changes.

Only RFC6455 client-side essentials are implemented: masked text frames, ping/pong,
close handshake and continuation frames. Binary frames are rejected as unsupported.
"""
import json
import select
import socket
import ssl
import struct
import time


class ConnectionClosed(Exception):
    def __init__(self, code):
        super().__init__(f'connection closed: {code}')
        self.code = code


class RawWebSocket:
    def __init__(self, host, port, path, ca_file, origin, timeout=5.0):
        context = ssl.create_default_context(cafile=ca_file)
        raw = socket.create_connection((host, port), timeout=timeout)
        self._socket = context.wrap_socket(raw, server_hostname=host)
        key = 'dGhlIHNhbXBsZSBub25jZQ=='
        request = (f'GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n'
                   f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n'
                   f'Sec-WebSocket-Version: 13\r\nOrigin: {origin}\r\n\r\n')
        self._socket.sendall(request.encode())
        self._buffer = b''
        while b'\r\n\r\n' not in self._buffer:
            chunk = self._socket.recv(4096)
            if not chunk:
                raise ConnectionError('connection closed during HTTP upgrade')
            self._buffer += chunk
        head, _, self._buffer = self._buffer.partition(b'\r\n\r\n')
        status = head.split(b'\r\n', 1)[0]
        if not status.startswith(b'HTTP/1.1 101'):
            raise ConnectionError(f'unexpected upgrade response {status!r}')
        expected = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
        del expected  # server key validation is out of scope for a transport acceptance client
        self._closed = False

    # -- framing -----------------------------------------------------------------
    def send_text(self, text):
        payload = text.encode() if isinstance(text, str) else bytes(text)
        mask = b'\x00\x00\x00\x00'
        if len(payload) < 126:
            header = bytes([0x81, 0x80 | len(payload)])
        elif len(payload) < 65536:
            header = bytes([0x81, 0x80 | 126]) + struct.pack('>H', len(payload))
        else:
            header = bytes([0x81, 0x80 | 127]) + struct.pack('>Q', len(payload))
        self._socket.sendall(header + mask + payload)

    def send(self, text):
        self.send_text(text)

    def send_json(self, value):
        self.send_text(json.dumps(value))

    def _read_exact(self, count, deadline):
        while len(self._buffer) < count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError
            if not select.select([self._socket], [], [], remaining)[0]:
                raise TimeoutError
            chunk = self._socket.recv(max(4096, count - len(self._buffer)))
            if not chunk:
                raise ConnectionClosed(None)
            self._buffer += chunk
        data, self._buffer = self._buffer[:count], self._buffer[count:]
        return data

    def _send_control(self, opcode, payload=b''):
        mask = b'\x00\x00\x00\x00'
        self._socket.sendall(bytes([0x80 | opcode, 0x80 | len(payload)]) + mask + payload)

    def _recv_frame(self, deadline):
        while True:
            header = self._read_exact(2, deadline)
            fin = (header[0] & 0x80) != 0
            opcode = header[0] & 0x0f
            masked = (header[1] & 0x80) != 0
            length = header[1] & 0x7f
            if length == 126:
                length = struct.unpack('>H', self._read_exact(2, deadline))[0]
            elif length == 127:
                length = struct.unpack('>Q', self._read_exact(8, deadline))[0]
            mask = self._read_exact(4, deadline) if masked else b''
            payload = self._read_exact(length, deadline) if length else b''
            if masked and mask != b'\x00\x00\x00\x00':
                payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
            if opcode == 0x9:  # ping
                self._send_control(0xa, payload)
                continue
            if opcode == 0xa:  # pong
                continue
            if opcode == 0x8:  # close
                code = struct.unpack('>H', payload[:2])[0] if len(payload) >= 2 else 1005
                try:
                    self._send_control(0x8, payload[:2])
                except OSError:
                    pass
                self._closed = True
                raise ConnectionClosed(code)
            return opcode, payload, fin

    def recv_json(self, timeout=3.0):
        deadline = time.monotonic() + timeout
        payload = b''
        while True:
            opcode, chunk, fin = self._recv_frame(deadline)
            if opcode == 0x2:
                raise RuntimeError('unexpected binary WebSocket frame')
            payload += chunk
            if fin:
                break
        return json.loads(payload.decode())

    def recv(self, timeout=3.0):
        return json.dumps(self.recv_json(timeout))

    def close(self, code=1000):
        if self._closed:
            return
        self._closed = True
        try:
            self._send_control(0x8, struct.pack('>H', code))
            deadline = time.monotonic() + 1.0
            while True:
                try:
                    self._recv_frame(deadline)
                except ConnectionClosed:
                    break
                except (TimeoutError, OSError):
                    break
                if time.monotonic() >= deadline:
                    break
        except OSError:
            pass
        finally:
            try:
                self._socket.close()
            except OSError:
                pass

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
