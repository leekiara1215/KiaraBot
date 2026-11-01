#!/usr/bin/env python3
"""Four real daemons against the owned P03 TLS cluster; no external DSN accepted."""
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import select
import signal
import subprocess
import sys

from service_config_test import certificates


def require(condition, stage):
    if not condition:
        raise RuntimeError('service database assertion: ' + stage)


def main(root_name, port_text, initializer_name, unused_proxy_port):
    root = Path(root_name)
    require(not root.is_symlink() and root.resolve() == root, 'canonical fixture root')
    require(root.name.startswith('kiarabot-p03-') and root.stat().st_uid == os.getuid(), 'owned root')
    require((root / 'p03-owned-marker').is_file(), 'fixture ownership marker')
    pg_bin = Path(os.environ['KIARABOT_TEST_PG_BIN']).resolve()
    binaries = Path(os.environ['KIARABOT_SERVICE_BIN_DIR']).resolve()
    port = int(port_text)
    require(0 < port < 65536, 'local fixture port')
    password = (root / 'password').read_text().strip()
    require(re.fullmatch('[a-f0-9]{64}', password) is not None, 'generated fixture password')
    environment = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
    role_passwords = {'p03_admin': password}

    def sql(statement, user='p03_admin', database='postgres'):
        connection_environment = {**environment, 'PGPASSWORD': role_passwords[user],
                                  'PGSSLMODE': 'verify-full', 'PGCHANNELBINDING': 'require',
                                  'PGSSLROOTCERT': str(root / 'server.pem'), 'PGGSSENCMODE': 'disable'}
        result = subprocess.run([str(pg_bin / 'psql'), '-X', '-qAt', '-v', 'ON_ERROR_STOP=1',
                                 '-h', 'localhost', '-p', str(port), '-U', user, '-d', database,
                                 '-f', '-'], input=statement, env=connection_environment,
                                capture_output=True, text=True, timeout=10)
        require(result.returncode == 0, 'private fixture SQL (output redacted)')
        return result.stdout.strip()

    require(Path(sql('SHOW data_directory;')).resolve() == root / 'data', 'actual private cluster')
    platform_root = root / 'platform'
    platform_root.mkdir()
    base = certificates(platform_root, 'ec')
    names = ['identity', 'runtime', 'scheduler', 'model']
    configurations = {}
    paths = {}
    for name in names:
        database = 'p03_service_' + name
        migration, runtime = database + '_migration', database + '_runtime'
        for role in [migration, runtime]:
            role_passwords[role] = secrets.token_hex(32)
            secret_file = root / (role + '.password')
            secret_file.write_text(role_passwords[role] + '\n')
            secret_file.chmod(0o600)
        # All identifiers are fixed test names; the only literal is validated random hex.
        sql(f"CREATE ROLE {migration} LOGIN PASSWORD '{role_passwords[migration]}';\n"
            f"CREATE ROLE {runtime} LOGIN PASSWORD '{role_passwords[runtime]}';\n"
            f'CREATE DATABASE {database} OWNER {migration};\n'
            f'REVOKE ALL ON DATABASE {database} FROM PUBLIC;\n'
            f'GRANT CONNECT ON DATABASE {database} TO {runtime};\n')
        initialize = [initializer_name, '--initialize-empty', name, 'localhost',
                      str(port), database, migration, runtime, str(root / 'server.pem')]
        wrong_identity = subprocess.run([*initialize, str(root / (runtime + '.password'))],
                                        env=environment, capture_output=True, text=True, timeout=10)
        require(wrong_identity.returncode == 1, 'runtime secret cannot authenticate migration user')
        initialized = subprocess.run([*initialize, str(root / (migration + '.password'))],
                                     env=environment, capture_output=True,
                                     text=True, timeout=10)
        require(initialized.returncode == 0, name + ' explicit initialization')
        configurations[name] = {**base, 'service': name,
                                'database': {'host': 'localhost', 'port': port, 'name': database,
                                             'migration_role': migration, 'runtime_role': runtime,
                                             'password_file': str(root / (runtime + '.password')),
                                             'ca_file': str(root / 'server.pem')}}
        paths[name] = root / (name + '.json')
        paths[name].write_text(json.dumps(configurations[name]))

    children = {}

    def start(name):
        child = subprocess.Popen([str(binaries / ('kiarabot-' + name)), '--config', str(paths[name])],
                                 env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 text=True)
        children[name] = child
        require(bool(select.select([child.stdout], [], [], 10)[0]), name + ' bounded startup')
        line = child.stdout.readline()
        state = dict(item.split('=', 1) for item in line.strip().split())
        require(state.get('service') == name and state.get('ready') == 'false', name + ' startup')
        return state

    def probe(state, path, code):
        connection = http.client.HTTPConnection('127.0.0.1', int(state['probe_port']), timeout=2)
        try:
            connection.request('GET', path)
            response = connection.getresponse()
            require(response.status == code, path)
            expected = 'HEALTHY' if code == 200 else 'UNHEALTHY'
            require(json.loads(response.read()) == {'state': expected}, 'probe meaning')
        finally:
            connection.close()

    def finish(name, code, terminate=False):
        child = children[name]
        if terminate and child.poll() is None:
            child.send_signal(signal.SIGTERM)
        stdout, stderr = child.communicate(timeout=7)
        require(child.returncode == code, name + ' bounded shutdown')
        require(all(secret not in stdout + stderr for secret in role_passwords.values()) and
                'PRIVATE KEY' not in stdout + stderr, 'no secrets')
        if code == 1:
            require('reason=dependency_lost' in stderr, 'lost database stops service')

    def rejected(name, config):
        path = root / 'rejected.json'
        path.write_text(json.dumps(config))
        result = subprocess.run([str(binaries / ('kiarabot-' + name)), '--config', str(path)],
                                env=environment, capture_output=True, text=True, timeout=10)
        require(result.returncode == 1 and 'probe_port=' not in result.stdout,
                name + ' must reject dependency before listening')
        require(all(secret not in result.stdout + result.stderr for secret in role_passwords.values()) and
                'PRIVATE KEY' not in result.stdout + result.stderr, 'rejection redacted')

    try:
        states = {name: start(name) for name in names}
        for name in names:
            probe(states[name], '/health/live', 200)
            probe(states[name], '/health/ready', 503)  # PG success is not implemented business.
            rejected(name, configurations[name])  # Actual competing writer, not a port collision.
        runtime = configurations['runtime']
        # Remove the legitimate writer so its lock cannot mask broken TLS/password rejection.
        finish('runtime', 0, terminate=True)
        rejected('runtime', {**runtime, 'database': {**runtime['database'],
                                                    'name': configurations['identity']['database']['name']}})
        rejected('runtime', {**runtime, 'database': {**runtime['database'],
                                                    'ca_file': str(platform_root / 'ca.pem')}})
        wrong_password = root / 'wrong-password'
        wrong_password.write_text('wrong-credential\n')
        wrong_password.chmod(0o600)
        rejected('runtime', {**runtime, 'database': {**runtime['database'],
                                                    'password_file': str(wrong_password)}})
        states['runtime'] = start('runtime')

        db = runtime['database']['name']
        role = runtime['database']['runtime_role']
        terminated = sql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                         f"WHERE datname='{db}' AND usename='{role}' AND application_name='kiarabot';")
        require(terminated.splitlines() == ['t'], 'exactly one runtime writer session')
        finish('runtime', 1)
        for name in ['identity', 'scheduler', 'model']:
            require(children[name].poll() is None, 'private database failure isolation')
            probe(states[name], '/health/live', 200)
        states['runtime'] = start('runtime')
        probe(states['runtime'], '/health/live', 200)
        finish('runtime', 0, terminate=True)
        migration = runtime['database']['migration_role']
        version = int(sql('SELECT version FROM kiarabot.schema_versions;', migration, db))
        sql('UPDATE kiarabot.schema_versions SET version=version+1;', migration, db)
        rejected('runtime', runtime)
        sql(f'UPDATE kiarabot.schema_versions SET version={version};', migration, db)
        states['runtime'] = start('runtime')
        for name in names:
            finish(name, 0, terminate=True)
        print('Four PG-backed daemons: startup, writer lock, rejection, isolation, loss and restart passed')
    finally:
        for child in children.values():
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=5)


if __name__ == '__main__':
    require(len(sys.argv) == 5, 'owned fixture arguments')
    main(*sys.argv[1:])
