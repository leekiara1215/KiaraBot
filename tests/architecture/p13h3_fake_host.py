#!/usr/bin/env python3
"""Test-only HostBridge client used for P13h-3 negative matrix (UNKNOWN/KNOWN receipt)."""
from __future__ import annotations

import argparse
import asyncio
import json
import os
from pathlib import Path
import signal
import sys
import time

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'sdk/python/build/generated'))


async def read(stream, timeout=15.0):
    value = await asyncio.wait_for(stream.read(), timeout)
    if value is None:
        raise RuntimeError('host stream closed')
    return value


async def main_async(args) -> None:
    import grpc
    from google.protobuf.timestamp_pb2 import Timestamp
    from kiarabot.v1 import host_pb2, host_pb2_grpc, platform_pb2
    ca = Path(args.ca).read_bytes()
    certificate = Path(args.cert).read_bytes()
    key = Path(args.key).read_bytes()
    channel = grpc.aio.secure_channel(
        args.endpoint, grpc.ssl_channel_credentials(
            root_certificates=ca, private_key=key, certificate_chain=certificate))
    stub = host_pb2_grpc.HostBridgeStub(channel)
    stream = stub.Connect()
    await stream.write(host_pb2.HostToRuntime(
        registration=host_pb2.HostRegisterHostRequest(
            host=host_pb2.HostRecord(host_id=args.host_id))))
    frame = await read(stream)
    if frame.WhichOneof('body') != 'registered':
        raise RuntimeError('registration rejected')
    fence = frame.registered.fence
    Path(args.ready).write_text('ready')
    stop_seen = None
    while stop_seen is None:
        now = Timestamp()
        now.GetCurrentTime()
        await stream.write(host_pb2.HostToRuntime(
            fence=fence, heartbeat=host_pb2.HostHeartbeat(fence=fence, sent_at=now)))
        frame = await read(stream, timeout=5.0)
        if frame.WhichOneof('body') == 'stop':
            stop_seen = frame.stop
    record = {'command_id': stop_seen.command_id, 'instance_id': stop_seen.instance_id}
    Path(args.state).write_text(json.dumps(record))
    execution = host_pb2.ExecutionRecord(
        host_id=args.host_id, instance_id=stop_seen.instance_id,
        execution_generation=stop_seen.execution_generation or 1,
        desired_version=1,
        outcome=(platform_pb2.EXECUTION_UNKNOWN if args.mode == 'unknown'
                 else platform_pb2.EXECUTION_KNOWN_SUCCEEDED),
        process_marker='p13h3-fake-host', command_id=stop_seen.command_id)
    execution.observed_at.GetCurrentTime()
    await stream.write(host_pb2.HostToRuntime(
        fence=fence, receipt=host_pb2.HostCommandReceipt(
            command_id=stop_seen.command_id, persisted=True, execution=execution)))
    await asyncio.sleep(0.5)
    await stream.done_writing()
    await channel.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--endpoint', required=True)
    parser.add_argument('--ca', required=True)
    parser.add_argument('--cert', required=True)
    parser.add_argument('--key', required=True)
    parser.add_argument('--host-id', required=True)
    parser.add_argument('--mode', choices=('known', 'unknown'), required=True)
    parser.add_argument('--ready', required=True)
    parser.add_argument('--state', required=True)
    args = parser.parse_args()
    try:
        asyncio.run(main_async(args))
        return 0
    except Exception as error:  # noqa: BLE001
        print('p13h3 fake host failure: ' + ' '.join(str(error).split())[:500], file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
