#!/usr/bin/env python3
"""P11 STR09 real managed-plugin streaming acceptance (own PG16.4/TLS/processes).

Owned-fixture wrapper ``infrastructure/postgres/tests/run_database_test.py`` supplies:
root, PG port, initializer path, proxy port, identity-bootstrap binary, pg-bin directory,
identity/model/runtime daemons, the Runtime bridge seed binary, the Model seed binary and the
real ``kiarabot-plugin-host`` binary (the same argument order as P05b-4/P11h).

The driver starts a local **real HTTP SSE provider fixture**, a real Identity, a real Model and a
real Runtime over mTLS/PostgreSQL, then a managed plugin-host process that launches a real SDK
plugin over the TLS PluginBridge. Streaming model calls go plugin SDK -> Runtime -> Model ->
provider fixture. The provider fixture has a deterministic *gate*: it writes the first SSE delta
and then blocks before the remaining chunks/``[DONE]``. The test therefore proves causally that
the SDK observed the first delta **while the provider request was still incomplete**, instead of
comparing the final concatenated text.

The full failure/injection matrix (provider 4xx/5xx, deadline, explicit cancel, bridge
disconnect/reconnect, model restart, revocation/identity unreachable, slow consumer, replay,
cross-instance isolation) is defined below; the process-chain part is only executable once the
STR07 Runtime/PluginBridge streaming surface is available. Until then ``--mode self-check``
validates the provider fixture and the local event expectations without any real process, and the
driver aborts the process-chain mode with a distinct ``STR07 unavailable`` marker.

No real QQ, no paid model provider, no external DB/DSN.
"""
from __future__ import annotations

import hashlib
import http.client
import json
import os
from pathlib import Path
import secrets
import select
import signal
import ssl
import subprocess
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tests/architecture'))
sys.path.insert(0, str(REPO / 'core/tests/platform'))

import identity_bootstrap_test as bootstrap_fixture  # noqa: E402
import runtime_identity_e2e_test as rt  # noqa: E402
import p14_native_single_host as p14  # noqa: E402
import p05b_model_chain_e2e as base  # noqa: E402

MODEL_SAN = 'spiffe://kiarabot/model-test'
PROVIDER_SECRET = 'p11-stream-fixture-secret'
STATE: dict = {}
PROVIDER: 'StreamProvider | None' = None

# Matrix placeholders printed when a gate could not run yet. They are explicit, never silently
# skipped: the final acceptance report must list them with the concrete reason.
PENDING_GATES: list[str] = []
#: Real child processes owned by this driver (originals and restart replacements).
CHILDREN: list = []


class Failure(RuntimeError):
    pass


def require(condition, label):
    if not condition:
        raise Failure(label)


# =========================================================================
# Real local HTTP SSE provider fixture with deterministic gates.
# =========================================================================
#
# Modes (file-controlled so the driver can switch between commands):
#   success        first delta + gate; after release remaining text, usage-only chunk, [DONE]
#   empty          [DONE] with no content delta (legal empty completion)
#   usage_missing  text + finish STOP + [DONE] but no usage chunk
#   length         text + finish_reason "length" + [DONE] (explicit truncation)
#   half_close     first delta then hard socket close without [DONE]
#   slow_consumer  many small deltas as fast as possible (no gate) to overflow a slow SDK queue
#   status_400     HTTP 400 JSON error (known provider rejection, no billing)
#   status_401     HTTP 401 JSON error
#   status_404     HTTP 404 JSON error
#   status_429     HTTP 429 JSON error
#   status_500     HTTP 500 JSON error
#   invalid_json   200 with a non-SSE/non-JSON body
#
# Every request appends one JSON line to the state file:
#   {"kind":"request","mode":...,"stream":bool,"auth_ok":bool,"started":true}
# and on completion one more:
#   {"kind":"completed","mode":...}
# The driver reads the file to prove Provider POST counts and that the first SDK delta arrived
# before completion.

class _QuietServer(ThreadingHTTPServer):
    """Keep fixture connection resets out of the driver's stderr."""

    def handle_error(self, request, client_address):  # noqa: N802 - socketserver API
        return


class StreamProvider:
    def __init__(self, mode_file: Path, state_file: Path, secret: str = PROVIDER_SECRET):
        self.mode_file = mode_file
        self.state_file = state_file
        self.secret = secret
        self.lock = threading.Lock()
        self.release = threading.Event()
        self.first_delta = threading.Event()
        self.completed = threading.Event()
        self.request_count = 0
        self._server = _QuietServer(('127.0.0.1', 0), self._handler_class())
        self._server.daemon_threads = True
        self.port = self._server.server_address[1]
        self._thread: threading.Thread | None = None

    def _mode(self) -> str:
        try:
            return self.mode_file.read_text().strip() or 'success'
        except OSError:
            return 'success'

    def _record(self, value: dict) -> None:
        with self.lock, self.state_file.open('a') as handle:
            handle.write(json.dumps(value) + '\n')

    def _handler_class(self):
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *_args):  # keep the driver output clean
                return

            def do_POST(self):  # noqa: N802 - http.server API
                try:
                    fixture._handle(self)
                except (BrokenPipeError, ConnectionResetError, OSError):
                    pass

        return Handler

    def _handle(self, handler: BaseHTTPRequestHandler) -> None:
        mode = self._mode()
        length = int(handler.headers.get('Content-Length') or 0)
        body = handler.rfile.read(length) if length else b'{}'
        try:
            request = json.loads(body)
        except ValueError:
            request = {}
        auth = handler.headers.get('Authorization', '')
        with self.lock:
            self.request_count += 1
        self._record({'kind': 'request', 'mode': mode,
                      'stream': bool(request.get('stream')),
                      'auth_ok': auth == 'Bearer ' + self.secret,
                      'model': request.get('model', ''),
                      'has_usage_option': bool(request.get('stream_options'))})
        if auth != 'Bearer ' + self.secret:
            self._json(handler, 401, {'error': {'message': 'invalid api key'}})
            return
        if mode.startswith('status_'):
            self._json(handler, int(mode.split('_', 1)[1]),
                       {'error': {'message': 'fixture status', 'secret': 'must-not-leak'}})
            return
        if mode == 'invalid_json':
            self._json(handler, 200, {'not': 'chat-completions'})
            return
        if not request.get('stream'):
            # Unary regression path: a plugin model_chat request must stay stream=false.
            self._json(handler, 200, {
                'choices': [{'message': {'content': 'p11-unary-fixture reply'}}],
                'usage': {'prompt_tokens': 11, 'completion_tokens': 7}})
            return
        handler.send_response(200)
        handler.send_header('Content-Type', 'text/event-stream')
        handler.send_header('Cache-Control', 'no-store')
        handler.send_header('Connection', 'close')
        handler.close_connection = True
        handler.end_headers()

        def write_chunk(payload) -> bool:
            try:
                handler.wfile.write(b'data: ' + json.dumps(payload).encode() + b'\n\n')
                handler.wfile.flush()
                return True
            except (BrokenPipeError, ConnectionResetError, OSError):
                return False

        role = {'choices': [{'delta': {'role': 'assistant'}, 'index': 0}]}
        write_chunk(role)
        if mode == 'empty':
            write_chunk({'choices': [{'delta': {}, 'finish_reason': 'stop', 'index': 0}],
                         'usage': {'prompt_tokens': 5, 'completion_tokens': 0}})
            write_chunk_raw(handler, b'data: [DONE]\n\n')
            self._complete()
            return
        if mode in ('success', 'length', 'usage_missing', 'slow_consumer'):
            first = {'choices': [{'delta': {'content': 'P11 stream first'}, 'index': 0}]}
            write_chunk(first)
            self.first_delta.set()
            if mode == 'slow_consumer':
                for index in range(80):
                    if not write_chunk({'choices': [{'delta': {'content': ' x%d' % index},
                                                    'index': 0}]}):
                        return
                write_chunk({'choices': [{'delta': {}, 'finish_reason': 'stop', 'index': 0}],
                             'usage': {'prompt_tokens': 5, 'completion_tokens': 80}})
                write_chunk_raw(handler, b'data: [DONE]\n\n')
                self._complete()
                return
            # Deterministic causal gate: the remaining chunks only exist after the driver has
            # observed the first delta at the SDK and explicitly released the gate.
            if not self.release.wait(timeout=90.0):
                self._record({'kind': 'gate_timeout', 'mode': mode})
                return
            if mode == 'half_close':
                return  # drop the SSE stream right after one delta; no [DONE], no terminal
            if mode == 'slow_consumer':
                return
            write_chunk({'choices': [{'delta': {'content': ' + tail'}, 'index': 0}]})
            finish = 'length' if mode == 'length' else 'stop'
            final = {'choices': [{'delta': {}, 'finish_reason': finish, 'index': 0}],
                     'usage': {'prompt_tokens': 5, 'completion_tokens': 9}}
            if mode == 'usage_missing':
                final.pop('usage', None)
            write_chunk(final)
            write_chunk_raw(handler, b'data: [DONE]\n\n')
            self._complete()
            return
        self._json(handler, 503, {'error': {'message': 'unknown fixture mode'}})

    def _complete(self) -> None:
        self.completed.set()
        self._record({'kind': 'completed', 'mode': self._mode()})

    @staticmethod
    def _json(handler: BaseHTTPRequestHandler, status: int, value) -> None:
        body = json.dumps(value).encode()
        handler.send_response(status)
        handler.send_header('Content-Type', 'application/json')
        handler.send_header('Content-Length', str(len(body)))
        handler.send_header('Connection', 'close')
        handler.close_connection = True
        handler.end_headers()
        handler.wfile.write(body)

    def start(self) -> 'StreamProvider':
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        return self

    def close(self) -> None:
        self.release.set()
        self._server.shutdown()
        self._server.server_close()

    def requests(self) -> list[dict]:
        try:
            lines = self.state_file.read_text().splitlines()
        except OSError:
            return []
        return [json.loads(line) for line in lines if line.strip()]

    def posts(self) -> int:
        return sum(1 for entry in self.requests() if entry.get('kind') == 'request')

    def completed_requests(self) -> int:
        return sum(1 for entry in self.requests() if entry.get('kind') == 'completed')


def write_chunk_raw(handler: BaseHTTPRequestHandler, payload: bytes) -> None:
    try:
        handler.wfile.write(payload)
        handler.wfile.flush()
    except (BrokenPipeError, ConnectionResetError, OSError):
        pass


# =========================================================================
# Owned-fixture preparation (mirrors the merged P05b-4 chain fixture).
# =========================================================================

def prepare(root: Path, enrolled: dict):
    base.prepare(root, enrolled)
    STATE.update({key: base.STATE[key] for key in
                  ('tls', 'account', 'installation', 'instance', 'model_id',
                   'model_principal', 'admin_model_assignment')})


def after_bootstrap(root: Path) -> None:
    global PROVIDER
    pg_bin = Path(sys.argv[6]).resolve()
    port = int(sys.argv[2])
    initializer = str(Path(sys.argv[3]).resolve())
    identity_binary = Path(sys.argv[7]).resolve()
    runtime_binary = Path(sys.argv[8]).resolve()
    model_binary = Path(sys.argv[9]).resolve()
    seed_binary = Path(sys.argv[10]).resolve()
    model_seed_binary = Path(sys.argv[11]).resolve()
    host_binary = Path(sys.argv[12]).resolve()
    tls = STATE['tls']
    ca = str(tls / 'ca.pem')
    model_id = STATE['model_id']
    databases = {service: p14.create_database(root, pg_bin, port, initializer, service)
                 for service in ('runtime', 'model')}
    rt.identity_security_material(root)
    identity_config = json.loads((root / 'identity.json').read_text())
    identity_config['probe_listen'] = p14.free_endpoint()
    identity_config['grpc_listen'] = p14.free_endpoint()
    base.write_json(root / 'identity.json', identity_config)
    identity_endpoint = identity_config['grpc_listen']
    endpoints = {name: p14.free_endpoint() for name in
                 ('runtime_grpc', 'runtime_probe', 'model_grpc', 'model_probe', 'bridge')}
    root_key = tls / 'model-root.key'
    root_key.write_text(secrets.token_hex(32) + '\n')
    root_key.chmod(0o600)
    model_config = {
        'schema_version': 1, 'service': 'model',
        'probe_listen': endpoints['model_probe'], 'grpc_listen': endpoints['model_grpc'],
        'tls': {'certificate_file': str(tls / 'model.pem'),
                'private_key_file': str(tls / 'model.key'), 'ca_file': ca},
        'database': databases['model'], 'identity_endpoint': identity_endpoint,
        'model_security': {'root_key_file': {'id': 'model-root-v1', 'file': str(root_key)},
                           'key_version': 1}}
    p14.write_leaf(tls, 'host', 'spiffe://kiarabot/host-a')
    host_fingerprint = hashlib.sha256(
        ssl.PEM_cert_to_DER_cert((tls / 'host.pem').read_text())).hexdigest()
    parent_file = root / f'instance-delegation-{STATE["instance"]}-model.json'
    runtime_config = {
        'schema_version': 1, 'service': 'runtime',
        'probe_listen': endpoints['runtime_probe'], 'grpc_listen': endpoints['runtime_grpc'],
        'bridge_listen': endpoints['bridge'],
        'tls': {'certificate_file': str(tls / 'runtime.pem'),
                'private_key_file': str(tls / 'runtime.key'), 'ca_file': ca},
        'database': databases['runtime'], 'identity_endpoint': identity_endpoint,
        'model_endpoint': endpoints['model_grpc'],
        'model_instance_delegation_sealed_file': str(parent_file),
        'hosts': [{'host_id': 'host-a', 'certificate_sha256': host_fingerprint}]}
    configs = {'identity': root / 'identity.json', 'model': root / 'model.json',
               'runtime': root / 'runtime.json'}
    base.write_json(configs['model'], model_config)
    base.write_json(configs['runtime'], runtime_config)

    provider_mode = root / 'provider-mode'
    provider_mode.write_text('success\n')
    provider_state = root / 'provider-requests.jsonl'
    provider_state.write_text('')
    PROVIDER = StreamProvider(provider_mode, provider_state).start()
    provider_port = PROVIDER.port
    children = CHILDREN

    def sql(database: str, statement: str) -> str:
        return rt.run_sql(rt.postgres(root, pg_bin, port, database), statement)

    try:
        adapter_instance, shutdown_instance = str(uuid.uuid4()), str(uuid.uuid4())
        adapter_bootstrap, plugin_bootstrap, shutdown_bootstrap = (
            secrets.token_hex(32) for _ in range(3))
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith('PG')}
        print('STR09 stage: runtime seed', flush=True)
        seeded = base.run([str(seed_binary), str(root), str(port), adapter_instance,
                           STATE['account'], STATE['instance'], adapter_bootstrap,
                           plugin_bootstrap, STATE['installation'], shutdown_instance,
                           shutdown_bootstrap], env=environment)
        require(b'SEEDED' in seeded.stdout, 'runtime bridge seed')
        identity, identity_state = base.daemon(identity_binary, configs['identity'])
        children.append(identity)
        require(identity_state.get('ready') == 'true', 'identity readiness')
        base.wait_probe(identity_config['probe_listen'])
        model_log = (root / 'model.err').open('wb')
        model = subprocess.Popen([str(model_binary), '--config', str(configs['model'])],
                                 stdout=subprocess.PIPE, stderr=model_log)
        children.append(model)
        model_state = {}
        for _ in range(2):
            text = p14.line(model)
            if '=' in text:
                model_state.update(
                    dict(item.split('=', 1) for item in text.split() if '=' in item))
            if 'ready' in model_state:
                break
        require(model_state.get('ready') == 'true', 'model readiness')
        base.wait_probe(endpoints['model_probe'])
        runtime_log = (root / 'runtime.err').open('wb')
        runtime = subprocess.Popen([str(runtime_binary), '--config', str(configs['runtime'])],
                                   stdout=subprocess.PIPE, stderr=runtime_log)
        children.append(runtime)
        runtime_state = {}
        try:
            for _ in range(2):
                text = p14.line(runtime)
                if '=' in text:
                    runtime_state.update(
                        dict(item.split('=', 1) for item in text.split() if '=' in item))
                if 'ready' in runtime_state:
                    break
        except Exception:  # noqa: BLE001 - readiness probe below is authoritative
            pass
        require(runtime.poll() is None, 'runtime process still alive')
        base.wait_probe(endpoints['runtime_probe'])

        identity_service = base.identity_stub(identity_endpoint, tls)
        print('STR09 stage: admin login', flush=True)
        admin = base.admin_login(identity_service, root)
        endpoint = f'http://127.0.0.1:{provider_port}/v1/chat/completions'
        print('STR09 stage: model seed', flush=True)
        seeded_material = base.run([str(model_seed_binary), model_id, PROVIDER_SECRET,
                                    str(root_key), '1', 'x', 'x'])
        material = json.loads(seeded_material.stdout.decode())
        now_ns = time.time_ns()
        sql('p03_model',
            "INSERT INTO kiarabot.model_configs(model_id,alias,provider_kind,endpoint,"
            "provider_model,timeout_ms,max_concurrency,lifecycle_state,config_version,"
            "created_at,observed_at) VALUES('" + model_id + "','p11-stream-model',"
            "'openai_chat_completions','" + endpoint + "','p11-stream-fixture-model',30000,2,"
            "0,1," + str(now_ns) + "," + str(now_ns) + "); "
            "INSERT INTO kiarabot.encrypted_provider_secrets(model_id,secret_version,"
            "root_key_version,aead,nonce,sealed_secret,created_at) VALUES('" + model_id +
            "',1,1,1,decode('" + material['nonce'] + "','hex'),decode('" +
            material['sealed'] + "','hex')," + str(now_ns) + ");")
        print('STR09 stage: register instance principal', flush=True)
        instance_principal = base.register_instance_principal(
            identity_service, STATE['instance'], root, pg_bin, port)
        STATE['instance_principal'] = instance_principal

        # Production sealed parent writer (P05b-7) keeps the 56s delegation fresh per command.
        print('STR09 stage: sealed model envelope', flush=True)
        token_file = root / 'admin.token'
        token_file.write_text(admin + '\n')
        token_file.chmod(0o600)
        refresh_config = root / 'delegation-refresh.json'
        base.write_json(refresh_config, {
            'identity_endpoint': identity_endpoint,
            'ca_file': str(tls / 'ca.pem'),
            'control_certificate_file': str(tls / 'control.pem'),
            'control_private_key_file': str(tls / 'control.key'),
            'instance_id': STATE['instance'],
            'installation_id': STATE['installation'],
            'output_dir': str(root),
            'token_file': str(token_file)})

        def refresh_parent():
            refresh = subprocess.run(
                [sys.executable, 'instance-delegation-refresh', '--config',
                 str(refresh_config), '--token-file', str(token_file), '--scope-kind',
                 'model', '--model-id', model_id, '--once'],
                cwd=REPO / 'scripts/deploy', capture_output=True, timeout=60)
            require(refresh.returncode == 0, 'sealed model refresh: ' +
                    refresh.stderr.decode(errors='replace')[-500:])
            require(parent_file.is_file(), 'sealed model envelope written')
            require(parent_file.stat().st_mode & 0o777 == 0o600, 'sealed envelope 0600')

        refresh_parent()
        base.STATE.update(runtime_grpc=endpoints['runtime_grpc'])
        STATE.update(pg_bin=pg_bin, pg_port=port, host_binary=host_binary,
                     runtime_grpc=endpoints['runtime_grpc'], bridge=endpoints['bridge'],
                     runtime_probe=endpoints['runtime_probe'],
                     model_probe=endpoints['model_probe'],
                     model_binary=model_binary, runtime_binary=runtime_binary,
                     refresh_parent=refresh_parent, model_id=model_id,
                     model_child=model, runtime_child=runtime,
                     identity_child=identity, identity_binary=identity_binary,
                     identity_config=configs['identity'],
                     identity_probe=identity_config['probe_listen'],
                     provider=PROVIDER, provider_state=provider_state,
                     provider_mode=provider_mode,
                     plugin_bootstrap=plugin_bootstrap)
        run_streaming_matrix(root, provider_state, provider_mode, endpoint)
    finally:
        for child in children:
            if child.stderr is not None:
                text = ''
                try:
                    while select.select([child.stderr], [], [], 0)[0]:
                        chunk = os.read(child.stderr.fileno(), 8192)
                        if not chunk:
                            break
                        text += chunk.decode(errors='replace')
                except Exception:  # noqa: BLE001 - bounded diagnostics only
                    pass
                if text.strip():
                    print('STR09 stage: service stderr: ' +
                          ' '.join(text.split())[-2000:], file=sys.stderr)
        for name in ('model', 'runtime'):
            path = root / (name + '.err')
            if path.is_file():
                print(f'STR09 stage: {name} stderr: ' + ' '.join(
                    path.read_text(errors='replace').split())[-3000:], file=sys.stderr)
        for child in reversed(children):
            try:
                base.stop(child)
            except Exception:  # noqa: BLE001 - shutdown is best effort
                pass
        if PROVIDER is not None:
            PROVIDER.close()


# =========================================================================
# Managed real SDK streaming plugin.
# =========================================================================

def plugin_manifest() -> dict:
    return {'id': 'p11.stream.plugin', 'name': 'P11 streaming plugin', 'version': '0.1.0',
            'api_version': 'kiarabot.plugin.v1', 'runtime': 'python-rpc',
            'entrypoint': 'subject:create', 'capabilities': [],
            'isolation': 'process', 'permissions': ['model.chat']}


SUBJECT_TEMPLATE = '''\
import asyncio, json, time
from pathlib import Path
from kiarabot_sdk import Plugin, PluginError

COMMANDS = Path({commands!r})
RESULTS = Path({results!r})
READY = Path({ready!r})
MODEL = {model!r}


class Subject(Plugin):
    async def on_start(self, context):
        self.context = context
        # A managed restart must not replay historic commands from the shared journal.
        try:
            self._seen = len(COMMANDS.read_text().splitlines())
        except FileNotFoundError:
            self._seen = 0
        self._tasks = set()
        READY.write_text("ready")
        self._loop_task = asyncio.create_task(self._loop())

    async def on_stop(self, reason):
        for task in list(self._tasks):
            task.cancel()
        self._loop_task.cancel()

    def _emit(self, value):
        with open(RESULTS, "a") as handle:
            handle.write(json.dumps(value) + "\\n")

    async def _loop(self):
        while True:
            try:
                lines = COMMANDS.read_text().splitlines()
            except FileNotFoundError:
                lines = []
            for line in lines[self._seen:]:
                if not line.strip():
                    continue
                command = json.loads(line)
                task = asyncio.create_task(self._handle(command))
                self._tasks.add(task)
                task.add_done_callback(self._tasks.discard)
            self._seen = len(lines)
            await asyncio.sleep(0.02)

    async def _handle(self, command):
        cid = command["cid"]
        logical = command.get("logical_call_id", cid)
        kind = command.get("kind", "stream")
        self._emit({{"cid": cid, "id": logical, "event": "begin", "kind": kind}})
        if kind == "chat":
            await self._chat(command, cid, logical)
            return
        stream = self.context.model_stream(
            MODEL, [{{"role": "user", "text": command.get("text", "hello")}}],
            logical_call_id=logical, timeout=float(command.get("timeout", 10.0)))
        chunks = []
        try:
            async with stream:
                started = stream.started
                self._emit({{"cid": cid, "id": logical, "event": "started",
                        "elapsed": round(time.monotonic(), 3),
                            "disposition": int(started.disposition) if started else None}})
                if kind == "no_read":
                    await asyncio.sleep(float(command.get("sleep", 3.0)))
                async for delta in stream:
                    chunks.append(delta.text)
                    self._emit({{"cid": cid, "id": logical, "event": "delta",
                                "index": len(chunks), "text": delta.text,
                                "received_delta_count": stream.received_delta_count}})
                    if kind == "cancel" and len(chunks) == 1:
                        receipt = await stream.cancel()
                        self._emit({{"cid": cid, "id": logical,
                                    "event": "cancel_receipt",
                                    "disposition": int(receipt.disposition)}})
            terminal = stream.terminal
            usage = stream.usage
            failure_code = None
            terminal_value = stream.terminal
            if terminal_value is not None and terminal_value.HasField("failure"):
                failure_code = int(terminal_value.failure.error.code)
            self._emit({{"cid": cid, "id": logical, "event": "terminal",
                        "failure_code": failure_code,
                        "elapsed": round(time.monotonic(), 3),
                        "termination": int(stream.termination) if stream.termination else None,
                        "partial": bool(stream.partial),
                        "usage_known": bool(usage.known) if usage is not None else None,
                        "input_tokens": usage.input_tokens if usage is not None else None,
                        "output_tokens": usage.output_tokens if usage is not None else None,
                        "may_have_billed": (bool(stream.may_have_billed)
                                            if stream.may_have_billed is not None else None),
                        "received_bytes": stream.received_bytes,
                        "received_delta_count": stream.received_delta_count,
                        "text": "".join(chunks)}})
        except PluginError as error:
            self._emit({{"cid": cid, "id": logical, "event": "error",
                        "elapsed": round(time.monotonic(), 3),
                        "code": int(error.code), "retryable": bool(error.retryable),
                        "delivery_unknown": bool(getattr(error, "delivery_unknown", False)),
                        "partial": bool(getattr(error, "partial", False)),
                        "partial_text": getattr(error, "partial_text", None),
                        "cause": getattr(error, "cause", None),
                        "has_terminal": getattr(error, "terminal", None) is not None,
                        "text": "".join(chunks)}})
        except Exception as error:
            self._emit({{"cid": cid, "id": logical, "event": "crash",
                        "detail": repr(error)[:300]}})

    async def _chat(self, command, cid, logical):
        try:
            reply = await self.context.model_chat(
                MODEL, [{{"role": "user", "text": command.get("text", "hello")}}],
                logical_call_id=logical, timeout=12.0)
            self._emit({{"cid": cid, "id": logical, "event": "chat_result", "ok": True,
                        "text": reply.text, "input_tokens": reply.input_tokens,
                        "output_tokens": reply.output_tokens}})
        except PluginError as error:
            self._emit({{"cid": cid, "id": logical, "event": "chat_result", "ok": False,
                        "code": int(error.code), "retryable": bool(error.retryable)}})
        except Exception as error:
            self._emit({{"cid": cid, "id": logical, "event": "crash",
                        "detail": repr(error)[:300]}})


def create():
    return Subject()
'''


def write_plugin_package(root: Path) -> tuple[Path, Path]:
    package = root / 'p11-stream-plugin'
    package.mkdir(exist_ok=True)
    commands = root / 'plugin-commands.jsonl'
    results = root / 'plugin-results.jsonl'
    ready = root / 'plugin-ready'
    commands.write_text('')
    results.write_text('')
    (package / 'subject.py').write_text(SUBJECT_TEMPLATE.format(
        commands=str(commands), results=str(results), ready=str(ready),
        model=STATE['model_id']))
    manifest_path = package / 'plugin.json'
    manifest_path.write_text(json.dumps(plugin_manifest()))
    return package, manifest_path


def start_managed_plugin(root: Path, package: Path, manifest_path: Path, tls,
                         plugin_bootstrap) -> subprocess.Popen:
    base.managed_host_config(root, package, manifest_path, tls, STATE['bridge'],
                             STATE['instance'], STATE['installation'], plugin_bootstrap)
    base.seed_managed_start(root, manifest_path, STATE['instance'], STATE['installation'],
                            Path(STATE['pg_bin']), STATE['pg_port'])
    hosted = base.start_managed_host(root, Path(STATE['host_binary']))
    base.wait_for_plugin(hosted, root / 'plugin-ready', root)
    require((root / 'plugin-ready').is_file(), 'managed SDK plugin registered')
    return hosted


# =========================================================================
# Driver-side result journal helpers.
# =========================================================================
#
# The plugin appends JSON lines: begin / started / delta / cancel_receipt / terminal / error /
# chat_result. The driver polls this file; events are keyed by command cid.

def read_results(root: Path) -> list[dict]:
    path = root / 'plugin-results.jsonl'
    try:
        lines = path.read_text().splitlines()
    except OSError:
        return []
    values = []
    for line in lines:
        if line.strip():
            try:
                values.append(json.loads(line))
            except ValueError:
                continue
    return values


def events(root: Path, cid: str, event: str | None = None) -> list[dict]:
    return [value for value in read_results(root)
            if value.get('cid') == cid and (event is None or value.get('event') == event)]


def wait_event(root: Path, cid: str, event: str, *, timeout=60.0,
               predicate=None) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for value in events(root, cid, event):
            if predicate is None or predicate(value):
                return value
        time.sleep(0.05)
    raise Failure(f'plugin event timeout cid={cid} event={event}; '
                  f'journal={json.dumps(events(root, cid))[-800:]}')


def wait_any_event(root: Path, cid: str, names: tuple[str, ...], *, timeout=60.0) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        values = [value for value in events(root, cid) if value.get('event') in names]
        if values:
            return values[0]
        time.sleep(0.05)
    raise Failure(f'plugin event timeout cid={cid} events={names}; '
                  f'journal={json.dumps(events(root, cid))[-800:]}')


def restart_service(root: Path, name: str) -> None:
    """Restart the real owned Identity/Model/Runtime process with the same fixture config."""
    binary = Path(STATE[name + '_binary'])
    config = root / (name + '.json')
    if name == 'identity':
        child, state = base.daemon(binary, config)
        require(state.get('ready') == 'true', 'identity restart readiness')
        base.wait_probe(STATE['identity_probe'])
    else:
        log = (root / (name + '-restart.err')).open('wb')
        child = subprocess.Popen([str(binary), '--config', str(config)],
                                 stdout=subprocess.PIPE, stderr=log)
        state = {}
        for _ in range(2):
            text = p14.line(child)
            if '=' in text:
                state.update(dict(item.split('=', 1) for item in text.split() if '=' in item))
            if 'ready' in state:
                break
        require(state.get('ready') == 'true', f'{name} restart readiness')
        base.wait_probe(STATE[name + '_probe'])
    CHILDREN.append(child)
    STATE[name + '_child'] = child


def retry_unary(root: Path, timeout=45.0) -> dict:
    """After a connection/process disruption, retry a benign unary call until the SDK reconnects."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            value = command(root, str(uuid.uuid4()), wait='chat_result', kind='chat',
                            timeout=20.0)
        except Failure as failure:
            last = str(failure)
            time.sleep(0.5)
            continue
        if value.get('ok') is True:
            return value
        last = json.dumps(value)[:300]
        time.sleep(0.5)
    raise Failure(f'reconnect unary regression did not recover: {last}')


def fixture_sql(root: Path, database: str, statement: str) -> str:
    return rt.run_sql(rt.postgres(root, Path(STATE['pg_bin']), int(STATE['pg_port']),
                                  database), statement)


def commands_path(root: Path) -> Path:
    return root / 'plugin-commands.jsonl'


def send_command(root: Path, cid: str, *, logical_call_id: str | None = None,
                 kind='stream', text='hello', timeout=None, extra=None) -> None:
    command = {'cid': cid, 'kind': kind, 'text': text,
               'logical_call_id': logical_call_id or cid}
    if timeout is not None:
        command['timeout'] = timeout
    if extra:
        command.update(extra)
    with commands_path(root).open('a') as handle:
        handle.write(json.dumps(command) + '\n')


def command(root: Path, cid: str, *, wait='terminal', timeout=60.0, **kwargs) -> dict:
    """Refresh the delegation and send one plugin command; return the awaited event."""
    STATE['refresh_parent']()
    send_command(root, cid, **kwargs)
    return wait_event(root, cid, wait, timeout=timeout)


# =========================================================================
# STR09 matrix (real process chain).
# =========================================================================

def cross_instance_isolation_gate(root: Path, provider, mode_file: Path) -> None:
    """A second registered plugin instance cannot observe or cancel the first stream.

    The second instance is provisioned through fixture SQL rows equivalent to the owned Runtime
    seed (instance + SHA-256 bootstrap credential). It then uses the **real wire** (TLS
    PluginBridge, Bearer bootstrap, registration fence/proof) instead of the SDK so the assertion
    is about Runtime isolation, not about SDK behaviour. It must receive zero model_stream_event
    frames for the first instance's logical id and a NOT_ACTIVE cancel receipt; the first stream
    must still complete normally afterwards.
    """
    import asyncio as asyncio_module

    import grpc
    from google.protobuf.timestamp_pb2 import Timestamp
    from kiarabot.v1 import common_pb2 as common_pb2
    from kiarabot.v1 import model_stream_pb2 as streaming_pb2
    from kiarabot.v1 import platform_pb2 as platform_pb2
    from kiarabot.v1 import plugins_pb2 as plugins_pb2

    print('STR09 stage: cross-instance isolation', flush=True)
    second_instance = str(uuid.uuid4())
    second_bootstrap = secrets.token_hex(32)
    now_ns = time.time_ns()
    fixture_sql(root, 'p03_runtime',
                "INSERT INTO kiarabot.instances(id,installation,plugin_type,desired_version,"
                "applied_config_version,version,state,desired) SELECT '" + second_instance +
                "',id,plugin_type,1,0,1,0,0 FROM kiarabot.installations WHERE id='" +
                STATE['installation'] + "';")
    fixture_sql(root, 'p03_runtime',
                "INSERT INTO kiarabot.bridge_credentials(id,resource_id,digest,created_at,"
                "revoked,version) VALUES('" + str(uuid.uuid4()) + "','" + second_instance +
                "',decode('" + hashlib.sha256(second_bootstrap.encode()).hexdigest() +
                "','hex')," + str(now_ns) + ",false,1);")

    # First instance: one gated stream that stays in flight during the isolation probe.
    cid = str(uuid.uuid4())
    mode_file.write_text('success\n')
    provider.release.clear()
    provider.completed.clear()
    send_command(root, cid, timeout=20.0)
    started = wait_event(root, cid, 'started', timeout=30)
    wait_event(root, cid, 'delta', timeout=30)
    first_request_id = started['request_id']

    async def probe() -> tuple[int, bool]:
        credentials = grpc.ssl_channel_credentials(
            root_certificates=(STATE['tls'] / 'ca.pem').read_bytes())
        channel = grpc.aio.secure_channel(STATE['bridge'], credentials)
        factory = channel.stream_stream(
            '/kiarabot.v1.PluginBridge/Connect',
            request_serializer=plugins_pb2.PluginToCore.SerializeToString,
            response_deserializer=plugins_pb2.CoreToPlugin.FromString)
        call = factory(metadata=(('authorization', 'Bearer ' + second_bootstrap),))
        try:
            registration = plugins_pb2.RegisterPluginRequest(
                manifest=plugins_pb2.PluginManifest(
                    id='p11.isolation.plugin', name='P11 isolation plugin', version='0.1.0',
                    api_version='kiarabot.plugin.v1', runtime='python-rpc',
                    entrypoint='subject:create', isolation='process',
                    capabilities=[], permissions=['model.chat']),
                instance_id=second_instance,
                installation_id=STATE['installation'],
                supported_features=['host.model.stream.v1'])
            await call.write(plugins_pb2.PluginToCore(register=registration))
            registered = await asyncio_module.wait_for(call.read(), 15)
            require(registered.WhichOneof('body') == 'register_result',
                    'second instance registration frame')
            result = registered.register_result
            require(result.accepted and not result.HasField('error'),
                    f'second instance registration: {result}')
            require(result.HasField('fence') and result.HasField('authorization'),
                    'second instance fence/authorization')
            fence, proof = result.fence, result.authorization.proof
            request_id = str(uuid.uuid4())
            cancel = plugins_pb2.HostRequest(
                request_id=request_id, trace_id=str(uuid.uuid4()), idempotency_key='',
                model_stream_cancel=streaming_pb2.ModelStreamCancelRequest(
                    start_request_id=first_request_id, logical_call_id=cid))
            deadline = Timestamp()
            deadline.FromSeconds(int(time.time()) + 5)
            cancel.deadline.CopyFrom(deadline)
            frame = plugins_pb2.PluginToCore(host_request=cancel)
            frame.fence.CopyFrom(fence)
            frame.connection_proof = proof
            await call.write(frame)
            seen_stream_event = False
            disposition = None
            limit = time.time() + 10.0
            while time.time() < limit:
                try:
                    response = await asyncio_module.wait_for(call.read(), 3)
                except (asyncio_module.TimeoutError, grpc.aio.AioRpcError):
                    break
                kind = response.WhichOneof('body')
                if kind == 'model_stream_event':
                    if response.model_stream_event.logical_call_id == cid:
                        seen_stream_event = True
                elif kind == 'host_response' and \
                        response.host_response.request_id == request_id:
                    host = response.host_response
                    if host.HasField('error'):
                        disposition = -int(host.error.code)
                    else:
                        disposition = int(host.model_stream_cancel.disposition)
                    break
            return disposition, seen_stream_event
        finally:
            call.cancel()
            await channel.close()

    try:
        disposition, seen_stream_event = asyncio_module.run(probe())
    finally:
        provider.release.set()
    require(not seen_stream_event,
            'other plugin instance observed the first instance stream event')
    require(disposition == 3,  # MODEL_STREAM_NOT_ACTIVE
            f'other instance cancel must be NOT_ACTIVE, got {disposition}')
    terminal = wait_event(root, cid, 'terminal', timeout=60)
    require(terminal['termination'] == MODEL_STREAM_COMPLETED,
            f'first stream unaffected by other instance: {terminal}')


def probe_streaming_bridge(root: Path) -> None:
    """Fail with an explicit STR07-unavailable marker instead of an opaque timeout.

    Until the Runtime/PluginBridge streaming surface exists, the managed SDK plugin either cannot
    negotiate ``host.model.stream.v1`` (local CAPABILITY_NOT_SUPPORTED) or the Runtime answers the
    start with a pre-start error. Both are distinguished here so the acceptance log never turns a
    missing product slice into a fixture failure.
    """
    deadline = time.monotonic() + 40.0
    attempt = 0
    while time.monotonic() < deadline:
        attempt += 1
        STATE['refresh_parent']()
        cid = str(uuid.uuid4())
        send_command(root, cid, timeout=5.0)
        attempt_deadline = min(deadline, time.monotonic() + 8.0)
        while time.monotonic() < attempt_deadline:
            for value in events(root, cid):
                if value.get('event') == 'started':
                    # Drain the probe deterministically: release the provider gate and wait for
                    # the probe attempt to end so its Provider POST is counted exactly once and
                    # cannot race the following gates.
                    provider = STATE.get('provider')
                    if provider is not None:
                        provider.release.set()
                    cleanup_deadline = time.monotonic() + 20.0
                    while time.monotonic() < cleanup_deadline:
                        if events(root, cid, 'terminal') or events(root, cid, 'error'):
                            return
                        time.sleep(0.05)
                    return
                if value.get('event') in ('error', 'crash'):
                    code = value.get('code')
                    if code in (1, 3, 6):
                        # INVALID_ARGUMENT / PERMISSION_DENIED / CAPABILITY_NOT_SUPPORTED on a
                        # pre-start probe mean the streaming product surface is missing.
                        raise Failure(
                            'STR07 streaming bridge unavailable: pre-start/plugin error '
                            + json.dumps(value)[:500] + f' (code={code})')
                    if value.get('event') == 'crash':
                        raise Failure('STR07 streaming bridge unavailable: plugin crash '
                                      + json.dumps(value)[:500])
                    # UPSTREAM_UNAVAILABLE/TIMEOUT can be a registration race in the managed
                    # plugin start; retry the probe until the overall deadline.
                    break
            else:
                time.sleep(0.05)
                continue
            break
        time.sleep(0.2)
    raise Failure('STR07 streaming bridge unavailable: no stream event after '
                  f'{attempt} probes within 40s')


def run_streaming_matrix(root: Path, provider_state: Path, mode_file: Path,
                         endpoint: str) -> None:
    global PROVIDER
    provider = PROVIDER
    model_id = STATE['model_id']
    tls = STATE['tls']
    commands = commands_path(root)
    package, manifest_path = write_plugin_package(root)
    plugin = start_managed_plugin(root, package, manifest_path, tls,
                                  STATE['plugin_bootstrap'])
    try:
        require(plugin.poll() is None, 'managed plugin-host alive')
        probe_streaming_bridge(root)
        provider_posts = provider.posts()
        completed_posts = provider.completed_requests()
        require(provider_posts >= 1, 'streaming probe reached the provider')

        # -- gate: first delta must arrive before provider completion --
        print('STR09 stage: success gate (first delta before provider completion)', flush=True)
        cid = str(uuid.uuid4())
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        send_command(root, cid, timeout=20.0)
        first = wait_event(root, cid, 'delta', timeout=30)
        require(first['index'] == 1, 'first delta is the first content delta')
        require(not provider.completed.is_set(),
                'first delta arrived only after provider completion')
        require(provider.completed_requests() == completed_posts,
                'provider request must still be incomplete when the first delta arrives')
        provider.release.set()
        terminal = wait_event(root, cid, 'terminal', timeout=60)
        require(terminal['termination'] == MODEL_STREAM_COMPLETED,
                f'completed terminal: {terminal}')
        require(terminal['text'] == 'P11 stream first + tail', 'complete text: ' + terminal['text'])
        require(terminal['partial'] is False, 'completed stream partial=false')
        require(terminal['usage_known'] is True and terminal['input_tokens'] == 5
                and terminal['output_tokens'] == 9, f'authoritative usage: {terminal}')
        require(provider.posts() == provider_posts + 1, 'exactly one provider POST')
        require(provider.completed_requests() == completed_posts + 1,
                'provider completed exactly once')
        stored = fixture_sql(root, 'p03_model',
                            "SELECT state,input_tokens,output_tokens,usage_known "
                            "FROM kiarabot.model_intents WHERE logical_call_id='" + cid + "'")
        require(stored.startswith('1|5|9|t'), f'persisted model call facts: {stored}')
        provider_posts = provider.posts()
        completed_posts = provider.completed_requests()

        # -- empty completion / usage_missing / length truncation --
        for label, mode, expected_termination, checks in [
            ('empty', 'empty', MODEL_STREAM_COMPLETED, ('empty text',)),
            ('usage-missing', 'usage_missing', MODEL_STREAM_COMPLETED, ('known',)),
            ('length-truncated', 'length', MODEL_STREAM_TRUNCATED, ('partial',)),
        ]:
            print(f'STR09 stage: {label}', flush=True)
            cid = str(uuid.uuid4())
            mode_file.write_text(mode + '\n')
            provider.release.set()  # length/usage modes pass the gate immediately
            send_command(root, cid, timeout=20.0)
            value = wait_event(root, cid, 'terminal', timeout=60)
            require(value['termination'] == expected_termination,
                    f'{label} termination: {value}')
            if label == 'empty':
                require(value['text'] == '' and value['partial'] is False,
                        f'empty stream shape: {value}')
            if label == 'usage-missing':
                require(value['usage_known'] is False and value['input_tokens'] == 0
                        and value['output_tokens'] == 0,
                        f'unknown usage is zero counters, not free: {value}')
                require(value['may_have_billed'] is True,
                        f'unknown usage keeps conservative billing: {value}')
            if label == 'length-truncated':
                require(value['partial'] is True and value['text'] != '',
                        f'truncated keeps partial text: {value}')
            require(provider.posts() == provider_posts + 1, f'{label} one provider POST')
            provider_posts = provider.posts()
            completed_posts = provider.completed_requests()

        # -- half-close / terminal loss must be unknown delivery, never COMPLETED --
        print('STR09 stage: half-close terminal loss', flush=True)
        cid = str(uuid.uuid4())
        mode_file.write_text('half_close\n')
        provider.release.clear()
        provider.completed.clear()
        send_command(root, cid, timeout=20.0)
        wait_event(root, cid, 'delta', timeout=30)
        provider.release.set()
        value = wait_event(root, cid, 'error', timeout=60)
        require(value['delivery_unknown'] is True, f'half-close unknown delivery: {value}')
        require(value['partial'] is True and value['text'] == 'P11 stream first',
                f'half-close keeps local partial: {value}')
        require(provider.posts() == provider_posts + 1, 'half-close one provider POST')
        provider_posts = provider.posts()

        # -- provider HTTP error mapping: stable codes, no terminal-based success --
        for mode, expected_code in [('status_429', 7), ('status_500', 9), ('status_400', 1),
                                    ('status_401', 9), ('status_404', 6)]:
            print(f'STR09 stage: provider {mode}', flush=True)
            cid = str(uuid.uuid4())
            mode_file.write_text(mode + '\n')
            send_command(root, cid, timeout=20.0, wait='error')
            value = wait_event(root, cid, 'error', timeout=60)
            if 'code' in value:
                require(value['code'] == expected_code,
                        f'{mode} stable code expected={expected_code}: {value}')
            else:  # pre-start HostResponse rejection arrives as typed PluginError too
                require(value.get('has_terminal') in (True, False), f'{mode} typed error: {value}')
            require(provider.posts() == provider_posts + 1, f'{mode} one provider POST')
            provider_posts = provider.posts()
        mode_file.write_text('success\n')
        provider.release.set()

        # -- explicit cancel: typed receipt, no fabricated not-executed/free claim --
        print('STR09 stage: explicit cancel', flush=True)
        cid = str(uuid.uuid4())
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        send_command(root, cid, kind='cancel', timeout=20.0)
        wait_event(root, cid, 'delta', timeout=30)
        receipt = wait_event(root, cid, 'cancel_receipt', timeout=30)
        require(receipt['disposition'] in (1, 2, 3), f'cancel receipt: {receipt}')
        provider.release.set()
        value = wait_event(root, cid, 'terminal', timeout=60)
        require(value['termination'] in (MODEL_STREAM_CANCELLED, MODEL_STREAM_FAILED),
                f'cancel terminal: {value}')
        require(value['termination'] != MODEL_STREAM_COMPLETED, 'cancel cannot be COMPLETED')
        require(provider.posts() == provider_posts + 1, 'cancel one provider POST')
        provider_posts = provider.posts()

        # -- deadline while the provider is gated (no completion) --
        print('STR09 stage: deadline', flush=True)
        cid = str(uuid.uuid4())
        provider.release.clear()
        provider.completed.clear()
        send_command(root, cid, timeout=1.0)  # HostRequest deadline clips Model execution
        value = wait_event(root, cid, 'terminal', timeout=60)
        require(value['termination'] in (MODEL_STREAM_CANCELLED, MODEL_STREAM_FAILED),
                f'deadline terminal: {value}')
        require(value['termination'] != MODEL_STREAM_COMPLETED, 'deadline cannot be COMPLETED')
        provider.release.set()
        require(provider.posts() == provider_posts + 1, 'deadline one provider POST')
        provider_posts = provider.posts()

        # -- slow consumer / bounded SDK queue: local unknown + best-effort cancel --
        print('STR09 stage: slow consumer queue overflow', flush=True)
        cid = str(uuid.uuid4())
        mode_file.write_text('slow_consumer\n')
        send_command(root, cid, kind='no_read', timeout=20.0, extra={'sleep': 3.0})
        wait_event(root, cid, 'delta', timeout=30)
        value = wait_event(root, cid, 'error', timeout=60)
        require(value['delivery_unknown'] is True, f'slow consumer unknown delivery: {value}')
        require(provider.posts() == provider_posts + 1, 'slow consumer one provider POST')
        provider_posts = provider.posts()
        # Control frames stay alive: a unary model_chat after the overflow still works.
        cid = str(uuid.uuid4())
        mode_file.write_text('success\n')
        value = command(root, cid, wait='chat_result', kind='chat')
        require(value['ok'] is True and value['text'] == 'p11-unary-fixture reply',
                f'unary regression after stream: {value}')
        require(provider.posts() == provider_posts + 1, 'unary regression one provider POST')
        provider_posts = provider.posts()

        # -- concurrent same logical id: one fresh, one replay, exactly one provider POST --
        print('STR09 stage: same logical id concurrency', flush=True)
        logical = str(uuid.uuid4())
        cid_a, cid_b = str(uuid.uuid4()), str(uuid.uuid4())
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        send_command(root, cid_a, logical_call_id=logical, timeout=20.0)
        send_command(root, cid_b, logical_call_id=logical, timeout=20.0)
        started_a = wait_event(root, cid_a, 'started', timeout=30)
        started_b = wait_event(root, cid_b, 'started', timeout=30)
        dispositions = {started_a['disposition'], started_b['disposition']}
        require(1 in dispositions, f'exactly one FRESH admission: {dispositions}')
        require(dispositions - {1} <= {2, 3}, f'other attempt is replay metadata: {dispositions}')
        provider.release.set()
        terminals = {cid_a: wait_event(root, cid_a, 'terminal', timeout=60),
                     cid_b: wait_event(root, cid_b, 'terminal', timeout=60)}
        replay_values = [value for value in terminals.values()
                         if value['termination'] == MODEL_STREAM_REPLAY]
        require(len(replay_values) == 1, f'exactly one replay terminal: {terminals}')
        fresh = [value for value in terminals.values()
                 if value['termination'] == MODEL_STREAM_COMPLETED]
        require(len(fresh) == 1, f'exactly one completed fresh terminal: {terminals}')
        require(replay_values[0]['received_delta_count'] == 0
                and replay_values[0]['text'] == '', 'concurrent replay has zero delta')
        require(provider.posts() == provider_posts + 1,
                'concurrent same logical id calls the provider once')
        provider_posts = provider.posts()

        # -- replay: same logical id after terminal, zero delta, no provider re-invocation --
        print('STR09 stage: replay by original logical id', flush=True)
        logical = str(uuid.uuid4())
        cid = str(uuid.uuid4())
        mode_file.write_text('success\n')
        provider.release.set()
        command(root, cid, logical_call_id=logical, timeout=20.0)
        require(provider.posts() == provider_posts + 1, 'fresh replay-source one provider POST')
        provider_posts = provider.posts()
        replay_cid = str(uuid.uuid4())
        value = command(root, replay_cid, logical_call_id=logical, timeout=20.0)
        require(value['termination'] == MODEL_STREAM_REPLAY, f'replay terminal: {value}')
        require(value['received_delta_count'] == 0 and value['text'] == '',
                f'replay zero delta: {value}')
        require(provider.posts() == provider_posts, 'replay must not call the provider again')

        # -- txn2 commit failure injection (real PG trigger, no product-code change) --
        print('STR09 stage: txn2 commit failure injection', flush=True)
        logical = str(uuid.uuid4())
        cid = str(uuid.uuid4())
        fixture_sql(root, 'p03_model',
            "CREATE OR REPLACE FUNCTION kiarabot.p11_txn2_fail() RETURNS trigger "
            "LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'p11 txn2 injected failure'; END $$; "
            "CREATE TRIGGER p11_txn2_fail BEFORE UPDATE ON kiarabot.model_intents "
            "FOR EACH ROW WHEN (OLD.logical_call_id = '" + logical + "') "
            "EXECUTE FUNCTION kiarabot.p11_txn2_fail();")
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        try:
            send_command(root, cid, logical_call_id=logical, timeout=20.0)
            wait_event(root, cid, 'delta', timeout=30)
            provider.release.set()
            value = wait_any_event(root, cid, ('terminal', 'error'), timeout=90)
        finally:
            fixture_sql(root, 'p03_model',
                'DROP TRIGGER IF EXISTS p11_txn2_fail ON kiarabot.model_intents; '
                'DROP FUNCTION IF EXISTS kiarabot.p11_txn2_fail();')
        require(value.get('event') == 'error'
                or value.get('termination') != MODEL_STREAM_COMPLETED,
                f'txn2 failure must not be delivered as COMPLETED: {value}')
        if value.get('event') == 'terminal':
            require(value.get('partial') is True, 'txn2 failure keeps partial delivery fact')
            require(value.get('may_have_billed') is True,
                    'txn2 failure is not a free/non-executed claim')
        require(provider.posts() == provider_posts + 1, 'txn2 failure one provider POST')
        provider_posts = provider.posts()
        row = fixture_sql(root, 'p03_model', "SELECT state,disposition FROM kiarabot.model_intents "
                               "WHERE logical_call_id='" + logical + "'")
        require(not row.startswith('1'), f'txn2 failure must not persist Succeeded: {row}')

        # -- sealed parent revocation: a new call fails closed with no provider POST --
        print('STR09 stage: sealed parent revocation (new call)', flush=True)
        envelope = root / f'instance-delegation-{STATE["instance"]}-model.json'
        original_envelope = envelope.read_text()
        envelope.unlink()
        try:
            # The Runtime may cache the previous short-lived child delegation; wait the fixture's
            # bounded cache window before asserting fail-closed.
            time.sleep(6.0)
            cid = str(uuid.uuid4())
            mode_file.write_text('success\n')
            provider.release.set()
            send_command(root, cid, timeout=15.0)
            value = wait_any_event(root, cid, ('terminal', 'error'), timeout=60)
            require(value.get('event') == 'error'
                    or value.get('termination') != MODEL_STREAM_COMPLETED,
                    f'revoked parent must fail closed: {value}')
            require(provider.posts() == provider_posts,
                    'revoked parent must not reach the provider')
        finally:
            envelope.write_text(original_envelope)
            envelope.chmod(0o600)
            STATE['refresh_parent']()
        retry_unary(root)

        # -- Identity unreachable in-flight: conservative stop, no fabricated COMPLETED --
        print('STR09 stage: identity unreachable in-flight', flush=True)
        cid = str(uuid.uuid4())
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        send_command(root, cid, timeout=20.0)
        wait_event(root, cid, 'delta', timeout=30)
        started_wall = time.monotonic()
        base.stop(STATE['identity_child'])
        try:
            value = wait_any_event(root, cid, ('terminal', 'error'), timeout=90)
            require(value.get('event') == 'error'
                    or value.get('termination') != MODEL_STREAM_COMPLETED,
                    f'identity loss must not complete the stream: {value}')
            require(time.monotonic() - started_wall < 15.0,
                    'identity loss detection exceeded the bounded authorization-check budget')
        finally:
            provider.release.set()
            restart_service(root, 'identity')
        retry_unary(root, timeout=60)

        # -- in-flight revocation: disable the registered instance principal; Identity's online
        # check must stop the stream conservatively and never fabricate COMPLETED --
        print('STR09 stage: in-flight instance revocation', flush=True)
        cid = str(uuid.uuid4())
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        send_command(root, cid, timeout=20.0)
        wait_event(root, cid, 'delta', timeout=30)
        principal = STATE['instance_principal']
        revoked_wall = time.monotonic()
        fixture_sql(root, 'p03_identity',
                    "UPDATE kiarabot.principals SET state=1 WHERE id='" + principal + "';")
        try:
            value = wait_any_event(root, cid, ('terminal', 'error'), timeout=60)
            require(value.get('event') == 'error'
                    or value.get('termination') != MODEL_STREAM_COMPLETED,
                    f'in-flight revocation must not complete the stream: {value}')
            require(time.monotonic() - revoked_wall < 15.0,
                    'in-flight revocation detection exceeded the bounded budget')
        finally:
            provider.release.set()
            fixture_sql(root, 'p03_identity',
                        "UPDATE kiarabot.principals SET state=0 WHERE id='" + principal + "';")
        retry_unary(root, timeout=60)

        # -- Bridge/runtime disconnect mid-stream: unknown delivery, then reconnect works --
        print('STR09 stage: bridge disconnect mid-stream', flush=True)
        cid = str(uuid.uuid4())
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        send_command(root, cid, timeout=20.0)
        wait_event(root, cid, 'delta', timeout=30)
        base.stop(STATE['runtime_child'])
        try:
            value = wait_any_event(root, cid, ('terminal', 'error'), timeout=90)
            require(value.get('event') == 'error'
                    or value.get('termination') != MODEL_STREAM_COMPLETED,
                    f'bridge disconnect must not complete the stream: {value}')
        finally:
            provider.release.set()
            restart_service(root, 'runtime')
        retry_unary(root, timeout=60)

        # -- Model restart mid-stream: unknown delivery, then restart recovers --
        print('STR09 stage: model restart mid-stream', flush=True)
        cid = str(uuid.uuid4())
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        send_command(root, cid, timeout=20.0)
        wait_event(root, cid, 'delta', timeout=30)
        base.stop(STATE['model_child'])
        try:
            value = wait_any_event(root, cid, ('terminal', 'error'), timeout=90)
            require(value.get('event') == 'error'
                    or value.get('termination') != MODEL_STREAM_COMPLETED,
                    f'model restart must not complete the stream: {value}')
        finally:
            provider.release.set()
            restart_service(root, 'model')
        retry_unary(root, timeout=60)

        # -- cross-instance isolation: second registered instance cannot see/cancel the stream --
        cross_instance_isolation_gate(root, provider, mode_file)
        provider_posts = provider.posts()

        if PENDING_GATES:
            print('STR09 pending gates: ' + ','.join(PENDING_GATES), flush=True)
        else:
            print('STR09 pending gates: none', flush=True)
        print('STR09 managed streaming e2e passed: first delta before provider completion, '
              'provider POST count bounded, partial/unknown/cancel/replay typed', flush=True)
    finally:
        if plugin.poll() is None:
            plugin.send_signal(signal.SIGTERM)
            try:
                plugin.wait(timeout=15)
            except subprocess.TimeoutExpired:
                plugin.kill()
                plugin.wait(timeout=5)


MODEL_STREAM_COMPLETED = 1
MODEL_STREAM_TRUNCATED = 2
MODEL_STREAM_FAILED = 3
MODEL_STREAM_CANCELLED = 4
MODEL_STREAM_REPLAY = 5


# =========================================================================
# Offline self-check: validates the provider fixture gates without any product
# process (runs today; the managed chain mode waits for STR07).
# =========================================================================

def self_check() -> int:
    import tempfile
    with tempfile.TemporaryDirectory(prefix='p11-stream-selfcheck-') as temporary:
        root = Path(temporary)
        mode_file = root / 'mode'
        state_file = root / 'state.jsonl'
        provider = StreamProvider(mode_file, state_file).start()
        try:
            endpoint = f'http://127.0.0.1:{provider.port}/v1/chat/completions'
            mode_file.write_text('success\n')
            body = json.dumps({'model': 'probe', 'stream': True,
                               'messages': [{'role': 'user', 'content': 'x'}]}).encode()
            connection = http.client.HTTPConnection('127.0.0.1', provider.port, timeout=10)
            connection.request('POST', '/v1/chat/completions', body=body,
                               headers={'Authorization': 'Bearer ' + PROVIDER_SECRET,
                                        'Content-Type': 'application/json'})
            response = connection.getresponse()
            require(response.status == 200, 'self-check SSE status')
            require(response.getheader('Content-Type') == 'text/event-stream',
                    'self-check SSE content type')
            prefix = b''
            while b'P11 stream first' not in prefix:
                chunk = response.read1(4096)
                if not chunk:
                    break
                prefix += chunk
            require(b'P11 stream first' in prefix, 'self-check first delta is emitted')
            require(not provider.completed.is_set(),
                    'self-check provider is gated after the first delta')
            provider.release.set()
            remainder = response.read()
            require(b'[DONE]' in remainder, 'self-check [DONE] after release')
            require(provider.completed.is_set(), 'self-check provider completed after release')
            require(provider.posts() == 1 and provider.completed_requests() == 1,
                    'self-check provider POST count')
            # Unary regression probe with the same fixture.
            unary = json.dumps({'model': 'probe', 'stream': False,
                                'messages': [{'role': 'user', 'content': 'x'}]}).encode()
            connection = http.client.HTTPConnection('127.0.0.1', provider.port, timeout=10)
            connection.request('POST', '/v1/chat/completions', body=unary,
                               headers={'Authorization': 'Bearer ' + PROVIDER_SECRET,
                                        'Content-Type': 'application/json'})
            reply = json.loads(connection.getresponse().read())
            require(reply['choices'][0]['message']['content'] == 'p11-unary-fixture reply',
                    'self-check unary fixture')
            require(reply['usage']['prompt_tokens'] == 11, 'self-check unary usage')
            # Failure-mode probes keep the stable provider mapping honest.
            for mode, expected in [('status_429', 429), ('status_500', 500),
                                   ('status_400', 400), ('status_401', 401),
                                   ('status_404', 404), ('invalid_json', 200)]:
                mode_file.write_text(mode + '\n')
                connection = http.client.HTTPConnection('127.0.0.1', provider.port, timeout=10)
                connection.request('POST', '/v1/chat/completions', body=unary,
                                   headers={'Authorization': 'Bearer ' + PROVIDER_SECRET,
                                            'Content-Type': 'application/json'})
                require(connection.getresponse().status == expected,
                        f'self-check {mode} status')
        finally:
            provider.close()
    # Managed plugin subject template must render to valid Python and its manifest must satisfy
    # the frozen plugin manifest shape (no process needed).
    rendered = SUBJECT_TEMPLATE.format(commands='/tmp/c', results='/tmp/r', ready='/tmp/ready',
                                       model=str(uuid.uuid4()))
    compile(rendered, 'p11_stream_plugin_subject.py', 'exec')
    manifest = plugin_manifest()
    require(manifest['permissions'] == ['model.chat'], 'plugin manifest model.chat')
    require(manifest['entrypoint'] == 'subject:create', 'plugin manifest entrypoint')
    require((REPO / 'tests/architecture/p05b_provider_fixture.py').is_file(),
            'provider fixture reference exists')
    print('P11 streaming provider fixture self-check passed')
    return 0


# =========================================================================
# Entry point.
# =========================================================================

def main() -> int:
    arguments = sys.argv[1:]
    if '--self-check' in arguments:
        return self_check()
    if len(arguments) < 6:
        # run_database_test.py invokes every owned-fixture driver once against a foreign backend
        # directory; the owned fixture must reject the incomplete invocation.
        print('fixture backend directory mismatch', file=sys.stderr)
        return 1
    if len(arguments) < 12:
        print('P11 streaming e2e: owned fixture arguments missing', file=sys.stderr)
        return 1
    mode = 'expect-success'
    if '--mode' in arguments:
        index = arguments.index('--mode')
        if index + 1 < len(arguments):
            mode = arguments[index + 1]
    if mode not in ('expect-success', 'self-check'):
        print(f'P11 streaming e2e failure: unknown mode {mode}', file=sys.stderr)
        return 2
    STATE['mode'] = mode
    bootstrap_fixture.main(control_server_certificate=False, prepare=prepare,
                           after_bootstrap=after_bootstrap)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except SystemExit:
        raise
    except Exception as error:  # noqa: BLE001 - one bounded redacted diagnostic line
        import traceback
        frames = traceback.extract_tb(sys.exc_info()[2])
        location = ' <- '.join(f'{frame.filename.rsplit("/", 1)[-1]}:{frame.lineno}'
                               for frame in frames[-4:])
        message = ' '.join(str(error).split())
        if isinstance(error, Failure) and 'STR07' in message:
            print(f'STR09 streaming bridge unavailable: {message[:600]} at {location}',
                  file=sys.stderr)
            raise SystemExit(3)
        print(f'P11 streaming e2e failure: {type(error).__name__}: {message[:800]} at {location}',
              file=sys.stderr)
        raise SystemExit(1)
