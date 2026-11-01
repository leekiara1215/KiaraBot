"""Opaque loopback TCP relay that drops the first response after an armed command.

TLS terminates at PostgreSQL, never in this relay. The test arms only after its last
pre-COMMIT SQL response; the next client record is COMMIT. Durable state is checked
through an independent direct connection, so a disconnect alone cannot pass the test.
"""
import select
import socket
import threading
from pathlib import Path


class CommitReceiptProxy:
    def __init__(self, upstream_port: int, root: Path):
        self.upstream_port = upstream_port
        self.root = root
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.stop = threading.Event()
        self.error = None
        self.thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, exception_type, exception, traceback):
        self.stop.set()
        self.thread.join(timeout=2)
        self.listener.close()
        if self.thread.is_alive():
            raise RuntimeError('commit fault relay did not stop within budget')
        if self.error is not None and exception is None:
            raise RuntimeError('commit fault relay failed') from self.error

    def _run(self):
        try:
            while not self.stop.is_set():
                try:
                    client, _ = self.listener.accept()
                    break
                except socket.timeout:
                    continue
            else:
                return
            with client, socket.create_connection(('127.0.0.1', self.upstream_port), timeout=3) as upstream:
                client.settimeout(2)
                upstream.settimeout(2)
                armed = False
                command_forwarded = False
                while not self.stop.is_set():
                    if not armed and (self.root / 'drop-commit-arm').is_file():
                        armed = True
                        (self.root / 'drop-commit-ready').write_text('ready')
                    readable, _, _ = select.select([client, upstream], [], [], 0.05)
                    for source in readable:
                        payload = source.recv(65536)
                        if not payload:
                            return
                        if source is client:
                            upstream.sendall(payload)
                            command_forwarded = command_forwarded or armed
                        elif command_forwarded:
                            (self.root / 'commit-receipt-dropped').write_text('dropped')
                            return  # PG sent COMMIT result; discard it and close both sockets.
                        else:
                            client.sendall(payload)
        except Exception as error:
            self.error = error
