import json
import secrets
import sqlite3

from conftest import validate


def test_all_read_routes_and_authorization(contract_server):
    server = contract_server
    admin = server.login()["access_token"]
    viewer = server.login("viewer")["access_token"]
    schemas = {"/auth/me": "Principal", "/auth/sessions": "SessionPage", "/health": "Health",
               "/adapters": "AdapterPage", "/plugins": "PluginPage", "/audit": "AuditPage",
               "/bots": "BotPage", "/models": "ModelPage", "/groups": "GroupPage", "/users": "UserPage",
               "/tasks": "TaskPage", "/logs": "LogPage", "/plugins/test.plugin/health": "Health",
               "/plugins/test.plugin/config": "PluginConfig"}
    for route, schema in schemas.items():
        status, body, headers = server.request(route, token=admin)
        assert status == 200, (route, body)
        validate(body, schema)
        assert headers["Cache-Control"] == "no-store"
        status, body, _ = server.request(route)
        assert status == 401, route
        validate(body, "Error")
    for path in ("/logs", "/users", "/audit", "/plugins/test.plugin/config"):
        assert server.request(path, token=viewer)[0] == 403
    assert server.request("/actions/missing", token=admin)[0] == 404
    assert server.request("/actions/by-idempotency-key", token=admin, headers={"Idempotency-Key": "missing"})[0] == 404
    for query in ("?page_size=0", "?page_size=101", "?page_size=1x", "?filter=x", "?page_size=1&page_size=2", "?page_token=forged"):
        assert server.request("/bots" + query, token=admin)[0] == 400


def test_login_rotation_revoke_restart_and_encryption(server):
    key = secrets.token_hex(16)
    tokens = server.login(key=key)
    assert tokens == server.login(key=key)
    session = server.request("/auth/me", token=tokens["access_token"])[1]["session_id"]
    body = {"refresh_token": tokens["refresh_token"]}
    key = secrets.token_hex(16)
    status, rotated, _ = server.request("/auth/refresh", "POST", body, key=key)
    assert status == 200
    assert server.request("/auth/me", token=tokens["access_token"])[0] == 401
    assert server.request("/auth/refresh", "POST", body, key=key)[1] == rotated
    assert server.request("/auth/refresh", "POST", body)[0] == 401
    server.stop()
    server.start()
    assert server.request("/auth/me", token=rotated["access_token"])[1]["session_id"] == session
    assert server.request("/auth/refresh", "POST", body, key=key)[1] == rotated
    database = (server.directory / "runtime.db").read_bytes()
    for secret in (tokens["access_token"], tokens["refresh_token"], rotated["access_token"], server.password):
        assert secret.encode() not in database
    assert server.request(f"/auth/sessions/{session}/revoke", "POST", token=rotated["access_token"])[0] == 204
    assert server.request("/auth/me", token=rotated["access_token"])[0] == 401
    assert server.request("/auth/refresh", "POST", body, key=key)[0] == 401


def test_mutations_version_idempotency_and_audit(server):
    admin = server.login()["access_token"]
    viewer = server.login("viewer")["access_token"]
    config = {"expected_version": "0", "values": {"enabled": True, "limit": 4}}
    manifest = server.settings["plugins"][0]
    for path, method, body in [("/plugins/validate", "POST", manifest),
                               ("/plugins/test.plugin/enable", "POST", None),
                               ("/plugins/test.plugin/disable", "POST", None),
                               ("/plugins/test.plugin/config", "PUT", config)]:
        assert server.request(path, method, body, token=viewer)[0] == 403
    status, validation, _ = server.request("/plugins/validate", "POST", manifest, token=admin)
    assert status == 200 and validation["valid"]
    validate(validation, "ValidationResult")
    key = secrets.token_hex(16)
    status, changed, _ = server.request("/plugins/test.plugin/config", "PUT", config, token=admin, key=key)
    assert status == 200
    assert changed == server.request("/plugins/test.plugin/config", "PUT", config, token=admin, key=key)[1]
    assert server.request("/plugins/test.plugin/config", "PUT", config, token=admin)[0] == 409
    config["values"]["limit"] = 5
    assert server.request("/plugins/test.plugin/config", "PUT", config, token=admin, key=key)[0] == 409
    assert server.request("/plugins/test.plugin/config", "PUT", {"expected_version": changed["version"], "values": {"api_key": "hidden"}}, token=admin)[0] == 400
    for enabled in (False, True):
        operation = "enable" if enabled else "disable"
        assert server.request(f"/plugins/test.plugin/{operation}", "POST", token=admin)[0] == 202
        state = server.request("/plugins", token=admin)[1]["items"][0]["state"]
        assert state == ("PLUGIN_STATE_STOPPED" if enabled else "PLUGIN_STATE_DISABLED")
    audit = server.request("/audit?page_size=100", token=admin)[1]["items"]
    actions = [item["action"] for item in audit]
    for action in ("login", "validatePlugin", "enablePlugin", "disablePlugin", "updatePluginConfig"):
        assert action in actions
    assert actions.count("updatePluginConfig") == 5  # deny, commit, stale version, mismatch, sensitive field
    assert "hidden" not in json.dumps(audit)


def test_immutable_pagination_and_cross_session_visibility(server):
    admin = server.login()["access_token"]
    first = server.request("/auth/sessions?page_size=1", token=admin)[1]
    assert "next_page_token" not in first["page"]
    viewer = server.login("viewer")["access_token"]
    first = server.request("/auth/sessions?page_size=1", token=admin)[1]
    cursor = first["page"]["next_page_token"]
    server.login("operator")
    status, last, _ = server.request("/auth/sessions?page_size=1&page_token=" + cursor, token=admin)
    assert status == 200 and len(last["items"]) == 1 and "next_page_token" not in last["page"]
    assert server.request("/auth/sessions?page_size=1&page_token=" + cursor, token=viewer)[0] == 400
    assert server.request("/bots?page_size=1&page_token=" + cursor, token=admin)[0] == 400
    assert server.request("/auth/sessions?page_size=2&page_token=" + cursor, token=admin)[0] == 400
    own = server.request("/auth/sessions", token=viewer)[1]["items"]
    assert len(own) == 1 and own[0]["user_id"] == "viewer"


def test_dependency_failure_does_not_accept_write(server):
    admin = server.login()["access_token"]
    connection = sqlite3.connect(server.directory / "runtime.db")
    try:
        connection.execute("BEGIN IMMEDIATE")
        status, body, _ = server.request("/plugins/test.plugin/disable", "POST", token=admin)
        assert status == 409 and body["code"] == "CONFLICT"
    finally:
        connection.rollback()
        connection.close()
    assert server.request("/plugins", token=admin)[1]["items"][0]["state"] == "PLUGIN_STATE_STOPPED"


def test_login_rate_limit_and_invalid_request(server):
    for _ in range(30):
        assert server.request("/auth/login", "POST", {"username": "bad", "password": "wrong"})[0] == 401
    status, body, headers = server.request("/auth/login", "POST", {"username": "bad", "password": "wrong"})
    assert status == 429 and body["code"] == "RATE_LIMITED" and int(headers["Retry-After"]) > 0
    assert server.request("/auth/login", "POST", {"username": "admin"})[0] == 400
    assert server.request("/auth/login", "POST", {"username": "admin", "password": server.password}, key="short")[0] == 400


def test_transport_rejects_oversize_and_duplicate_credentials(server):
    import socket
    for headers in (b"Content-Length: 999999\r\n", b"Authorization: Bearer a\r\nAuthorization: Bearer b\r\nContent-Length: 0\r\n"):
        with socket.create_connection(("127.0.0.1", server.port), timeout=3) as connection:
            connection.sendall(b"POST /api/v1/auth/login HTTP/1.1\r\nHost: localhost\r\n" + headers + b"\r\n")
            response = b""
            while True:
                part = connection.recv(4096)
                if not part:
                    break
                response += part
        assert response.startswith(b"HTTP/1.1 400")
        body = json.loads(response.split(b"\r\n\r\n", 1)[1])
        validate(body, "Error")
        assert body["code"] == "INVALID_ARGUMENT"
