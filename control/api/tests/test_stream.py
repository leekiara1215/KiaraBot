import json
import time

import pytest
from websockets.sync.client import connect
from websockets.exceptions import ConnectionClosed


def open_stream(server, token, channel="control", resume=None):
    socket = connect(f"ws://127.0.0.1:{server.port}/ws/{channel}", open_timeout=3)
    frame = {"type": "hello", "access_token": token, "last_sequence": 0}
    if resume:
        frame.update(type="resume", session_id=resume[0], last_sequence=resume[1])
    socket.send(json.dumps(frame))
    return socket


def test_stream_live_replay_snapshot_and_revocation(server):
    token = server.login()["access_token"]
    auth_session = server.request("/auth/me", token=token)[1]["session_id"]
    socket = open_stream(server, token)
    ready = json.loads(socket.recv(timeout=3))
    assert ready["type"] == "SESSION_READY" and ready["last_sequence"] == 0
    assert len(ready["session_id"]) >= 32
    # Subscribe before taking the REST snapshot; a concurrent change must be observable.
    snapshot = server.request("/plugins", token=token)[1]
    assert snapshot["items"][0]["state"] == "PLUGIN_STATE_STOPPED"
    assert server.request("/plugins/test.plugin/disable", "POST", token=token)[0] == 202
    event = json.loads(socket.recv(timeout=3))
    while event.get("event_type") != "plugin.updated":
        event = json.loads(socket.recv(timeout=3))
    assert event["event_type"] == "plugin.updated" and event["payload"]["state"] == "PLUGIN_STATE_DISABLED"
    assert event["sequence"] >= 1
    socket.close()
    assert server.request("/plugins/test.plugin/enable", "POST", token=token)[0] == 202
    time.sleep(0.2)
    socket = open_stream(server, token, resume=(ready["session_id"], 0))
    replay_ready = json.loads(socket.recv(timeout=3))
    assert replay_ready["type"] == "SESSION_READY" and replay_ready["last_sequence"] == 0
    replayed = json.loads(socket.recv(timeout=3))
    while replayed.get("event_type") != "plugin.updated":
        replayed = json.loads(socket.recv(timeout=3))
    assert replayed == event
    replayed = json.loads(socket.recv(timeout=3))
    while replayed.get("event_type") != "plugin.updated":
        replayed = json.loads(socket.recv(timeout=3))
    assert replayed["payload"]["state"] == "PLUGIN_STATE_STOPPED"
    assert server.request(f"/auth/sessions/{auth_session}/revoke", "POST", token=token)[0] == 204
    start = time.monotonic()
    with pytest.raises(ConnectionClosed) as failure:
        while True:
            socket.recv(timeout=1)
    assert failure.value.rcvd.code == 4401
    assert time.monotonic() - start < 1
    socket.close()


def test_stream_permissions_identity_and_restart(server):
    admin = server.login()["access_token"]
    viewer = server.login("viewer")["access_token"]
    for token, channel in [(viewer, "logs"), (admin, "messages")]:
        socket = open_stream(server, token, channel)
        with pytest.raises(ConnectionClosed) as failure:
            socket.recv(timeout=3)
        assert failure.value.rcvd.code == 4403
        socket.close()
    socket = open_stream(server, admin, "plugins")
    ready = json.loads(socket.recv(timeout=3))
    socket.close()
    wrong = open_stream(server, viewer, "plugins", (ready["session_id"], 0))
    with pytest.raises(ConnectionClosed) as failure:
        wrong.recv(timeout=3)
    assert failure.value.rcvd.code == 4403
    wrong.close()
    server.stop()
    server.start()
    socket = open_stream(server, admin, "plugins", (ready["session_id"], 0))
    assert json.loads(socket.recv(timeout=3)) == {"type": "RESYNC_REQUIRED", "reason": "SERVER_RESTART"}
    socket.close()


def test_stream_rejects_invalid_and_expired_tokens(server):
    socket = open_stream(server, "x" * 64)
    with pytest.raises(ConnectionClosed) as failure:
        socket.recv(timeout=3)
    assert failure.value.rcvd.code == 4401
    socket.close()
    token = server.login()["access_token"]
    socket = connect(f"ws://127.0.0.1:{server.port}/ws/control")
    socket.send(json.dumps({"type": "hello", "access_token": token, "last_sequence": -1}))
    with pytest.raises(ConnectionClosed) as failure:
        socket.recv(timeout=3)
    assert failure.value.rcvd.code == 4400
    socket.close()
