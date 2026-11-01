#!/usr/bin/env python3
"""Local NapCat deployment driver; business account ownership stays in Core."""
import argparse
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

from start import Failure, ROOT, check, lock, read, state_directory, write

IMAGE = 'mlikiowa/napcat-docker:latest'
OWNER_LABEL = 'io.kiarabot.napcat-owner'


def command(arguments, environment=None, timeout=30):
    result = subprocess.run(arguments, env=environment, capture_output=True, text=True,
                            timeout=timeout)
    check(result.returncode == 0, 'external_command_failed_' + Path(arguments[0]).name)
    return result.stdout


def settings(path):
    marker = json.loads(read(path / 'owner.json'))
    vm = path / 'vm.json'
    if not vm.exists():
        write(vm, json.dumps({'profile': 'kiarabot-' + marker['owner'][:8]}))
    profile = json.loads(read(vm))['profile']
    check(re.fullmatch('kiarabot(?:-[a-f0-9]{8})?', profile), 'invalid_owned_profile')
    environment = {key: value for key, value in os.environ.items()
                   if key not in ('DOCKER_HOST', 'DOCKER_CONTEXT', 'DOCKER_TLS_VERIFY',
                                  'DOCKER_CERT_PATH')}
    if sys.platform == 'darwin':
        environment['DOCKER_HOST'] = 'unix://' + str(Path.home() / '.colima' / profile / 'docker.sock')
    return marker['owner'], profile, environment


def prepare(path):
    for name in ['container', 'container/config', 'container/qq', 'container/plugins']:
        target = path / name
        check(not target.is_symlink(), 'symlink_component_directory')
        target.mkdir(mode=0o700, exist_ok=True)
        check(target.is_dir() and target.stat().st_uid == os.getuid()
              and target.stat().st_mode & 0o777 == 0o700, 'unsafe_component_directory')
    for name in ['webui-password', 'onebot-token']:
        target = path / name
        if not target.exists() and not target.is_symlink():
            write(target, secrets.token_urlsafe(32) + '\n')
        read(target)
    config = path / 'container/config'
    if not (config / 'webui.json').exists():
        write(config / 'webui.json', json.dumps({'host': '0.0.0.0', 'port': 6099,
              'token': read(path / 'webui-password').strip(), 'loginRate': 3}))
    if not (config / 'onebot11.json').exists():
        write(config / 'onebot11.json', json.dumps({'network': {
            'httpServers': [], 'httpSseServers': [], 'httpClients': [],
            'websocketServers': [{'enable': True, 'name': 'kiarabot', 'host': '0.0.0.0',
                'port': 3001, 'messagePostFormat': 'array',
                'token': read(path / 'onebot-token').strip(), 'reportSelfMessage': False,
                'enableForcePushEvent': True, 'debug': False, 'heartInterval': 30000}],
            'websocketClients': [], 'plugins': []}, 'musicSignUrl': '',
            'enableLocalFile2Url': False, 'parseMultMsg': False}))
    read(config / 'webui.json')
    read(config / 'onebot11.json')


def container(path, environment):
    owner = json.loads(read(path / 'owner.json'))['owner']
    name = 'kiarabot-napcat-' + owner[:12]
    result = subprocess.run(['docker', 'container', 'inspect', name], env=environment,
                            capture_output=True, text=True, timeout=15)
    if result.returncode != 0:
        command(['docker', 'info', '--format', '{{.ServerVersion}}'], environment)
        return name, None
    record = json.loads(result.stdout)[0]
    check(record['Config'].get('Labels', {}).get(OWNER_LABEL) == owner,
          'foreign_container_refusing_takeover')
    ports = record['HostConfig'].get('PortBindings', {})
    check(all(ports.get(str(port) + '/tcp') ==
              [{'HostIp': '127.0.0.1', 'HostPort': str(port)}] for port in [6099, 3001]),
          'unexpected_container_port_exposure')
    return name, record


def webui_ready():
    try:
        with urllib.request.urlopen('http://127.0.0.1:6099/webui/', timeout=2) as response:
            return response.status == 200
    except (OSError, urllib.error.URLError):
        return False


def login_info(path):
    # Keep optional platform libraries in the locked Adapter interpreter.
    probe = '''import json,sys
from pathlib import Path
from websockets.sync.client import connect
try:
    token=Path(sys.argv[1]).read_text().strip()
    with connect("ws://127.0.0.1:3001",additional_headers={"Authorization":"Bearer "+token},open_timeout=3) as ws:
        def call(action):
            echo="kiarabot-read-only-"+action
            ws.send(json.dumps({"action":action,"params":{},"echo":echo}))
            for _ in range(16):
                data=json.loads(ws.recv(timeout=3))
                if data.get("echo")==echo:
                    assert data.get("status")=="ok" and data.get("retcode")==0
                    return data["data"]
            raise ValueError("response_not_received")
        account=str(call("get_login_info")["user_id"])
        assert account.isascii() and account.isdecimal() and int(account)>0
        online=call("get_status").get("online")
        print(json.dumps({"account":account,"online":online if isinstance(online,bool) else None}));sys.exit(0)
except Exception:
    sys.exit(1)
sys.exit(1)
'''
    read(path / 'onebot-token')
    result = subprocess.run([str(ROOT / 'adapters/nonebot/.venv/bin/python'), '-c', probe,
                             str(path / 'onebot-token')], capture_output=True, text=True, timeout=10)
    return json.loads(result.stdout) if result.returncode == 0 else None


def setup(path, install_tools):
    owner, profile, environment = settings(path)
    required = ['docker'] + (['colima'] if sys.platform == 'darwin' else [])
    if any(not shutil.which(name) for name in required):
        check(install_tools and sys.platform == 'darwin' and shutil.which('brew'),
              'container_tools_missing_use_install_tools_or_install_docker')
        print('operation=install_container_tools', flush=True)
        command(['brew', 'install', 'colima', 'docker'], timeout=1800)
    if sys.platform == 'darwin':
        print('operation=start_owned_vm', flush=True)
        command(['colima', 'start', profile, '--runtime', 'docker', '--vm-type', 'vz',
                 '--cpus', '2', '--memory', '4', '--disk', '20', '--activate=false',
                 '--ssh-agent=false', '--ssh-config=false',
                 '--mount', str(path / 'container') + ':w'], timeout=1800)
    image_lock = path / 'image-lock.json'
    if not image_lock.exists():
        print('operation=download_napcat_image', flush=True)
        command(['docker', 'pull', IMAGE], environment, timeout=1800)
        metadata = json.loads(command(['docker', 'image', 'inspect', IMAGE], environment))[0]
        digests = [item for item in metadata['RepoDigests']
                   if re.fullmatch(r'mlikiowa/napcat-docker@sha256:[a-f0-9]{64}', item)]
        check(len(digests) == 1, 'image_digest_missing_or_ambiguous')
        write(image_lock, json.dumps({'image': digests[0]}))
    image = json.loads(read(image_lock))['image']
    check(re.fullmatch(r'mlikiowa/napcat-docker@sha256:[a-f0-9]{64}', image), 'invalid_image_lock')
    name, record = container(path, environment)
    if record is None:
        for port in [6099, 3001]:
            with socket.socket() as probe:
                try:
                    probe.bind(('127.0.0.1', port))
                except OSError:
                    raise Failure('local_port_in_use_' + str(port)) from None
        args = ['docker', 'run', '-d', '--name', name, '--hostname', 'kiarabot-qq',
                '--label', OWNER_LABEL + '=' + owner, '--restart', 'unless-stopped',
                '--publish', '127.0.0.1:6099:6099', '--publish', '127.0.0.1:3001:3001',
                '--env', 'MODE=ws', '--env', 'NAPCAT_UID=' + str(os.getuid()),
                '--env', 'NAPCAT_GID=' + str(os.getgid()), '--ulimit', 'core=0',
                '--log-driver', 'local', '--log-opt', 'max-size=5m', '--log-opt', 'max-file=2']
        for local, remote in [('config', '/app/napcat/config'), ('qq', '/app/.config/QQ'),
                              ('plugins', '/app/napcat/plugins')]:
            args += ['--mount', 'type=bind,src=' + str(path / 'container' / local) + ',dst=' + remote]
        command(args + [image], environment, timeout=120)
    elif not record['State']['Running']:
        command(['docker', 'start', name], environment)
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        _, record = container(path, environment)
        check(record and record['State']['Running'], 'napcat_process_exited')
        if webui_ready():
            return
        time.sleep(1)
    raise Failure('napcat_webui_timeout')


def operate(path, args):
    prepare(path)
    _, _, environment = settings(path)
    if args.command == 'setup':
        setup(path, args.install_tools)
        print(json.dumps({'state': 'awaiting_user_login', 'webui_url': 'http://127.0.0.1:6099/webui/',
                          'password_file': str(path / 'webui-password')}))
        return 0
    name, record = container(path, environment)
    running = bool(record and record['State']['Running'])
    if args.command == 'stop':
        if running:
            command(['docker', 'stop', '--time', '20', name], environment, timeout=30)
        print(json.dumps({'napcat_running': False, 'data_preserved': True}))
        return 0
    info = login_info(path) if running else None
    if args.command == 'status':
        print(json.dumps({'napcat_running': running, 'webui_available': running and webui_ready(),
                          'onebot_authenticated': info is not None,
                          'qq_online': info.get('online') if info else None,
                          'account': info['account'] if info else None}))
        return 0 if info and info.get('online') is True else 1
    check(args.conversation and re.fullmatch('[1-9][0-9]{0,19}', args.conversation),
          'explicit_test_conversation_required')
    check(info is not None and info.get('online') is True,
          'qq_login_or_authenticated_onebot_not_ready')
    cli = [sys.executable, str(ROOT / 'scripts/local/start.py'), 'start',
           '--state-dir', str(ROOT / 'data/real-qq'), '--account', info['account'],
           '--conversation', args.conversation, '--onebot-url', 'ws://127.0.0.1:3001',
           '--onebot-token-file', str(path / 'onebot-token'),
           '--core', str(ROOT / 'build/full-locked/reference/core/kiarabot-core'),
           '--credentials', str(ROOT / 'build/full-locked/reference/control/api/kiarabot-control-credentials'),
           '--python', str(ROOT / 'adapters/nonebot/.venv/bin/python')]
    if args.ui:
        cli += ['--ui', '--ui-port', str(args.ui_port)]
    return subprocess.run(cli, check=False).returncode


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['setup', 'status', 'stop', 'connect'])
    parser.add_argument('--state-dir', default=str(ROOT / 'data/napcat-qq'))
    parser.add_argument('--install-tools', action='store_true')
    parser.add_argument('--conversation')
    parser.add_argument('--ui', action='store_true')
    parser.add_argument('--ui-port', type=int, choices=range(1, 65536), default=5173, metavar='PORT')
    args = parser.parse_args()
    path = state_directory(args.state_dir)
    descriptor = lock(path)
    try:
        return operate(path, args)
    finally:
        os.close(descriptor)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Failure as error:
        print('operation=napcat_driver outcome=failed reason=' + str(error), file=sys.stderr)
        sys.exit(1)
    except Exception:
        print('operation=napcat_driver outcome=failed reason=local_dependency_or_configuration_error',
              file=sys.stderr)
        sys.exit(1)
