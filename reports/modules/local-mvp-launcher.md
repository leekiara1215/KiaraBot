# Local reference MVP launcher

Scope: scripts/local/start.py, its ownership tests, doc/08 minimum local instructions;
no C++/proto/locks/P04/DB schema changes. Dependencies: reviewed fixed Sender adc0d06
and local OneBot4990932 (local a9809dd/0ea09d7), retained as separate commits.

Commands: init/start/status/stop, private state-dir and explicit local binary/Python
paths. Default demo owns loopback WebSocket/Unix IPC platform, actual NoneBot and
reference Core with managed fixed SDK Sender. Optional --ui uses existing Vite live
API/WS. Real OneBot requires explicit URL/token-file mode; no paid/external call was run.
Tokens/password/encryption key persist in 0600 files, directory0700, SQLite/outbox are
never reset by restart. HTTP/gRPC bind loopback/ephemeral by default. Existing auth,
action allowlist and quotas are retained. No plugin management expansion.

Supervisor lifetime uses an inherited flock, owned marker and run ID; signalling
requires exact UID/PID start identity and own command/state marker. Children are
Popen-owned, bounded reaped even after failures. Suspect old PID or live orphan
refuses takeover; symlink state/lock and insecure private files fail. Logs retain only
whitelisted structured metadata, redact known material, never print secrets or bodies.
A running start is idempotent; changing config requires stopping it. Missing child or
health failure ends the supervisor nonzero. status healthy means the owned supervisor
and Control live endpoint; default demo additionally waits for its actual WS client.

Actual checks: real reference init/login/health/idempotent start/persistent restart/
double stop passed. Default managed SDK→actual NoneBot→local OneBot group message
returned 收到：hello; login→real WS SESSION_READY→REST/me passed. Restart preserved
password/control hash and SQLite, all owned children stopped and demo socket removed;
logs contained none of password/tokens/login secrets. Lock environment restored with
uv sync --locked Python3.11.18 and generated files ignored; no lock upgrade.
Ownership tests3/3 passed (symlink directory/insecure secret, forged foreign PID never
signalled, symlink lock victim unchanged). Python compile/diff/document links and
boundary checks are recorded with final delivery. No full repository suite repeated;
parent owns aggregate MVP/UI acceptance. Optional live UI was not locally installed
or tested here. Additional sequential group/private probe at numeric456 returned a
private reply timeout; left as explicit limitation, not hidden by weaker policies.

Document impact: doc08 directly updated. No new wire/API/error schema or module
boundary, so proto/API and unrelated docs need no sync; protected doc18 untouched.
