#!/usr/bin/env python3
"""Deterministic BOOT-05 ACK-loss fault endpoint (test-side real TCP relay).

The frozen bootstrap-delivery contract has no injection hook in the Control process: the sink write
and the no-secret report are two sequential steps of one HTTP request, so the only contract-visible
way to lose the report is a real transport failure between Control and Runtime. This helper is a
plain TCP relay the *fixture configuration* points Control's `control_api.runtime_endpoint` at; it
never parses or modifies traffic (TLS stays end-to-end between the real Control and real Runtime
processes) and it never logs payload bytes.

Behavior:
  * forwards Runtime -> Control immediately;
  * delays whole Control -> Runtime message frames by `--delay-ms`. Small TLS/HTTP2 control frames
    stay immediate, so ordinary requests do not accumulate the delay in a round-trip pipeline,
    while an RPC message that is already queued when the trigger fires cannot reach Runtime;
  * a watcher process busy-polls `--watch` (the fixed-directory 0600 sink file). The instant the
    sink file becomes visible, it SIGKILLs the relay and then writes `--marker`. The sink write and
    the report are separated by the sink's receipt fsync, so the relay is dead before the report
    call can complete: the report is deterministically lost while the sink file is already durable.

Exit codes: 42 = ACK-loss triggered (expected by the driver); 3 = watch timeout; 4 = relay died
before the trigger; 2 = usage error; the driver stops it by SIGTERM during cleanup. This is a test
fixture only: no product source, proto, registry, API or CMake input is touched.
"""
import argparse
import os
import select
import signal
import socket
import sys
import threading
import time

TRIGGERED_EXIT = 42
WATCH_TIMEOUT_EXIT = 3
RELAY_DIED_EXIT = 4


def endpoint(value):
    host, separator, port = value.rpartition(':')
    if not separator or not host or not port.isdigit():
        raise argparse.ArgumentTypeError(f'invalid endpoint {value!r}')
    return host, int(port)


def pump(source, destination, delay_seconds, delay_min_bytes):
    try:
        while True:
            data = source.recv(65536)
            if not data:
                try:
                    destination.shutdown(socket.SHUT_WR)
                except OSError:
                    pass
                return
            if delay_seconds > 0 and len(data) >= delay_min_bytes:
                time.sleep(delay_seconds)
            destination.sendall(data)
    except OSError:
        return


def relay_connection(client, target, delay_seconds, delay_min_bytes):
    try:
        client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        upstream = socket.create_connection(target, timeout=10)
        upstream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError as error:
        sys.stderr.write(f'RELAY_CONNECT_FAILED {error}\n')
        sys.stderr.flush()
        client.close()
        return
    forward = threading.Thread(
        target=pump, args=(client, upstream, delay_seconds, delay_min_bytes), daemon=True)
    backward = threading.Thread(target=pump, args=(upstream, client, 0, 0), daemon=True)
    forward.start()
    backward.start()
    backward.join()
    try:
        client.close()
    except OSError:
        pass
    try:
        upstream.close()
    except OSError:
        pass


def relay_main(listener, target, delay_seconds, delay_min_bytes, parent_pid):
    while True:
        if os.getppid() != parent_pid:
            os._exit(0)
        try:
            readable, _, _ = select.select([listener], [], [], 0.5)
        except InterruptedError:
            continue
        if not readable:
            continue
        try:
            client, _ = listener.accept()
        except OSError:
            continue
        threading.Thread(target=relay_connection,
                         args=(client, target, delay_seconds, delay_min_bytes),
                         daemon=True).start()


def write_marker(path):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
    try:
        os.write(descriptor, b'ack-loss-triggered\n')
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def watch(relay_pid, watch_path, marker_path, timeout_seconds):
    signal.signal(signal.SIGTERM, lambda *_: terminate(relay_pid))
    signal.signal(signal.SIGINT, lambda *_: terminate(relay_pid))
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if os.path.lexists(watch_path):
            # Kill the relay first: the report frame must never reach Runtime. Only then publish
            # the durable trigger marker for the driver.
            try:
                os.kill(relay_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(relay_pid, 0)
            except ChildProcessError:
                pass
            write_marker(marker_path)
            sys.stdout.write(f'RELAY_TRIGGERED watch={watch_path}\n')
            sys.stdout.flush()
            return TRIGGERED_EXIT
        joined, _ = os.waitpid(relay_pid, os.WNOHANG)
        if joined == relay_pid:
            sys.stderr.write('RELAY_DIED_BEFORE_TRIGGER\n')
            sys.stderr.flush()
            return RELAY_DIED_EXIT
        # The delayed Control->Runtime message frame gives this watcher a 60ms (default) window, so
        # a short sleep keeps the fixture polite without making the kill racy.
        time.sleep(0.0002)
    try:
        os.kill(relay_pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    sys.stderr.write('RELAY_WATCH_TIMEOUT\n')
    sys.stderr.flush()
    return WATCH_TIMEOUT_EXIT


def terminate(relay_pid):
    try:
        os.kill(relay_pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    try:
        os.waitpid(relay_pid, 0)
    except ChildProcessError:
        pass
    os._exit(143)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--listen', type=endpoint, required=True)
    parser.add_argument('--target', type=endpoint, required=True)
    parser.add_argument('--watch', required=True)
    parser.add_argument('--marker', required=True)
    parser.add_argument('--delay-ms', type=int, default=60)
    parser.add_argument('--delay-min-bytes', type=int, default=192)
    parser.add_argument('--watch-timeout', type=float, default=120.0)
    arguments = parser.parse_args()
    if arguments.delay_ms < 1 or arguments.delay_min_bytes < 1 or arguments.watch_timeout <= 0:
        parser.error('invalid delay/watch timeout')

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(arguments.listen)
    listener.listen(16)
    port = listener.getsockname()[1]
    sys.stdout.write(f'RELAY_READY listen_port={port} target={arguments.target[0]}:'
                     f'{arguments.target[1]} delay_ms={arguments.delay_ms}\n')
    sys.stdout.flush()

    relay_pid = os.fork()
    if relay_pid == 0:
        # The relay child owns the listening socket; the parent only watches the sink file.
        relay_main(listener, arguments.target, arguments.delay_ms / 1000.0,
                   arguments.delay_min_bytes, os.getppid())
        os._exit(0)
    listener.close()
    return watch(relay_pid, arguments.watch, arguments.marker, arguments.watch_timeout)


if __name__ == '__main__':
    sys.exit(main())
