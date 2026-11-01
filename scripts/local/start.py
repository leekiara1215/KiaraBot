#!/usr/bin/env python3
"""Owned, loopback reference MVP lifecycle. No PG or target six-service startup."""
import argparse
import fcntl
import http.client
import json
import os
from pathlib import Path
import queue
import re
import secrets
import shutil
import signal
import stat
import subprocess
import sys
import threading
import time
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]


class Failure(Exception):
    pass


def check(ok, reason):
    if not ok:
        raise Failure(reason)


def private_file(path, required=True):
    try:
        info = path.lstat()
    except FileNotFoundError:
        check(not required, 'required_private_file_missing')
        return False
    check(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
          and stat.S_IMODE(info.st_mode) == 0o600, 'unsafe_private_file')
    return True


def write(path, value):
    if path.exists() or path.is_symlink():
        private_file(path)
    temporary = path.parent / ('.write-' + secrets.token_hex(12))
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, 'w') as stream:
            stream.write(value)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def read(path):
    private_file(path)
    return path.read_text()


def state_directory(value):
    path = Path(os.path.abspath(value))
    for component in [*reversed(path.parents), path]:
        check(not component.is_symlink(), 'symlink_state_directory')
    if not path.exists():
        path.mkdir(parents=True, mode=0o700)
    info = path.stat()
    check(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid()
          and stat.S_IMODE(info.st_mode) == 0o700, 'unsafe_state_directory')
    marker = path / 'owner.json'
    if not marker.exists():
        check(not any(path.iterdir()), 'unowned_nonempty_directory')
        write(marker, json.dumps({'format': 1, 'repository': str(ROOT),
                                 'owner': secrets.token_hex(32)}))
    owner = json.loads(read(marker))
    check(owner.get('format') == 1 and owner.get('repository') == str(ROOT)
          and re.fullmatch('[a-f0-9]{64}', owner.get('owner', '')), 'invalid_directory_owner')
    return path


def lock(path):
    file = path / 'lock'
    if file.exists() or file.is_symlink():
        private_file(file)
    descriptor = os.open(file, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(descriptor)
        raise Failure('already_running_or_starting') from None
    return descriptor


def process_identity(pid):
    result = subprocess.run(['ps', '-p', str(pid), '-o', 'uid=,lstart=,command='],
                            capture_output=True, text=True, timeout=2)
    return result.stdout.strip() if result.returncode == 0 else ''


def runtime(path):
    file = path / 'run.json'
    return json.loads(read(file)) if file.exists() else {}


def owned_alive(path, state):
    pid = state.get('pid')
    identity = process_identity(pid) if isinstance(pid, int) and pid > 1 else ''
    return bool(identity and identity == state.get('identity') and '_run' in identity
                and str(path) in identity and state.get('run_id', '') in identity
                and identity.split()[0] == str(os.getuid()))


def live(port, target='/health/live'):
    try:
        connection = http.client.HTTPConnection('127.0.0.1', int(port), timeout=1)
        connection.request('GET', target)
        response = connection.getresponse()
        response.read()
        connection.close()
        return response.status == 200
    except (OSError, ValueError, http.client.HTTPException):
        return False


def status(path):
    state = runtime(path)
    alive = owned_alive(path, state)
    ready = alive and state.get('state') == 'running' and live(state.get('control_port', 0))
    return {**state, 'alive': alive, 'healthy': ready,
            'state': state.get('state', 'stopped') if alive else 'stopped',
            'password_file': str(path / 'admin-password')}


def first_existing(candidates):
    return str(next((path for path in candidates if path.is_file()), candidates[0]))


def configure(path, args):
    file = path / 'settings.json'
    config = json.loads(read(file)) if file.exists() else {
        'core': first_existing([ROOT / 'build/full-locked/reference/core/kiarabot-core',
                                ROOT / 'build/full/reference/core/kiarabot-core',
                                ROOT / 'build/full/core/kiarabot-core',
                                ROOT / 'build/proto/core/kiarabot-core']),
        'python': first_existing([ROOT / 'adapters/nonebot/.venv/bin/python',
                                  ROOT / 'sdk/python/.venv/bin/python']),
        'manifest': str(ROOT / 'plugins/local-reply/plugin.json'),
        'account': '123', 'conversation': '456', 'mode': 'demo', 'url': None,
        'core_only': False, 'control_port': 0, 'grpc_port': 0, 'ui': False, 'ui_port': 5173}
    for flag, field in [('core', 'core'), ('credentials', 'credentials'), ('python', 'python'),
                        ('plugin_manifest', 'manifest'), ('account', 'account'),
                        ('conversation', 'conversation'), ('core_only', 'core_only'),
                        ('control_port', 'control_port'), ('grpc_port', 'grpc_port'),
                        ('ui', 'ui'), ('ui_port', 'ui_port')]:
        value = getattr(args, flag, None)
        if value is not None:
            config[field] = str(Path(value).absolute()) if flag in (
                'core', 'credentials', 'python', 'plugin_manifest') else value
    if not config.get('credentials'):
        config['credentials'] = str(Path(config['core']).parent.parent /
                                    'control/api/kiarabot-control-credentials')
    check(re.fullmatch('[1-9][0-9]{0,19}', config['account'])
          and re.fullmatch('[1-9][0-9]{0,19}', config['conversation']), 'invalid_account_or_conversation')
    check(0 < config['ui_port'] < 65536, 'invalid_ui_port')
    demo, real = getattr(args, 'demo_url', None), getattr(args, 'onebot_url', None)
    check(not (demo and real), 'conflicting_platform_modes')
    if demo or real:
        parsed = urlsplit(demo or real)
        check(parsed.scheme in ('ws', 'wss') and parsed.hostname and not parsed.username
              and not parsed.password and not parsed.query and not parsed.fragment
              and (parsed.port is None or 0 < parsed.port < 65536),
              'invalid_onebot_url')
        if demo:
            check(parsed.hostname in ('127.0.0.1', 'localhost', '::1'), 'demo_requires_loopback')
        config.update(mode='real' if real else 'demo', url=demo or real)
    for name in ['admin-password', 'adapter-token', 'plugin-token', 'onebot-token']:
        secret = path / name
        if not private_file(secret, False):
            write(secret, secrets.token_urlsafe(32) + '\n')
    if getattr(args, 'onebot_token_file', None):
        token = read(Path(args.onebot_token_file).absolute()).strip()
        check(token and '\n' not in token and '\r' not in token, 'invalid_onebot_token')
        write(path / 'onebot-token', token + '\n')
    check(not real or getattr(args, 'onebot_token_file', None), 'real_mode_requires_token_file')
    control_file = path / 'control.json'
    if not control_file.exists():
        tool = Path(config['credentials'])
        check(tool.is_file() and os.access(tool, os.X_OK), 'credentials_tool_missing')
        hashed = subprocess.run([str(tool)], input=read(path / 'admin-password'),
                                capture_output=True, text=True, timeout=10)
        check(hashed.returncode == 0 and hashed.stdout.strip().startswith('$argon2id$'),
              'password_hash_failed')
        write(control_file, json.dumps({'listen': f"127.0.0.1:{config['control_port']}",
              'encryption_key': secrets.token_hex(32), 'cors_origins': [],
              'users': [{'id': 'admin', 'username': 'admin',
                         'password_hash': hashed.stdout.strip(), 'role': 'admin'}]}))
    control = json.loads(read(control_file))
    control['listen'] = f"127.0.0.1:{config['control_port']}"
    control['cors_origins'] = [f"http://127.0.0.1:{config['ui_port']}"] if config['ui'] else []
    write(control_file, json.dumps(control))
    write(file, json.dumps(config, indent=2) + '\n')
    return config


def clean_environment():
    return {key: value for key, value in os.environ.items()
            if not key.startswith(('KIARABOT_', 'CORE_', 'PLUGIN_', 'ONEBOT_', 'VITE_'))}


def supervise(path, descriptor, run_id):
    check(os.fstat(descriptor).st_ino == (path / 'lock').stat().st_ino, 'invalid_lock_handle')
    fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    config = json.loads(read(path / 'settings.json'))
    stop = threading.Event()
    for name in (signal.SIGINT, signal.SIGTERM):
        signal.signal(name, lambda *_: stop.set())
    state = {'pid': os.getpid(), 'identity': process_identity(os.getpid()), 'run_id': run_id,
             'state': 'starting', 'mode': config['mode'], 'account': config['account'], 'children': []}
    write(path / 'run.json', json.dumps(state))
    sensitive = [read(path / name).strip() for name in
                 ['admin-password', 'adapter-token', 'plugin-token', 'onebot-token']]
    sensitive.append(json.loads(read(path / 'control.json'))['encryption_key'])
    children, readers = [], []
    lines = queue.Queue(maxsize=512)
    log_file = path / 'process.log'
    if log_file.exists() or log_file.is_symlink():
        private_file(log_file)
    log_descriptor = os.open(log_file, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
    log = os.fdopen(log_descriptor, 'w', buffering=1)
    log_lock = threading.Lock()

    def spawn(label, command, environment):
        child = subprocess.Popen(command, cwd=ROOT, env=environment, start_new_session=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        children.append(child)
        state['children'].append({'name': label, 'pid': child.pid,
                                  'identity': process_identity(child.pid)})
        write(path / 'run.json', json.dumps(state))

        def pump():
            while raw := child.stdout.readline(65536):
                encoded = raw.decode(errors='replace')
                for secret in sensitive:
                    encoded = encoded.replace(secret, '[redacted]')
                # Framework tracebacks/URLs/bodies are not a diagnostic channel.
                fields = re.findall(r'\b(operation|outcome|error_code|port|rpc_ready)=([A-Za-z0-9_.:-]{1,64})\b', encoded)
                try:
                    structured = json.loads(encoded)
                    if isinstance(structured, dict):
                        fields += [(key, str(structured[key])) for key in
                                   ('operation', 'outcome', 'error_code')
                                   if key in structured and re.fullmatch(
                                       '[A-Za-z0-9_.:-]{1,64}', str(structured[key]))]
                except ValueError:
                    pass
                safe = ' '.join(key + '=' + value for key, value in fields)
                with log_lock:
                    log.write(label + ' ' + (safe or 'operation=child_output redacted=true') + '\n')
                try:
                    lines.put_nowait((label, encoded))
                except queue.Full:
                    pass
            child.stdout.close()
        thread = threading.Thread(target=pump, daemon=True)
        readers.append(thread)
        thread.start()
        return child

    result = 1
    try:
        core = Path(config['core'])
        check(core.is_file() and os.access(core, os.X_OK), 'reference_core_missing')
        environment = clean_environment()
        environment.update(KIARABOT_CONTROL_CONFIG=str(path / 'control.json'),
            KIARABOT_RUNTIME_DB=str(path / 'runtime.db'), KIARABOT_ADAPTER_ID='local-adapter',
            KIARABOT_ADAPTER_TOKEN=sensitive[1], KIARABOT_ADAPTER_ACCOUNTS=config['account'],
            KIARABOT_ACTION_ALLOWED_CONVERSATIONS='message.send=' + config['conversation'])
        if not config['core_only']:
            manifest = Path(config['manifest'])
            check(manifest.is_file(), 'fixed_sender_manifest_missing')
            metadata = json.loads(manifest.read_text())
            check(set(metadata['permissions']) <= {'message.read', 'message.send'}
                  and 'message.send' in metadata['permissions'], 'unexpected_sender_permissions')
            environment.update(KIARABOT_PLUGIN_ID=metadata['id'], KIARABOT_PLUGIN_TOKEN=sensitive[2],
                KIARABOT_PLUGIN_PERMISSIONS=','.join(metadata['permissions']),
                KIARABOT_PLUGIN_EXECUTABLE=config['python'], KIARABOT_PLUGIN_MANIFEST=str(manifest),
                KIARABOT_PLUGIN_PYTHONPATH=os.pathsep.join(map(str, [ROOT / 'sdk/python/src',
                    ROOT / 'sdk/python/build/generated', manifest.parent])))
        adapter_url = config['url']
        if config['mode'] == 'demo' and adapter_url is None and not config['core_only']:
            demo = ROOT / 'scripts/local/onebot_demo.py'
            check(demo.is_file(), 'demo_server_missing')
            demo_socket = path / 'demo.sock'
            demo_child = spawn('demo', [config['python'], str(demo), 'serve', '--token-file',
                str(path / 'onebot-token'), '--control-socket', str(demo_socket), '--port', '0',
                '--account-id', config['account']], clean_environment())
            demo_deadline = time.monotonic() + 8
            while True:
                check(time.monotonic() < demo_deadline and demo_child.poll() is None
                      and not stop.is_set(), 'demo_start_failed')
                try:
                    label, encoded = lines.get(timeout=.1)
                except queue.Empty:
                    continue
                if label != 'demo':
                    continue
                try:
                    ready = json.loads(encoded)
                except ValueError:
                    continue
                if ready.get('event') == 'ready':
                    check(ready.get('host') == '127.0.0.1' and isinstance(ready.get('port'), int)
                          and 0 < ready['port'] < 65536
                          and str(ready.get('account_id')) == config['account']
                          and ready.get('control_socket') == str(demo_socket), 'invalid_demo_ready')
                    adapter_url = f"ws://127.0.0.1:{ready['port']}"
                    state.update(demo_port=ready['port'], demo_socket=str(demo_socket))
                    break
        core_child = spawn('core', [str(core), '--grpc-listen', f"127.0.0.1:{config['grpc_port']}"], environment)
        deadline = time.monotonic() + 15
        waiting = True
        while waiting and not stop.is_set():
            check(time.monotonic() < deadline and core_child.poll() is None, 'core_start_failed')
            try:
                label, line = lines.get(timeout=.1)
            except queue.Empty:
                continue
            if label == 'core':
                if match := re.search(r'operation=control_start port=(\d+)', line):
                    state['control_port'] = int(match[1])
                if match := re.search(r'transport_listening=true port=(\d+)', line):
                    state['grpc_port'] = int(match[1])
                waiting = 'operation=process_start outcome=waiting' not in line
        check(not stop.is_set() and state.get('control_port') and state.get('grpc_port')
              and live(state['control_port']), 'control_not_healthy')
        if adapter_url:
            environment = clean_environment()
            environment.update(CORE_GRPC_ENDPOINT=f"127.0.0.1:{state['grpc_port']}",
                CORE_AUTH_TOKEN=sensitive[1], ADAPTER_ID='local-adapter', ACCOUNT_ID=config['account'],
                ONEBOT_WS_URL=adapter_url, ONEBOT_ACCESS_TOKEN=sensitive[3],
                OUTBOX_PATH=str(path / 'outbox.db'), HEALTH_PORT='0', PYTHONUNBUFFERED='1',
                PYTHONPATH=os.pathsep.join(map(str, [ROOT / 'adapters/nonebot/src',
                    ROOT / 'adapters/nonebot/generated', ROOT / 'sdk/python/build/generated'])))
            spawn('adapter', [config['python'], '-m', 'kiarabot_adapter_nonebot'], environment)
        if config['ui']:
            vite = ROOT / 'panel/node_modules/vite/bin/vite.js'
            check(vite.is_file() and shutil.which('node'), 'panel_dependencies_missing')
            environment = clean_environment()
            environment.update(VITE_API_MODE='live',
                VITE_API_BASE=f"http://127.0.0.1:{state['control_port']}/api/v1",
                VITE_WS_URL=f"ws://127.0.0.1:{state['control_port']}/ws/control")
            spawn('panel', [shutil.which('node'), str(vite), str(ROOT / 'panel'), '--host',
                            '127.0.0.1', '--port', str(config['ui_port']), '--strictPort'], environment)
            state['ui_url'] = f"http://127.0.0.1:{config['ui_port']}"
        if state.get('demo_socket'):
            connected_deadline = time.monotonic() + 10
            while True:
                check(not stop.is_set() and time.monotonic() < connected_deadline
                      and all(child.poll() is None for child in children), 'demo_adapter_not_connected')
                result_status = subprocess.run(
                    [config['python'], str(ROOT / 'scripts/local/onebot_demo.py'), 'status',
                     '--token-file', str(path / 'onebot-token'), '--control-socket', state['demo_socket']],
                    capture_output=True, text=True, timeout=3)
                if result_status.returncode == 0:
                    snapshot = json.loads(result_status.stdout)
                    if snapshot.get('connected') is True:
                        state['platform_connected'] = True
                        break
                stop.wait(.1)
        # Catch dependency/import/bind failures before reporting success; no platform readiness claim.
        check(not stop.wait(1) and all(child.poll() is None for child in children), 'child_start_failed')
        state.update(state='running', control_url=f"http://127.0.0.1:{state['control_port']}",
                     platform_configured=bool(adapter_url), sender_enabled=not config['core_only'])
        write(path / 'run.json', json.dumps(state))
        while not stop.wait(.2):
            check(all(child.poll() is None for child in children), 'child_exited')
            check(live(state['control_port']), 'control_health_lost')
        result = 0
    except Failure as error:
        state['reason'] = str(error)
    except Exception:
        state['reason'] = 'startup_or_child_failed'
    finally:
        failures = []
        for child in reversed(children):
            try:
                if child.poll() is None:
                    try:
                        child.terminate()
                    except ProcessLookupError:
                        pass
                try:
                    child.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    if child.poll() is None:
                        try:
                            os.killpg(child.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    child.wait(timeout=3)
                    failures.append('forced_child_stop')
                if child.returncode not in (0, -signal.SIGTERM):
                    failures.append('child_failed')
            except (OSError, subprocess.SubprocessError):
                failures.append('child_cleanup_failed')
        for thread in readers:
            thread.join(timeout=2)
        result = 1 if failures else result
        state.update(state='stopped' if result == 0 else 'failed', exit_code=result)
        write(path / 'run.json', json.dumps(state))
        log.close()
        os.close(descriptor)
    return result


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['init', 'start', 'status', 'stop', '_run'])
    parser.add_argument('--state-dir', default=str(ROOT / 'data/local'))
    for name in ['core', 'credentials', 'python', 'plugin-manifest', 'account', 'conversation',
                 'demo-url', 'onebot-url', 'onebot-token-file']:
        parser.add_argument('--' + name)
    for name in ['control-port', 'grpc-port', 'ui-port']:
        parser.add_argument('--' + name, type=int, choices=range(0, 65536), metavar='PORT')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--core-only', action='store_true', default=None)
    mode.add_argument('--with-sender', dest='core_only', action='store_false')
    parser.add_argument('--ui', action='store_true', default=None)
    parser.add_argument('--lock-fd', type=int)
    parser.add_argument('--run-id')
    parser.set_defaults(core_only=None, ui=None)
    args = parser.parse_args()
    path = state_directory(args.state_dir)
    if args.command == '_run':
        check(args.lock_fd is not None and args.run_id, 'internal_arguments_missing')
        return supervise(path, args.lock_fd, args.run_id)
    current = status(path)
    if args.command == 'status':
        print(json.dumps(current))
        return 0 if current['healthy'] else 1
    if args.command == 'stop':
        previous = runtime(path)
        if owned_alive(path, previous):
            os.kill(previous['pid'], signal.SIGTERM)
            deadline = time.monotonic() + 35
            while owned_alive(path, previous) and time.monotonic() < deadline:
                time.sleep(.1)
            check(not owned_alive(path, previous), 'owned_supervisor_stop_timeout')
        elif previous.get('state') in ('running', 'starting'):
            raise Failure('stale_or_foreign_pid_refusing_signal')
        result = status(path)
        print(json.dumps(result))
        return 0 if result.get('exit_code', 0) == 0 else 1
    if current['alive']:
        check(not any(getattr(args, field) is not None for field in
                      ('core', 'credentials', 'python', 'plugin_manifest', 'account',
                       'conversation', 'demo_url', 'onebot_url', 'onebot_token_file',
                       'core_only', 'control_port', 'grpc_port', 'ui', 'ui_port')),
              'stop_before_reconfiguring_running_instance')
        check(args.command == 'start' and current['healthy'], 'already_running_or_starting')
        print(json.dumps(current))
        return 0
    previous = runtime(path)
    check(previous.get('state') not in ('running', 'starting'),
          'stale_or_foreign_state_requires_inspection')
    for child in previous.get('children', []):
        identity = process_identity(child.get('pid', 0))
        check(not identity or identity != child.get('identity'), 'owned_orphan_requires_inspection')
    descriptor = lock(path)
    try:
        configure(path, args)
        if args.command == 'init':
            print(json.dumps({'state': 'initialized', 'state_dir': str(path),
                              'username': 'admin', 'password_file': str(path / 'admin-password')}))
            return 0
        run_id = secrets.token_hex(16)
        child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '_run',
            '--state-dir', str(path), '--lock-fd', str(descriptor), '--run-id', run_id],
            pass_fds=(descriptor,), start_new_session=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    finally:
        os.close(descriptor)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        current = status(path)
        if current.get('run_id') == run_id and current['healthy']:
            print(json.dumps(current))
            return 0
        check(child.poll() is None, 'supervisor_start_failed_see_private_log')
        time.sleep(.1)
    child.terminate()
    child.wait(timeout=35)
    raise Failure('supervisor_start_timeout')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Failure as error:
        print('operation=local_launcher outcome=failed reason=' + str(error), file=sys.stderr)
        sys.exit(1)
    except Exception:
        print('operation=local_launcher outcome=failed reason=configuration_or_lifecycle_error; '
              'inspect_private_state_and_process_log', file=sys.stderr)
        sys.exit(1)
