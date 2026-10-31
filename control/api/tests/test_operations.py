import json
import os
import secrets
import sqlite3
import subprocess
import time
import urllib.error
import urllib.request

from conftest import BUILD, validate


def probe(server, path):
    try:
        response = urllib.request.urlopen(f"http://127.0.0.1:{server.port}{path}", timeout=3)
    except urllib.error.HTTPError as failure:
        response = failure
    return response.status, json.loads(response.read())


def test_liveness_independent_of_storage_and_readiness_write(server):
    for path in ("/health/live", "/health/ready"):
        status, body = probe(server, path)
        assert status == 200
        validate(body, "ProcessProbe")
    admin = server.login()["access_token"]
    connection = sqlite3.connect(server.directory / "runtime.db")
    try:
        connection.execute("BEGIN IMMEDIATE")
        assert probe(server, "/health/live")[0] == 200
        status, failure = probe(server, "/health/ready")
        assert status == 503 and failure["code"] == "UPSTREAM_UNAVAILABLE"
        assert server.request("/health", token=admin)[0] == 503
    finally:
        connection.rollback()
        connection.close()
    assert probe(server, "/health/ready")[0] == 200


def test_migration_repeat_corruption_and_lock(tmp_path):
    executable = BUILD / "infrastructure/kiarabot-migrate"
    path = tmp_path / "runtime.db"
    for _ in range(2):
        result = subprocess.run([executable, path], capture_output=True, timeout=4)
        assert result.returncode == 0
    connection = sqlite3.connect(path)
    assert connection.execute("PRAGMA user_version").fetchone()[0] == 6
    connection.execute("BEGIN IMMEDIATE")
    process = subprocess.Popen([executable, path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        time.sleep(0.25)
        assert process.poll() is None
        connection.rollback()
        process.communicate(timeout=4)
        assert process.returncode == 0
        connection.execute("PRAGMA user_version=999")
        connection.commit()
        assert subprocess.run([executable, path], capture_output=True, timeout=4).returncode == 1
    finally:
        connection.close()
        if process.poll() is None:
            process.kill()
            process.communicate()
    assert subprocess.run([executable], capture_output=True).returncode == 2
    # Daemon must never announce its control listener after migration failure.
    env = {key: value for key, value in os.environ.items() if not key.startswith("KIARABOT_")}
    env.update(KIARABOT_RUNTIME_DB=str(path), KIARABOT_ADAPTER_ID="test", KIARABOT_ADAPTER_TOKEN=secrets.token_hex(32), KIARABOT_ADAPTER_ACCOUNTS="one")
    result = subprocess.run([BUILD / "core/kiarabot-core", "--grpc-listen", "127.0.0.1:0"], env=env, capture_output=True, timeout=5)
    assert result.returncode == 1
    assert b"operation=control_start" not in result.stdout


def test_logs_contain_outcomes_without_passwords_or_tokens(server):
    tokens = server.login()
    viewer = server.login("viewer")["access_token"]
    assert server.request("/audit", token=viewer)[0] == 403
    logs = server.request("/logs", token=tokens["access_token"])[1]
    validate(logs, "LogPage")
    messages = [item["message"] for item in logs["items"]]
    assert any("login: OK" == message for message in messages)
    assert any("PERMISSION_DENIED" in message for message in messages)
    assert server.password not in json.dumps(logs)
    assert tokens["access_token"] not in json.dumps(logs)
    assert tokens["refresh_token"] not in json.dumps(logs)


def test_wrong_key_and_unsafe_configuration_fail_closed(server):
    import pytest
    server.login()
    server.stop()
    original = server.settings["encryption_key"]
    server.settings["encryption_key"] = secrets.token_hex(32)
    server.config.write_text(json.dumps(server.settings))
    with pytest.raises(AssertionError, match="startup failed"):
        server.start()
    server.settings["encryption_key"] = original
    server.config.write_text(json.dumps(server.settings))
    server.config.chmod(0o644)
    with pytest.raises(AssertionError, match="startup failed"):
        server.start()
    server.config.chmod(0o600)
    server.start()
    assert probe(server, "/health/ready")[0] == 200
