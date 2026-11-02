#!/usr/bin/env python3
"""P08e real managed-host e2e: PG16.4 + Runtime + kiarabot-plugin-host + SDK plugin child.

The test drives the frozen managed-host path with real processes: a disposable PG16.4
cluster, a real mTLS Runtime, the real kiarabot-plugin-host binary and the shipped Python
SDK runner as the managed child. It asserts Runtime host_commands/host_observations and the
Host SQLite journal, plugin registration, stop/drain, Host restart and credential hygiene.

Requires the built host/seed binaries and generated Python protos (ctest supplies them).
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import secrets
import select
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.request

ADAPTER_INSTANCE = '33333333-3333-4333-8333-333333333333'
ACCOUNT = '11111111-1111-4111-8111-111111111111'
PLUGIN_INSTANCE = '44444444-4444-4444-8444-444444444444'
INSTALLATION = '55555555-5555-4555-8555-555555555555'
ADAPTER_BOOTSTRAP = 'a' * 64
PLUGIN_BOOTSTRAP = 'b' * 64
SHUTDOWN_INSTANCE = 'a1a1a1a1-a1a1-4a1a-8a1a-a1a1a1a1a1a1'
SHUTDOWN_BOOTSTRAP = 'c' * 64
HOST_ID = 'host-a'


class Failure(RuntimeError):
    pass


def require(condition, label):
    if not condition:
        raise Failure(label)


def load_fixture(repo: Path):
    sys.path.insert(0, str(repo / 'tests/architecture'))
    import p06_bridge_acceptance as fixture
    return fixture


def load_protos(repo: Path):
    for path in (repo / 'sdk/python/build/generated', repo / 'sdk/python/src'):
        require(path.is_dir(), f'missing proto path: {path}')
        sys.path.insert(0, str(path))
    from kiarabot.v1 import host_pb2, platform_pb2
    return host_pb2, platform_pb2


def free_port():
    import socket
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def openssl(openssl_path, *arguments, cwd):
    subprocess.run([openssl_path, *arguments], cwd=cwd, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def generate_host_cert(openssl_path, pki, directory: Path):
    """CA-signed Host leaf with both serverAuth and clientAuth (own TLS + HostBridge client)."""
    directory.mkdir(mode=0o700, exist_ok=True)
    openssl(openssl_path, 'req', '-new', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=host-a',
            '-keyout', 'host.key', '-out', 'host.csr', cwd=directory)
    (directory / 'host.ext').write_text(
        'basicConstraints=critical,CA:FALSE\n'
        'keyUsage=critical,digitalSignature\n'
        'extendedKeyUsage=clientAuth,serverAuth\n'
        'subjectAltName=DNS:localhost,IP:127.0.0.1\n')
    openssl(openssl_path, 'x509', '-req', '-in', 'host.csr', '-CA', str(pki.ca.certificate),
            '-CAkey', str(pki.ca.key), '-CAcreateserial', '-days', '1',
            '-extfile', 'host.ext', '-out', 'host.pem', cwd=directory)
    (directory / 'host.key').chmod(0o600)
    return directory / 'host.pem', directory / 'host.key'


def start_process(command, cwd, stderr_path, ready_line=None):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('KIARABOT_', 'CORE_', 'ONEBOT_', 'PG', 'PLUGIN_'))}
    error_file = stderr_path.open('wb')
    process = subprocess.Popen(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                               stderr=error_file, start_new_session=True)
    if ready_line is not None:
        if not select.select([process.stdout], [], [], 15)[0]:
            raise Failure('process did not report startup: ' + ' '.join(map(str, command)))
        line = process.stdout.readline().decode(errors='replace').strip()
        if ready_line not in line:
            raise Failure(f'process startup line missing {ready_line!r}: {line[:300]}')
    return process, error_file


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def wait_until(predicate, label, timeout=45.0, interval=0.05):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(interval)
    raise Failure(f'{label}: timed out after {timeout}s')


def sqlite_rows(path: Path, query: str):
    connection = sqlite3.connect(path)
    try:
        return connection.execute(query).fetchall()
    finally:
        connection.close()


def write_package(directory: Path, repo: Path):
    package = directory / 'package'
    package.mkdir(parents=True, exist_ok=True)
    applied = directory / 'applied.jsonl'
    started = directory / 'plugin.started'
    stopped = directory / 'plugin.stopped'
    (package / 'subject.py').write_text(
        'import json\n'
        'from pathlib import Path\n'
        'from kiarabot_sdk import Plugin\n'
        'class Subject(Plugin):\n'
        '    async def on_start(self, context):\n'
        f'        Path({str(started)!r}).write_text("started")\n'
        '    async def on_stop(self, reason):\n'
        f'        Path({str(stopped)!r}).write_text(str(reason))\n'
        '    async def on_config(self, config, context):\n'
        f'        Path({str(applied)!r}).open("a").write(json.dumps(dict(\n'
        '            version=config.version, config_version=config.config_version,\n'
        '            delivery_id=config.delivery_id, values=dict(config.values))) + "\\n")\n'
        'def create():\n'
        '    return Subject()\n')
    manifest = json.loads((repo / 'plugins/event-observer/plugin.json').read_text())
    manifest['entrypoint'] = 'subject:create'
    manifest_path = package / 'plugin.json'
    manifest_path.write_text(json.dumps(manifest))
    return package, manifest_path, started, stopped


def host_config(directory: Path, repo: Path, runtime_endpoint: str, bridge_endpoint: str,
                package: Path, manifest: Path, ca: Path, host_cert: Path, host_key: Path):
    credentials = directory / 'credentials'
    credentials.mkdir(mode=0o700, exist_ok=True)
    # P08d deployment helper writes the one-time credential file (0600, no argv/stdout leak).
    response = json.dumps({'instance': {'instance_id': PLUGIN_INSTANCE},
                           'bootstrap_credential': PLUGIN_BOOTSTRAP})
    helper = repo / 'scripts/deploy/target_host_credential.py'
    result = subprocess.run([sys.executable, str(helper), 'store', '--directory', str(credentials)],
                            input=response, text=True, capture_output=True, check=False)
    require(result.returncode == 0, f'credential helper failed: {result.stderr[:500]}')
    require(PLUGIN_BOOTSTRAP not in result.stdout + result.stderr,
            'credential helper leaked plaintext')
    environment_names = ['PYTHONPATH', 'PLUGIN_CORE_ENDPOINT', 'PLUGIN_TLS_ROOT',
                         'PLUGIN_INSTANCE_ID', 'PLUGIN_IO_TIMEOUT', 'PLUGIN_HEARTBEAT_INTERVAL',
                         'PLUGIN_RETRY_DELAY', 'KIARABOT_PLUGIN_BOOTSTRAP']
    environment = {
        'PYTHONPATH': os.pathsep.join([str(repo / 'sdk/python/src'),
                                       str(repo / 'sdk/python/build/generated'),
                                       str(package)]),
        'PLUGIN_CORE_ENDPOINT': bridge_endpoint,
        'PLUGIN_TLS_ROOT': str(ca),
        'PLUGIN_INSTANCE_ID': PLUGIN_INSTANCE,
        'PLUGIN_IO_TIMEOUT': '2', 'PLUGIN_HEARTBEAT_INTERVAL': '0.2',
        'PLUGIN_RETRY_DELAY': '0.1',
    }
    runner = directory / 'run_plugin.sh'
    runner.write_text(
        '#!/bin/sh\n'
        f'exec "{sys.executable}" -m kiarabot_sdk "$@" >>"{directory / "plugin.log"}" 2>&1\n')
    runner.chmod(0o700)
    config = {
        'schema_version': 1, 'service': 'plugin-host',
        'probe_listen': f'127.0.0.1:{free_port()}',
        'grpc_listen': f'127.0.0.1:{free_port()}',
        'tls': {'certificate_file': str(host_cert), 'private_key_file': str(host_key),
                'ca_file': str(ca)},
        'host': {
            'host_id': HOST_ID, 'sqlite_path': str(directory / 'host.sqlite'),
            'runtime_endpoint': runtime_endpoint,
            'credential_directory': str(credentials),
            'packages': [{'plugin_type_id': 'event-observer', 'root': str(package),
                          'sha256': 'a' * 64, 'launch_profile': 'python',
                          'allowed_argument_names': ['--manifest']}],
            'profiles': [{'name': 'python', 'executable': str(runner),
                          'fixed_arguments': [str(runner)],
                          'allowed_environment_names': environment_names,
                          'credential_environment': 'KIARABOT_PLUGIN_BOOTSTRAP'}],
            'environment': environment,
        },
    }
    path = directory / 'host.json'
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    return path, credentials


async def main_async(args) -> None:
    repo = Path(args.repo_source).resolve()
    fixture = load_fixture(repo)
    host_pb2, platform = load_protos(repo)
    from fixtures import TestPki

    root = Path(tempfile.mkdtemp(prefix='kiarabot-p08e-')).resolve()
    root.chmod(0o700)
    runtime = host = None
    runtime_err = host_err = None
    try:
        port = fixture.start_postgres(Path(args.pg_bin).resolve(), root,
                                      Path(args.openssl).resolve(), None)
        admin = (root / 'password').read_text().strip()
        env = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
        env['PGPASSWORD'] = admin
        env['LC_ALL'] = 'C'
        admin_dsn = (f'host=localhost port={port} dbname=postgres user=p03_admin '
                     f'sslmode=verify-full sslrootcert={root / "server.pem"}')
        runtime_password = (root / 'password-p03_runtime_runtime').read_text().strip()
        migration_password = (root / 'password-p03_runtime_migration').read_text().strip()
        psql_admin = [Path(args.pg_bin) / 'psql', admin_dsn, '-tA', '-v',
                      'ON_ERROR_STOP=1', '-c']
        fixture.run([*psql_admin,
                     f"CREATE ROLE p03_runtime_migration LOGIN NOINHERIT NOSUPERUSER "
                     f"NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD "
                     f"{fixture.quote_sql(migration_password)}"], env=env)
        fixture.run([*psql_admin,
                     f"CREATE ROLE p03_runtime_runtime LOGIN NOINHERIT NOSUPERUSER "
                     f"NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD "
                     f"{fixture.quote_sql(runtime_password)}"], env=env)
        fixture.run([*psql_admin, 'CREATE DATABASE p03_runtime OWNER p03_runtime_migration'],
                    env=env)
        fixture.run([*psql_admin, 'REVOKE ALL ON DATABASE p03_runtime FROM PUBLIC'], env=env)
        fixture.run([*psql_admin,
                     'GRANT CONNECT ON DATABASE p03_runtime TO p03_runtime_runtime'], env=env)
        fixture.run([args.initializer, '--initialize-empty', 'runtime', 'localhost', str(port),
                     'p03_runtime', 'p03_runtime_migration', 'p03_runtime_runtime',
                     str(root / 'server.pem'), str(root / 'password-p03_runtime_migration')],
                    env=env)
        seeded = fixture.run([args.seed, str(root), str(port), ADAPTER_INSTANCE, ACCOUNT,
                              PLUGIN_INSTANCE, ADAPTER_BOOTSTRAP, PLUGIN_BOOTSTRAP, INSTALLATION,
                              SHUTDOWN_INSTANCE, SHUTDOWN_BOOTSTRAP], env=env)
        require(b'SEEDED' in seeded.stdout, 'P06 seed driver failed')

        with TestPki(Path(args.openssl).resolve()) as pki:
            server_leaf = pki.servers['runtime']
            host_cert, host_key = generate_host_cert(
                Path(args.openssl), pki, pki.root / 'host')
            host_der = subprocess.check_output(
                [args.openssl, 'x509', '-in', str(host_cert), '-outform', 'der'])
            fingerprint = hashlib.sha256(host_der).hexdigest()
            probe_port, grpc_port, bridge_port = free_port(), free_port(), free_port()
            runtime_config = {
                'schema_version': 1, 'service': 'runtime',
                'probe_listen': f'127.0.0.1:{probe_port}',
                'grpc_listen': f'127.0.0.1:{grpc_port}',
                'bridge_listen': f'127.0.0.1:{bridge_port}',
                'tls': {'certificate_file': str(server_leaf.certificate),
                        'private_key_file': str(server_leaf.key),
                        'ca_file': str(pki.ca.certificate)},
                'database': {'host': 'localhost', 'port': port, 'name': 'p03_runtime',
                             'migration_role': 'p03_runtime_migration',
                             'runtime_role': 'p03_runtime_runtime',
                             'password_file': str(root / 'password-p03_runtime_runtime'),
                             'ca_file': str(root / 'server.pem')},
                'hosts': [{'host_id': HOST_ID, 'certificate_sha256': fingerprint}],
            }
            runtime_config_path = root / 'runtime.json'
            runtime_config_path.write_text(json.dumps(runtime_config))
            runtime_config_path.chmod(0o600)
            runtime, runtime_err = start_process(
                [args.runtime, '--config', str(runtime_config_path)], root,
                root / 'runtime.err', 'grpc_port=')
            # start_process consumes the one startup line; the configured port is known.
            require(grpc_port > 0, 'runtime grpc port')

            package, manifest, plugin_started, plugin_stopped = write_package(root, repo)
            host_config_path, credentials = host_config(
                root, repo, f'127.0.0.1:{grpc_port}', f'127.0.0.1:{bridge_port}',
                package, manifest, pki.ca.certificate, host_cert, host_key)
            now_sql = '(extract(epoch from clock_timestamp())*1000000000)::bigint'
            args_sql = "'--manifest=" + str(manifest).replace("'", "''") + "'"
            env_sql = ("ARRAY['PYTHONPATH','PLUGIN_CORE_ENDPOINT','PLUGIN_TLS_ROOT',"
                       "'PLUGIN_INSTANCE_ID','PLUGIN_IO_TIMEOUT','PLUGIN_HEARTBEAT_INTERVAL',"
                       "'PLUGIN_RETRY_DELAY','KIARABOT_PLUGIN_BOOTSTRAP']")
            fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                         "INSERT INTO kiarabot.host_commands("
                         "command_id,host_id,instance_id,installation_id,kind,"
                         "execution_generation,desired_version,desired_state,package_sha256,"
                         "launch_profile,arguments,environment_names,payload_sha256,state,"
                         "created_at,updated_at) "
                         "VALUES('hc-p08e-start','host-a','" + PLUGIN_INSTANCE + "','" +
                         INSTALLATION + "',1,1,1,0,repeat('a',64),'python',"
                         f"ARRAY[{args_sql}],{env_sql},repeat('b',64),0,{now_sql},{now_sql})")
            host, host_err = start_process(
                [args.host_binary, '--config', str(host_config_path)], root,
                root / 'host.err', None)

            def command_state():
                return fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                                    "SELECT state,receipt_persisted FROM kiarabot.host_commands "
                                    "WHERE command_id='hc-p08e-start'").stdout.decode().strip()

            wait_until(lambda: command_state().startswith('2|t'), 'start command receipt')
            def observation_count():
                return fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                                    "SELECT count(*) FROM kiarabot.host_observations "
                                    f"WHERE host_id='host-a' AND instance_id='{PLUGIN_INSTANCE}'"
                                    ).stdout.decode().strip()
            wait_until(lambda: observation_count() != '0', 'host observation')
            wait_until(lambda: plugin_started.is_file(), 'SDK plugin start/registration')
            require(plugin_started.read_text() == 'started',
                    'SDK plugin start marker content')
            journal = root / 'host.sqlite'
            require(journal.is_file(), 'Host SQLite journal missing')
            executions = sqlite_rows(journal, 'SELECT pid,state,outcome FROM managed_executions')
            require(executions, 'Host journal has no managed execution')
            pid = int(executions[0][0])
            require(pid > 0, 'managed plugin pid missing')
            os.kill(pid, 0)
            require((credentials / f'{PLUGIN_INSTANCE}.bootstrap').stat().st_mode & 0o777 == 0o600,
                    'credential file mode')
            log_text = (root / 'host.err').read_text(errors='replace')
            for stream in (root / 'runtime.err', root / 'plugin.log'):
                log_text += stream.read_text(errors='replace')
            require(PLUGIN_BOOTSTRAP not in log_text, 'bootstrap credential leaked to logs')

            # Stop command: Host must terminate the real child and report a terminal outcome.
            fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                         "INSERT INTO kiarabot.host_commands("
                         "command_id,host_id,instance_id,installation_id,kind,"
                         "execution_generation,desired_version,desired_state,package_sha256,"
                         "launch_profile,payload_sha256,state,created_at,updated_at) "
                         "VALUES('hc-p08e-stop','host-a','" + PLUGIN_INSTANCE + "','" +
                         INSTALLATION + "',2,1,1,1,repeat('a',64),'python',"
                         f"repeat('c',64),0,{now_sql},{now_sql})")
            wait_until(lambda: fixture.psql(
                Path(args.pg_bin), root, port, runtime_password,
                "SELECT state FROM kiarabot.host_commands WHERE "
                "command_id='hc-p08e-stop'").stdout.decode().strip() == '2', 'stop receipt')
            def child_gone():
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    return True
                return False
            try:
                wait_until(child_gone, 'plugin child exit')
                wait_until(lambda: plugin_stopped.is_file(), 'SDK plugin on_stop hook')
            except BaseException:
                for table in ('command_journal', 'managed_executions', 'execution_observations'):
                    try:
                        rows = sqlite_rows(journal, f'SELECT * FROM {table}')
                    except Exception as query_error:  # noqa: BLE001
                        rows = f'query failed: {query_error}'
                    print(f'P08e debug {table}: {rows}', file=sys.stderr)
                print('P08e debug commands: ' + fixture.psql(
                    Path(args.pg_bin), root, port, runtime_password,
                    'SELECT command_id,kind,state,connection_generation,receipt_persisted '
                    'FROM kiarabot.host_commands ORDER BY command_id'
                ).stdout.decode(errors='replace'), file=sys.stderr)
                print('P08e debug observations: ' + fixture.psql(
                    Path(args.pg_bin), root, port, runtime_password,
                    'SELECT connection_generation,sequence,instance_id,execution_generation,outcome '
                    'FROM kiarabot.host_observations ORDER BY sequence'
                ).stdout.decode(errors='replace'), file=sys.stderr)
                raise
            outcomes = sqlite_rows(journal, 'SELECT outcome FROM managed_executions')
            require(outcomes and outcomes[0][0] != 0, 'managed execution outcome not updated')
            stop_process(host)
            host_err.close()
            host = host_err = None

            # Host restart: journal survives, command receipts are not replayed/spawned again.
            host, host_err = start_process(
                [args.host_binary, '--config', str(host_config_path)], root,
                root / 'host-restart.err', None)
            time.sleep(2.0)
            executions_after = sqlite_rows(journal, 'SELECT count(*) FROM managed_executions')
            require(executions_after[0][0] == len(executions),
                    'Host restart spawned a duplicate execution')
            require(command_state().startswith('2|t'), 'restart lost receipt state')
            # Drain command reaches the real Host after restart and is journaled/receipted.
            fixture.psql(Path(args.pg_bin), root, port, runtime_password,
                         "INSERT INTO kiarabot.host_commands("
                         "command_id,host_id,instance_id,installation_id,kind,"
                         "execution_generation,desired_version,desired_state,package_sha256,"
                         "launch_profile,payload_sha256,state,created_at,updated_at) "
                         "VALUES('hc-p08e-drain','host-a','" + PLUGIN_INSTANCE + "','" +
                         INSTALLATION + "',3,1,1,1,repeat('a',64),'python',"
                         f"repeat('d',64),0,{now_sql},{now_sql})")
            wait_until(lambda: fixture.psql(
                Path(args.pg_bin), root, port, runtime_password,
                "SELECT state FROM kiarabot.host_commands WHERE "
                "command_id='hc-p08e-drain'").stdout.decode().strip() == '2', 'drain receipt')
            drain_rows = sqlite_rows(
                journal, "SELECT state FROM command_journal WHERE command_id='hc-p08e-drain'")
            require(drain_rows and drain_rows[0][0] == 2, 'Host drain journal state')
            print('P08 managed host e2e: PASS')
    except BaseException:
        for path in (root / 'runtime.err', root / 'host.err', root / 'host-restart.err',
                     root / 'plugin.log'):
            if path.is_file():
                print(f'P08e {path.name}:\n' + path.read_text(errors='replace')[-4000:],
                      file=sys.stderr)
        raise
    finally:
        stop_process(host)
        stop_process(runtime)
        for stream in (host_err, runtime_err):
            if stream is not None:
                stream.close()
        fixture = load_fixture(Path(args.repo_source).resolve())
        with __import__('contextlib').suppress(Exception):
            fixture.run([Path(args.pg_bin) / 'pg_ctl', '-D', str(root / 'data'), '-m',
                         'immediate', '-w', '-t', '20', 'stop'], timeout=30,
                        env={key: value for key, value in os.environ.items()
                             if not key.startswith('PG')})
        import shutil
        shutil.rmtree(root, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', required=True)
    parser.add_argument('--host-binary', required=True)
    parser.add_argument('--seed', required=True)
    parser.add_argument('--initializer', required=True)
    parser.add_argument('--openssl', required=True)
    parser.add_argument('--pg-bin', required=True)
    parser.add_argument('--repo-source', required=True)
    args = parser.parse_args()
    try:
        asyncio.run(main_async(args))
        return 0
    except Exception as error:  # noqa: BLE001
        print(f'P08e managed host e2e FAILED: {type(error).__name__}: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
