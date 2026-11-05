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
import shutil
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


MATRIX_START: float | None = None
LAST_ROOT: Path | None = None
ARTIFACT_DIR = REPO / 'build/logs/final-acceptance/str09-artifacts'


def preserve_fixture_artifacts(root: Path | None, label: str) -> Path | None:
    """Copy the owned fixture's diagnostic files out before run_database_test removes its root.

    The reconnect/restart gates must keep host.err, plugin.log and the plugin/result journals for
    the streaming owner's diagnosis; the temporary owned cluster directory itself is deleted by the
    wrapper after the driver returns.
    """
    if root is None or not root.is_dir():
        return None
    stamp = time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())
    target = ARTIFACT_DIR / f'{label}-{stamp}'
    try:
        target.mkdir(parents=True, exist_ok=True)
    except OSError:
        return None
    for name in ('host.err', 'plugin.log', 'plugin-results.jsonl', 'plugin-commands.jsonl',
                 'runtime.err', 'model.err', 'identity.err', 'host.json', 'host.sqlite',
                 'runtime.json', 'model.json', 'identity.json', 'provider-requests.jsonl',
                 'provider-mode', 'p03-owned-marker'):
        source = root / name
        if source.is_file():
            try:
                shutil.copy2(source, target / name)
            except OSError:
                pass
    for pattern in ('instance-delegation-*.json', '*.err', '*.log'):
        for source in root.glob(pattern):
            if source.is_file() and source.stat().st_size <= 4 * 1024 * 1024:
                try:
                    shutil.copy2(source, target / source.name)
                except OSError:
                    pass
    print(f'STR09 artifacts preserved: {target}', file=sys.stderr, flush=True)
    return target


def stage(label: str) -> None:
    if MATRIX_START is None:
        print('STR09 stage: ' + label, flush=True)
        return
    print(f'STR09 stage: {label} t={time.monotonic() - MATRIX_START:.1f}s', flush=True)


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
            write_chunk({'choices': [{'delta': {}, 'finish_reason': 'stop', 'index': 0}]})
            write_chunk({'choices': [], 'usage': {'prompt_tokens': 5, 'completion_tokens': 0}})
            write_chunk_raw(handler, b'data: [DONE]\n\n')
            self._complete()
            return
        if mode in ('success', 'length', 'usage_missing', 'half_close',
                    'slow_consumer'):
            first = {'choices': [{'delta': {'content': 'P11 stream first'}, 'index': 0}]}
            write_chunk(first)
            self.first_delta.set()
            if mode == 'slow_consumer':
                for index in range(80):
                    if not write_chunk({'choices': [{'delta': {'content': ' x%d' % index},
                                                    'index': 0}]}):
                        return
                write_chunk({'choices': [{'delta': {}, 'finish_reason': 'stop', 'index': 0}]})
                write_chunk({'choices': [],
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
            write_chunk({'choices': [{'delta': {}, 'finish_reason': finish, 'index': 0}]})
            if mode != 'usage_missing':
                # OpenAI-compatible authoritative usage arrives in its own choices=[] chunk.
                write_chunk({'choices': [],
                             'usage': {'prompt_tokens': 5, 'completion_tokens': 9}})
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
    global PROVIDER, LAST_ROOT
    LAST_ROOT = root
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
        stage('runtime seed')
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
        model_lines = []
        for _ in range(2):
            text = p14.line(model)
            model_lines.append(text)
            if '=' in text:
                model_state.update(
                    dict(item.split('=', 1) for item in text.split() if '=' in item))
            if 'ready' in model_state:
                break
        # The owned service contract prints one startup line with `ready=true|false`; the value can
        # legitimately be false until dependencies settle, so the HTTP probe is the authority.
        require('ready' in model_state,
                'model startup line is missing the ready token: ' + ' | '.join(model_lines))
        print('STR09 stage: model startup: ' + ' | '.join(model_lines), flush=True)
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
        stage('admin login')
        admin = base.admin_login(identity_service, root)
        endpoint = f'http://127.0.0.1:{provider_port}/v1/chat/completions'
        stage('model seed')
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
        stage('register instance principal')
        instance_principal = base.register_instance_principal(
            identity_service, STATE['instance'], root, pg_bin, port)
        STATE['instance_principal'] = instance_principal

        # Production sealed parent writer (P05b-7) keeps the 56s delegation fresh per command.
        stage('sealed model envelope')
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

        def admin_relogin() -> None:
            stub = base.identity_stub(identity_endpoint, tls)
            credential = base.admin_login(stub, root)
            token_file.write_text(credential + '\n')
            token_file.chmod(0o600)

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
                     identity_endpoint=identity_endpoint,
                     admin_relogin=admin_relogin,
                     plugin_bootstrap=plugin_bootstrap)
        try:
            run_streaming_matrix(root, provider_state, provider_mode, endpoint)
        except Exception:
            preserve_fixture_artifacts(root, 'matrix-failure')
            raise
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
        if kind == "ping":
            self._emit({{"cid": cid, "id": logical, "event": "ping",
                        "ok": bool(self.context._rpc)}})
            return
        if kind == "chat":
            await self._chat(command, cid, logical)
            return
        chunks = []
        try:
            stream = self.context.model_stream(
                MODEL, [{{"role": "user", "text": command.get("text", "hello")}}],
                logical_call_id=logical, timeout=float(command.get("timeout", 10.0)))
            async with stream:
                started = stream.started
                self._emit({{"cid": cid, "id": logical, "event": "started",
                        "elapsed": round(time.monotonic(), 3),
                        "request_id": stream.request_id,
                        "trace_id": stream.trace_id,
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
        # The startup line may legitimately report ready=false until dependency checks settle;
        # the owned HTTP probe is the readiness authority (same contract as the first start).
        require('ready' in state, 'identity restart startup line missing ready token')
        base.wait_probe(STATE['identity_probe'], 30)
    else:
        last_error: Exception | None = None
        for attempt in range(1, 3):
            log_path = root / (name + '-restart.err')
            log = log_path.open('wb')
            child = subprocess.Popen([str(binary), '--config', str(config)],
                                     stdout=subprocess.PIPE, stderr=log)
            state = {}
            lines = []
            try:
                for _ in range(2):
                    line_text = p14.line(child, timeout=30)
                    lines.append(line_text)
                    if '=' in line_text:
                        state.update(dict(item.split('=', 1)
                                          for item in line_text.split() if '=' in item))
                    if 'ready' in state:
                        break
                require('ready' in state,
                        f'{name} restart startup line missing ready token: ' +
                        ' | '.join(lines))
                base.wait_probe(STATE[name + '_probe'], 30)
                break
            except Exception as error:  # noqa: BLE001 - bounded restart retry with evidence
                last_error = error
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=5)
                tail = ''
                try:
                    tail = ' '.join(log_path.read_text(errors='replace').split())[-600:]
                except OSError:
                    pass
                print(f'STR09 restart {name} attempt={attempt} failed: '
                      f'{type(error).__name__}: {str(error)[:300]} exit={child.returncode} '
                      f'stderr={tail}', file=sys.stderr, flush=True)
                time.sleep(1.0)
        else:
            raise Failure(f'{name} restart failed after retries: {last_error}')
    CHILDREN.append(child)
    STATE[name + '_child'] = child


def seed_managed_command(root: Path, manifest, instance, installation, command_id: str,
                         execution_generation: int = 1,
                         desired_version: int = 1) -> None:
    """Seed one additional managed-start command with explicit generation/version.

    The Host refuses to respawn a terminal execution generation by design, so an explicit graceful
    recovery requests the next generation exactly like a real HostManagement apply.
    """
    environment, pg = rt.postgres(root, Path(STATE['pg_bin']), int(STATE['pg_port']), 'p03_runtime')
    arguments = ['--manifest=' + str(manifest)]
    names = ['PYTHONPATH', 'PLUGIN_CORE_ENDPOINT', 'PLUGIN_TLS_ROOT', 'PLUGIN_INSTANCE_ID',
             'PLUGIN_IO_TIMEOUT', 'PLUGIN_HEARTBEAT_INTERVAL', 'PLUGIN_RETRY_DELAY',
             'KIARABOT_PLUGIN_BOOTSTRAP']
    now_sql = '(extract(epoch from clock_timestamp())*1000000000)::bigint'
    args_sql = 'ARRAY[' + ','.join("'" + value.replace("'", "''") + "'"
                                   for value in arguments) + ']'
    env_sql = 'ARRAY[' + ','.join("'" + value + "'" for value in names) + ']'
    payload = hashlib.sha256(('p11-str09-' + command_id).encode()).hexdigest()
    statement = (
        "INSERT INTO kiarabot.host_commands(command_id,host_id,instance_id,installation_id,"
        "kind,execution_generation,desired_version,desired_state,package_sha256,"
        "launch_profile,arguments,environment_names,payload_sha256,state,created_at,updated_at) "
        "VALUES('" + command_id + "','host-a','" + instance + "','" + installation + "',1," +
        str(execution_generation) + "," + str(desired_version) + ",0,"
        "repeat('a',64),'python'," + args_sql + ',' + env_sql + ",'" + payload + "',0," +
        now_sql + ',' + now_sql + ");")
    reply = subprocess.run([str(pg / 'psql'), '-X', '-qAt', '-v', 'ON_ERROR_STOP=1'],
                           input=statement, text=True, env=environment, capture_output=True,
                           timeout=15)
    require(reply.returncode == 0, 'managed host command restart seed: ' +
            ' '.join(str(reply.stderr).split())[-500:])


def restart_managed_host(root: Path) -> None:
    """Restart the real managed Host/plugin pair after its Runtime dependency was restarted."""
    hosted = STATE.get('plugin_host')
    # base.start_managed_host reopens host.err with 'wb', so snapshot the pre-restart evidence
    # before stopping the first Host process (preserved later by preserve_fixture_artifacts).
    for source, snapshot in (('host.err', 'host-first.err'),
                             ('plugin.log', 'plugin-first.log')):
        path = root / source
        if path.is_file():
            try:
                shutil.copy2(path, root / snapshot)
            except OSError:
                pass
    if hosted is not None and hosted.poll() is None:
        hosted.send_signal(signal.SIGTERM)
        try:
            hosted.wait(timeout=15)
        except subprocess.TimeoutExpired:
            hosted.kill()
            hosted.wait(timeout=5)
    ready = root / 'plugin-ready'
    previous_mtime = ready.stat().st_mtime_ns if ready.is_file() else 0
    command_id = 'hc-str09-' + str(uuid.uuid4())
    seed_managed_command(root, STATE['manifest_path'], STATE['instance'],
                         STATE['installation'], command_id,
                         execution_generation=2, desired_version=2)
    host = base.start_managed_host(root, Path(STATE['host_binary']))
    STATE['plugin_host'] = host
    stage('managed host restarted (generation=2), waiting for a fresh plugin-ready')
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        if ready.is_file() and ready.stat().st_mtime_ns > previous_mtime:
            break
        if host.poll() is not None:
            dump_diagnostics(root, 'managed-restart')
            raise Failure('managed host exited before the plugin re-registered')
        time.sleep(0.05)
    else:
        dump_diagnostics(root, 'managed-restart-timeout')
        raise Failure('managed host did not respawn the plugin with the new generation')
    retry_unary(root, timeout=60)


def retry_unary(root: Path, timeout=30.0) -> dict:
    """After a connection/process disruption, retry a benign unary call until the SDK reconnects."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            value = command(root, str(uuid.uuid4()), wait='chat_result', kind='chat',
                            timeout=5.0)
        except Failure as failure:
            last = str(failure)
            time.sleep(0.3)
            continue
        if value.get('ok') is True:
            return value
        last = json.dumps(value)[:300]
        time.sleep(0.3)
    dump_diagnostics(root, 'reconnect')
    preserve_fixture_artifacts(root, 'reconnect')
    raise Failure(f'reconnect unary regression did not recover: {last}')


def fixture_sql(root: Path, database: str, statement: str) -> str:
    return rt.run_sql(rt.postgres(root, Path(STATE['pg_bin']), int(STATE['pg_port']),
                                  database), statement)


def commands_path(root: Path) -> Path:
    return root / 'plugin-commands.jsonl'


ENVELOPE_TTL_SECONDS = 56
ENVELOPE_MIN_REMAINING_SECONDS = 20.0


def envelope_document(root: Path):
    path = root / f'instance-delegation-{STATE["instance"]}-model.json'
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return None


def ensure_fresh_envelope(root: Path) -> dict:
    """Hard-validate and refresh the sealed 56s parent delegation before a gate.

    The Runtime exchanges a short child delegation from this envelope; an almost expired parent
    would shorten the governed Model execution window. Refresh (re-login when the admin token is
    stale) and require a real 56s TTL with enough remaining lifetime before sending the command.
    """
    document = envelope_document(root)
    remaining = (document.get('expires_at', 0) - time.time()) if document else -1.0
    if document and document.get('ttl_seconds') == ENVELOPE_TTL_SECONDS \
            and remaining >= ENVELOPE_MIN_REMAINING_SECONDS:
        return document
    STATE['refresh_parent']()
    document = envelope_document(root)
    remaining = (document.get('expires_at', 0) - time.time()) if document else -1.0
    if not document or document.get('ttl_seconds') != ENVELOPE_TTL_SECONDS \
            or remaining < ENVELOPE_MIN_REMAINING_SECONDS:
        relogin = STATE.get('admin_relogin')
        if callable(relogin):
            relogin()
        STATE['refresh_parent']()
        document = envelope_document(root)
        remaining = (document.get('expires_at', 0) - time.time()) if document else -1.0
    require(document is not None and document.get('ttl_seconds') == ENVELOPE_TTL_SECONDS
            and remaining >= ENVELOPE_MIN_REMAINING_SECONDS,
            f'sealed parent envelope is not fresh enough: remaining={remaining:.1f}s '
            f'ttl={None if document is None else document.get("ttl_seconds")}')
    return document


def reconnect_backoff(attempt: int) -> None:
    """Bounded backoff for a transient Bridge reconnect/UPSTREAM window under load."""
    time.sleep(min(1.0 + 0.5 * attempt, 3.0))


def wait_control_plane(root: Path, timeout=12.0) -> bool:
    """Wait until the SDK has a live, registered PluginBridge session again.

    A transient Bridge close (for example a refresh rotation under load) leaves ``_rpc`` unset for
    a short reconnect window; streaming attempts during that window only produce local
    UPSTREAM_UNAVAILABLE errors. Poll a cheap plugin-side ping that never calls the Provider.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        cid = str(uuid.uuid4())
        send_command(root, cid, kind='ping', timeout=5.0, refresh=False)
        try:
            value = wait_event(root, cid, 'ping', timeout=2.0)
        except Failure:
            time.sleep(0.3)
            continue
        if value.get('ok') is True:
            return True
        time.sleep(0.3)
    return False


def fence_rotation_cancel(value: dict) -> bool:
    """A partial CANCELLED terminal without a consumer cancel: refresh rotated the fence.

    The frozen contract cancels every in-flight stream when the connection authorization refreshes
    and rotates the Fence. That is product-expected; gates whose target is COMPLETED/TRUNCATED/provider
    mapping retry it with a fresh attempt and envelope.
    """
    return (value.get('event') == 'terminal'
            and value.get('termination') == MODEL_STREAM_CANCELLED
            and value.get('partial') is True
            and (value.get('received_delta_count') or 0) > 0)


def delegation_window_attempt(value: dict, post_delta: int) -> bool:
    """Governed-window boundary: no delta yet and a pre/early-dispatch TIMEOUT/unknown failure.

    ``post_delta`` is how many Provider POSTs this attempt added (0 = delegation expired before
    dispatch, 1 = dispatched but the governed text window closed before the first delta). Both are
    retryable fixture boundaries, not product expectations for a gate that requires a delta.
    """
    if value.get('received_delta_count'):
        return False
    if value.get('event') == 'terminal':
        if value.get('termination') != MODEL_STREAM_FAILED:
            return False
        return value.get('failure_code') in (8, 9) and post_delta <= 1
    if value.get('event') == 'error':
        return value.get('code') in (8, 9) and post_delta <= 1
    return False


def delegation_expired_attempt(value: dict) -> bool:
    """True for the governed-window boundary: started, then known FAILED TIMEOUT with no delta.

    A short Runtime->Model child delegation that expires before the Provider dispatch is reported
    by the frozen contract as started followed by FAILED TIMEOUT, zero deltas, zero Provider POST
    and may_have_billed=false. It is not a product expectation for the gate; log the attempt and
    retry it with a freshly validated envelope and new attempt identity.
    """
    if value.get('event') not in ('terminal', 'error'):
        return False
    if value.get('received_delta_count'):
        return False
    if value.get('event') == 'terminal':
        return (value.get('termination') == MODEL_STREAM_FAILED
                and value.get('failure_code') in (8, 9))
    return value.get('code') in (8, 9)


def stream_gate(root: Path, cid: str, *, kind='stream', timeout=20.0, extra=None,
                logical_call_id: str | None = None, attempts=4, window_retry=True,
                prestart_timeout_retry=False, retry_fence_rotation=False):
    """Send one stream gate and wait for its first observable event (delta/terminal/error).

    Retries only the delegation-window boundary above, with zero Provider POST and zero delta; the
    first attempt journal is printed, never rewritten, and product expectations are unchanged.
    Returns ``(cid, first_event, logical_call_id, posts_before)`` for the successful attempt.
    """
    provider = STATE['provider']
    for attempt in range(1, attempts + 1):
        logical = logical_call_id or cid
        posts = provider.posts()
        send_command(root, cid, kind=kind, timeout=timeout, extra=extra,
                     logical_call_id=logical)
        value = wait_any_event(root, cid, ('delta', 'terminal', 'error'), timeout=90.0)
        post_delta = provider.posts() - posts
        prestart_window = (prestart_timeout_retry and value.get('event') == 'error'
                           and value.get('code') == 8)
        if prestart_window or (window_retry and delegation_window_attempt(value, post_delta)) \
                or (retry_fence_rotation and fence_rotation_cancel(value)):
            print(f'STR09 delegation-window retry attempt={attempt} cid={cid}: '
                  f'{json.dumps(value)[:400]}', file=sys.stderr, flush=True)
            if attempt < attempts:
                wait_control_plane(root)
                reconnect_backoff(attempt)
                cid = str(uuid.uuid4())
                continue
        return cid, value, logical, posts
    raise Failure('stream gate delegation retries exhausted')


def send_command(root: Path, cid: str, *, logical_call_id: str | None = None,
                 kind='stream', text='hello', timeout=None, extra=None,
                 refresh=True) -> None:
    """Send one plugin command.

    A fresh 56s sealed delegation is refreshed before every command by default; the
    fail-closed revocation gate opts out so the deliberately removed envelope is not restored by
    the fixture itself.
    """
    if refresh:
        ensure_fresh_envelope(root)
    command = {'cid': cid, 'kind': kind, 'text': text,
               'logical_call_id': logical_call_id or cid}
    if timeout is not None:
        command['timeout'] = timeout
    if extra:
        command.update(extra)
    with commands_path(root).open('a') as handle:
        handle.write(json.dumps(command) + '\n')


def command(root: Path, cid: str, *, wait='terminal', timeout=60.0, **kwargs) -> dict:
    """Send one plugin command and return the awaited journal event."""
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

    stage('cross-instance isolation')
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
    cid, first, _, _ = stream_gate(root, cid, timeout=20.0)
    require(first.get('event') == 'delta',
            f'cross-instance gate first delta: {first}')
    started = wait_event(root, cid, 'started', timeout=30)
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


def dump_diagnostics(root: Path, label: str) -> None:
    for name in ('plugin.log', 'host.err', 'runtime.err', 'model.err',
                 'plugin-results.jsonl', 'plugin-commands.jsonl'):
        path = root / name
        if path.is_file():
            text = ' '.join(path.read_text(errors='replace').split())[-4000:]
            print(f'STR09 diag {label} {name}: {text}', file=sys.stderr, flush=True)


def probe_streaming_bridge(root: Path) -> None:
    """Fail with an explicit STR07-unavailable marker instead of an opaque timeout.

    Until the Runtime/PluginBridge streaming surface exists, the managed SDK plugin either cannot
    negotiate ``host.model.stream.v1`` (local CAPABILITY_NOT_SUPPORTED) or the Runtime answers the
    start with a pre-start error. Both are distinguished here so the acceptance log never turns a
    missing product slice into a fixture failure.
    """
    deadline = time.monotonic() + 120.0
    attempt = 0
    provider = STATE.get('provider')
    while time.monotonic() < deadline:
        attempt += 1
        posts_before = provider.posts() if provider is not None else 0
        cid = str(uuid.uuid4())
        send_command(root, cid, timeout=8.0)
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
                            break
                        time.sleep(0.05)
                    if provider is None or provider.posts() > posts_before:
                        return
                    # Started but the governed delegation window closed before Provider dispatch:
                    # refresh the envelope and retry the probe, keeping the first attempt logged.
                    print(f'STR09 delegation-window retry probe attempt={attempt} cid={cid}',
                          file=sys.stderr, flush=True)
                    break
                if value.get('event') in ('error', 'crash'):
                    code = value.get('code')
                    if code in (1, 3, 6):
                        # INVALID_ARGUMENT / PERMISSION_DENIED / CAPABILITY_NOT_SUPPORTED on a
                        # pre-start probe mean the streaming product surface is missing.
                        detail = ('feature not enabled at registration (code=6): '
                                  'check the Runtime bridge supported-feature catalog'
                                  if code == 6 else '')
                        dump_diagnostics(root, f'probe{attempt}-rejected')
                        raise Failure(
                            'STR07 streaming bridge unavailable: pre-start/plugin error '
                            + detail + ' ' + json.dumps(value)[:500] + f' (code={code})')
                    if value.get('event') == 'crash':
                        dump_diagnostics(root, f'probe{attempt}-crash')
                        raise Failure('STR07 streaming bridge unavailable: plugin crash '
                                      + json.dumps(value)[:500])
                    # UPSTREAM_UNAVAILABLE/TIMEOUT can be a registration race in the managed
                    # plugin start; retry the probe until the overall deadline.
                    print(f'STR09 probe attempt {attempt} transient: {json.dumps(value)[:400]}',
                          file=sys.stderr, flush=True)
                    break
            else:
                time.sleep(0.05)
                continue
            break
        time.sleep(0.2)
    dump_diagnostics(root, 'probe-final')
    raise Failure('STR07 streaming bridge unavailable: no stream event after '
                  f'{attempt} probes within 40s')


def run_streaming_matrix(root: Path, provider_state: Path, mode_file: Path,
                         endpoint: str) -> None:
    global PROVIDER, MATRIX_START
    MATRIX_START = time.monotonic()
    provider = PROVIDER
    model_id = STATE['model_id']
    tls = STATE['tls']
    commands = commands_path(root)
    package, manifest_path = write_plugin_package(root)
    stage('managed plugin start')
    plugin = start_managed_plugin(root, package, manifest_path, tls,
                                  STATE['plugin_bootstrap'])
    STATE.update(plugin_host=plugin, plugin_package=package, manifest_path=manifest_path)
    try:
        require(plugin.poll() is None, 'managed plugin-host alive')
        stage('streaming bridge probe')
        probe_streaming_bridge(root)
        provider_posts = provider.posts()
        completed_posts = provider.completed_requests()
        require(provider_posts >= 1, 'streaming probe reached the provider')

        # -- gate: first delta must arrive before provider completion --
        stage('success gate (first delta before provider completion)')
        terminal = None
        success_attempt_posts = provider_posts
        success_attempt_completed = completed_posts
        for attempt in range(1, 4):
            mode_file.write_text('success\n')
            provider.release.clear()
            provider.completed.clear()
            success_attempt_completed = provider.completed_requests()
            cid, first, _, attempt_posts = stream_gate(root, str(uuid.uuid4()), timeout=20.0)
            success_attempt_posts = attempt_posts
            if first.get('event') != 'delta':
                if delegation_expired_attempt(first) and provider.posts() == success_attempt_posts:
                    wait_control_plane(root)
                    wait_control_plane(root)
                    reconnect_backoff(attempt)
                    continue
                require(False, f'success gate first event is a delta: {first}')
            require(first['index'] == 1, 'first delta is the first content delta')
            require(not provider.completed.is_set(),
                    'first delta arrived only after provider completion')
            require(provider.completed_requests() == success_attempt_completed,
                    'provider request must still be incomplete when the first delta arrives')
            provider.release.set()
            terminal = wait_event(root, cid, 'terminal', timeout=60)
            if terminal['termination'] == MODEL_STREAM_COMPLETED:
                break
            if fence_rotation_cancel(terminal):
                print(f'STR09 fence-rotation retry success-gate attempt={attempt} cid={cid}: '
                      f'{json.dumps(terminal)[:300]}', file=sys.stderr, flush=True)
                provider.release.set()
                wait_control_plane(root)
                reconnect_backoff(attempt)
                continue
            if terminal.get('failure_code') in (8, 9):
                # The governed delegation window closed mid-stream after the first delta; the
                # Provider is still gated. Preserve the attempt and rerun the gate.
                print(f'STR09 delegation-window retry success-gate attempt={attempt} cid={cid}: '
                      f'{json.dumps(terminal)[:300]}', file=sys.stderr, flush=True)
                provider.release.set()
                wait_control_plane(root)
                wait_control_plane(root)
                reconnect_backoff(attempt)
                continue
            break
        require(terminal is not None, 'success gate produced no terminal')
        if terminal['termination'] != MODEL_STREAM_COMPLETED:
            print('STR09 diag provider records: ' + json.dumps(provider.requests()[-4:]),
                  file=sys.stderr, flush=True)
            print('STR09 diag journal: ' + json.dumps(events(root, cid)),
                  file=sys.stderr, flush=True)
            try:
                print('STR09 diag model intent: ' + fixture_sql(
                    root, 'p03_model',
                    "SELECT state,error,disposition,usage_known,input_tokens,output_tokens,"
                    "may_have_billed,provider_request_id,latency_ms "
                    "FROM kiarabot.model_intents WHERE logical_call_id='" + cid + "'"),
                    file=sys.stderr, flush=True)
            except Exception:  # noqa: BLE001 - diagnostics only
                pass
        require(terminal['termination'] == MODEL_STREAM_COMPLETED,
                f'completed terminal: {terminal}')
        require(terminal['text'] == 'P11 stream first + tail', 'complete text: ' + terminal['text'])
        require(terminal['partial'] is False, 'completed stream partial=false')
        require(terminal['usage_known'] is True and terminal['input_tokens'] == 5
                and terminal['output_tokens'] == 9, f'authoritative usage: {terminal}')
        require(provider.posts() == success_attempt_posts + 1,
                'exactly one provider POST for the successful attempt')
        require(provider.completed_requests() == success_attempt_completed + 1,
                'provider completed exactly once')
        require(all(entry.get('has_usage_option')
                    for entry in provider.requests()
                    if entry.get('kind') == 'request' and entry.get('stream')),
                'streaming provider requests ask for authoritative usage')
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
            stage(label)
            mode_file.write_text(mode + '\n')
            provider.release.set()  # length/usage modes pass the gate immediately
            cid, first, _, attempt_posts = stream_gate(
                root, str(uuid.uuid4()), timeout=30.0, retry_fence_rotation=True)
            value = (first if first.get('event') in ('terminal', 'error')
                     else wait_event(root, cid, 'terminal', timeout=60))
            if value['termination'] != expected_termination:
                print('STR09 diag provider records: ' + json.dumps(provider.requests()[-4:]),
                      file=sys.stderr, flush=True)
                print('STR09 diag journal cid=' + cid + ': ' + json.dumps(events(root, cid)),
                      file=sys.stderr, flush=True)
                try:
                    print('STR09 diag model intent: ' + fixture_sql(
                        root, 'p03_model',
                        "SELECT state,error,disposition,usage_known,input_tokens,output_tokens,"
                        "may_have_billed,provider_request_id,latency_ms "
                        "FROM kiarabot.model_intents WHERE logical_call_id='"
                        + cid + "'"), file=sys.stderr, flush=True)
                    print('STR09 diag model config: ' + fixture_sql(
                        root, 'p03_model',
                        "SELECT lifecycle_state,config_version,timeout_ms,max_concurrency "
                        "FROM kiarabot.model_configs"), file=sys.stderr, flush=True)
                except Exception:  # noqa: BLE001 - diagnostics only
                    pass
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
            require(provider.posts() == attempt_posts + 1, f'{label} one provider POST')
            provider_posts = provider.posts()
            completed_posts = provider.completed_requests()

        # -- half-close / terminal loss must be unknown delivery, never COMPLETED --
        stage('half-close terminal loss')
        mode_file.write_text('half_close\n')
        provider.release.clear()
        provider.completed.clear()
        cid, first, _, attempt_posts = stream_gate(
            root, str(uuid.uuid4()), timeout=20.0, retry_fence_rotation=True)
        require(first.get('event') == 'delta', f'half-close first delta: {first}')
        provider.release.set()
        value = wait_any_event(root, cid, ('terminal', 'error'), timeout=60)
        require(value.get('termination') != MODEL_STREAM_COMPLETED,
                f'half-close must not be COMPLETED: {value}')
        if value.get('event') == 'error':
            require(value['delivery_unknown'] is True, f'half-close unknown delivery: {value}')
        require(value.get('partial') is True and value.get('text') == 'P11 stream first',
                f'half-close keeps local partial: {value}')
        require(provider.posts() == attempt_posts + 1, 'half-close one provider POST')
        provider_posts = provider.posts()

        # -- provider HTTP error mapping: stable codes, no terminal-based success --
        for mode, expected_code in [('status_429', 7), ('status_500', 9), ('status_400', 1),
                                    ('status_401', 9), ('status_404', 6)]:
            print(f'STR09 stage: provider {mode}', flush=True)
            mode_file.write_text(mode + '\n')
            value = None
            for attempt in range(1, 4):
                posts = provider.posts()
                cid, value, _, attempt_posts = stream_gate(
                    root, str(uuid.uuid4()), timeout=20.0, window_retry=False)
                if value.get('event') == 'delta':
                    value = wait_any_event(root, cid, ('terminal', 'error'), timeout=60)
                observed = (value.get('failure_code') if value.get('event') == 'terminal'
                            else value.get('code'))
                if ((observed == 8 and expected_code != 8
                     or fence_rotation_cancel(value))
                        and attempt < 3 and provider.posts() - posts <= 1):
                    print(f'STR09 delegation-window retry provider {mode} attempt={attempt}: '
                          f'{json.dumps(value)[:300]}', file=sys.stderr, flush=True)
                    reconnect_backoff(attempt)
                    continue
                break
            require(value.get('termination') != MODEL_STREAM_COMPLETED,
                    f'{mode} must not be COMPLETED: {value}')
            if value.get('event') == 'terminal':
                require(value.get('failure_code') == expected_code,
                        f'{mode} terminal failure code expected={expected_code}: {value}')
            else:
                require(value.get('code') == expected_code,
                        f'{mode} stable code expected={expected_code}: {value}')
            require(provider.posts() == attempt_posts + 1, f'{mode} one provider POST')
            provider_posts = provider.posts()
        mode_file.write_text('success\n')
        provider.release.set()

        # -- explicit cancel: typed receipt, no fabricated not-executed/free claim --
        stage('explicit cancel')
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        cid, first, _, attempt_posts = stream_gate(
            root, str(uuid.uuid4()), kind='cancel', timeout=20.0)
        require(first.get('event') == 'delta', f'cancel gate first delta: {first}')
        receipt = wait_event(root, cid, 'cancel_receipt', timeout=30)
        require(receipt['disposition'] in (1, 2, 3), f'cancel receipt: {receipt}')
        provider.release.set()
        value = wait_event(root, cid, 'terminal', timeout=60)
        require(value['termination'] in (MODEL_STREAM_CANCELLED, MODEL_STREAM_FAILED),
                f'cancel terminal: {value}')
        require(value['termination'] != MODEL_STREAM_COMPLETED, 'cancel cannot be COMPLETED')
        require(provider.posts() == attempt_posts + 1, 'cancel one provider POST')
        provider_posts = provider.posts()

        # -- deadline while the provider is gated after dispatch (3s > Model minimum budget) --
        stage('deadline')
        provider.release.clear()
        provider.completed.clear()
        cid, first, _, attempt_posts = stream_gate(  # deadline clips Model execution
            root, str(uuid.uuid4()), timeout=3.0, attempts=3)
        value = (first if first.get('event') in ('terminal', 'error')
                 else wait_any_event(root, cid, ('terminal', 'error'), timeout=60))
        require(value.get('event') == 'error'
                or value.get('termination') in (MODEL_STREAM_CANCELLED, MODEL_STREAM_FAILED),
                f'deadline terminal: {value}')
        require(value.get('termination') != MODEL_STREAM_COMPLETED,
                'deadline cannot be COMPLETED')
        provider.release.set()
        require(provider.posts() >= attempt_posts + 1,
                'deadline attempt reached the provider before the text deadline')
        provider_posts = provider.posts()

        # -- slow consumer / bounded SDK queue: local unknown + best-effort cancel --
        stage('slow consumer queue overflow')
        mode_file.write_text('slow_consumer\n')
        cid, first, _, attempt_posts = stream_gate(
            root, str(uuid.uuid4()), kind='no_read', timeout=20.0,
            extra={'sleep': 3.0}, retry_fence_rotation=True)
        value = (first if first.get('event') in ('terminal', 'error')
                 else wait_any_event(root, cid, ('terminal', 'error'), timeout=60))
        require(value.get('termination') != MODEL_STREAM_COMPLETED,
                f'slow consumer must not be COMPLETED: {value}')
        require(provider.posts() == attempt_posts + 1, 'slow consumer one provider POST')
        provider_posts = provider.posts()
        # Control frames stay alive: a unary model_chat after the overflow still works. The
        # reader lane can briefly report a transient unknown while the overflowed stream unwinds,
        # so retry the benign unary call (each attempt is a real, logged POST).
        mode_file.write_text('success\n')
        value = retry_unary(root)
        require(value['text'] == 'p11-unary-fixture reply',
                f'unary regression after stream: {value}')
        provider_posts = provider.posts()

        # -- concurrent same logical id: one fresh, one replay, exactly one provider POST --
        stage('same logical id concurrency')
        concurrency_ok = False
        for attempt in range(1, 4):
            logical = str(uuid.uuid4())
            cid_a, cid_b = str(uuid.uuid4()), str(uuid.uuid4())
            mode_file.write_text('success\n')
            provider.release.clear()
            provider.completed.clear()
            posts = provider.posts()
            send_command(root, cid_a, logical_call_id=logical, timeout=20.0)
            send_command(root, cid_b, logical_call_id=logical, timeout=20.0)
            first_a = wait_any_event(root, cid_a, ('started', 'terminal', 'error'), timeout=30)
            first_b = wait_any_event(root, cid_b, ('started', 'terminal', 'error'), timeout=30)
            early = [value for value in (first_a, first_b)
                     if value.get('event') in ('terminal', 'error')]
            if early and all(delegation_window_attempt(value, provider.posts() - posts)
                             for value in early):
                print(f'STR09 delegation-window retry concurrency attempt={attempt}: '
                      f'{json.dumps(early)[:300]}', file=sys.stderr, flush=True)
                provider.release.set()
                reconnect_backoff(attempt)
                continue
            started_a = wait_event(root, cid_a, 'started', timeout=30)
            started_b = wait_event(root, cid_b, 'started', timeout=30)
            dispositions = {started_a['disposition'], started_b['disposition']}
            require(1 in dispositions, f'exactly one FRESH admission: {dispositions}')
            require(dispositions - {1} <= {2, 3},
                    f'other attempt is replay metadata: {dispositions}')
            provider.release.set()
            terminals = {cid_a: wait_event(root, cid_a, 'terminal', timeout=60),
                         cid_b: wait_event(root, cid_b, 'terminal', timeout=60)}
            replay_values = [value for value in terminals.values()
                             if value['termination'] == MODEL_STREAM_REPLAY]
            fresh = [value for value in terminals.values()
                     if value['termination'] == MODEL_STREAM_COMPLETED]
            if len(replay_values) != 1 or len(fresh) != 1:
                if (any(delegation_expired_attempt(value) for value in terminals.values())
                        or any(fence_rotation_cancel(value) for value in terminals.values())) \
                        and attempt < 3:
                    print(f'STR09 delegation-window retry concurrency terminals attempt={attempt}: '
                          f'{json.dumps(terminals)[:400]}', file=sys.stderr, flush=True)
                    reconnect_backoff(attempt)
                    continue
                require(False, f'exactly one replay + one completed terminal: {terminals}')
            require(replay_values[0]['received_delta_count'] == 0
                    and replay_values[0]['text'] == '', 'concurrent replay has zero delta')
            require(provider.posts() == posts + 1,
                    'concurrent same logical id calls the provider once')
            provider_posts = provider.posts()
            concurrency_ok = True
            break
        require(concurrency_ok, 'concurrent same logical id gate did not converge')

        # -- replay: same logical id after terminal, zero delta, no provider re-invocation --
        stage('replay by original logical id')
        logical = str(uuid.uuid4())
        mode_file.write_text('success\n')
        provider.release.set()
        for attempt in range(1, 4):
            cid, first, logical, attempt_posts = stream_gate(
                root, str(uuid.uuid4()), timeout=20.0)
            value = (first if first.get('event') in ('terminal', 'error')
                     else wait_event(root, cid, 'terminal', timeout=60))
            if fence_rotation_cancel(value) and attempt < 3:
                print(f'STR09 fence-rotation retry replay-source attempt={attempt} '
                      f'cid={cid}: {json.dumps(value)[:300]}', file=sys.stderr, flush=True)
                wait_control_plane(root)
                reconnect_backoff(attempt)
                continue
            break
        require(value.get('termination') == MODEL_STREAM_COMPLETED,
                f'replay-source completed: {value}')
        require(provider.posts() == attempt_posts + 1, 'fresh replay-source one provider POST')
        provider_posts = provider.posts()
        replay_cid = str(uuid.uuid4())
        value = command(root, replay_cid, logical_call_id=logical, timeout=20.0)
        require(value['termination'] == MODEL_STREAM_REPLAY, f'replay terminal: {value}')
        require(value['received_delta_count'] == 0 and value['text'] == '',
                f'replay zero delta: {value}')
        require(provider.posts() == provider_posts, 'replay must not call the provider again')

        # -- txn2 commit failure injection (real PG trigger, no product-code change) --
        stage('txn2 commit failure injection')
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        value = None
        for attempt in range(1, 4):
            logical = str(uuid.uuid4())
            fixture_sql(root, 'p03_model',
                "CREATE OR REPLACE FUNCTION kiarabot.p11_txn2_fail() RETURNS trigger "
                "LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'p11 txn2 injected failure'; END $$; "
                "CREATE TRIGGER p11_txn2_fail BEFORE UPDATE ON kiarabot.model_intents "
                "FOR EACH ROW WHEN (OLD.logical_call_id = '" + logical + "') "
                "EXECUTE FUNCTION kiarabot.p11_txn2_fail();")
            posts = provider.posts()
            try:
                cid, first, _, _ = stream_gate(root, str(uuid.uuid4()),
                                               logical_call_id=logical,
                                               timeout=20.0, attempts=1)
                if first.get('event') == 'delta':
                    provider.release.set()
                    value = wait_any_event(root, cid, ('terminal', 'error'), timeout=90)
                else:
                    value = first
            finally:
                fixture_sql(root, 'p03_model',
                    'DROP TRIGGER IF EXISTS p11_txn2_fail ON kiarabot.model_intents; '
                    'DROP FUNCTION IF EXISTS kiarabot.p11_txn2_fail();')
            if value is not None and (delegation_window_attempt(
                    value, provider.posts() - posts) or fence_rotation_cancel(value)) \
                    and attempt < 3:
                print(f'STR09 delegation-window retry txn2 attempt={attempt} '
                      f'cid={cid}: {json.dumps(value)[:400]}', file=sys.stderr, flush=True)
                reconnect_backoff(attempt)
                continue
            break
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
        stage('sealed parent revocation (new call)')
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
            send_command(root, cid, timeout=15.0, refresh=False)
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
        stage('identity unreachable in-flight')
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        cid, first, _, _ = stream_gate(root, str(uuid.uuid4()), timeout=20.0)
        require(first.get('event') == 'delta', f'identity gate first delta: {first}')
        started_wall = time.monotonic()
        base.stop(STATE['identity_child'])
        try:
            value = wait_any_event(root, cid, ('terminal', 'error'), timeout=45)
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
        stage('in-flight instance revocation')
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        cid, first, _, _ = stream_gate(root, str(uuid.uuid4()), timeout=20.0)
        require(first.get('event') == 'delta', f'revocation gate first delta: {first}')
        principal = STATE['instance_principal']
        revoked_wall = time.monotonic()
        fixture_sql(root, 'p03_identity',
                    "UPDATE kiarabot.principals SET state=1 WHERE id='" + principal + "';")
        try:
            value = wait_any_event(root, cid, ('terminal', 'error'), timeout=45)
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
        stage('bridge disconnect mid-stream')
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        cid, first, _, _ = stream_gate(root, str(uuid.uuid4()), timeout=20.0)
        require(first.get('event') == 'delta', f'bridge gate first delta: {first}')
        base.stop(STATE['runtime_child'])
        try:
            value = wait_any_event(root, cid, ('terminal', 'error'), timeout=45)
            require(value.get('event') == 'error'
                    or value.get('termination') != MODEL_STREAM_COMPLETED,
                    f'bridge disconnect must not complete the stream: {value}')
        finally:
            provider.release.set()
            restart_service(root, 'runtime')
        # A graceful Runtime stop delivers ShutdownNotice; the SDK drains and does not reconnect
        # (frozen contract). Assert no automatic resume, then recover through the real managed
        # Host path with the explicit next execution generation.
        probe_cid = str(uuid.uuid4())
        send_command(root, probe_cid, kind='chat', timeout=5.0, refresh=False)
        time.sleep(2.0)
        require(not events(root, probe_cid),
                'graceful shutdown must not auto-resume the SDK on a new connection')
        restart_managed_host(root)

        # -- Model restart mid-stream: unknown delivery, then restart recovers --
        stage('model restart mid-stream')
        mode_file.write_text('success\n')
        provider.release.clear()
        provider.completed.clear()
        cid, first, _, _ = stream_gate(root, str(uuid.uuid4()), timeout=20.0)
        require(first.get('event') == 'delta', f'model-restart gate first delta: {first}')
        base.stop(STATE['model_child'])
        try:
            value = wait_any_event(root, cid, ('terminal', 'error'), timeout=45)
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
        hosted = STATE.get('plugin_host', plugin)
        if hosted is not None and hosted.poll() is None:
            hosted.send_signal(signal.SIGTERM)
            try:
                hosted.wait(timeout=15)
            except subprocess.TimeoutExpired:
                hosted.kill()
                hosted.wait(timeout=5)


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
        preserve_fixture_artifacts(LAST_ROOT, 'after-bootstrap-failure')
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
