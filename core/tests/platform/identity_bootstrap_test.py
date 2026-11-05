#!/usr/bin/env python3
"""Real offline bootstrap CLI in the owned PG fixture; never accepts an external DSN."""
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import traceback
import uuid

from service_config_test import certificates


# Filled only when the optional v1.1 Host enrollment is exercised. Test fixtures only; never a
# credential, key or reusable proof.
HOST_STATE = {}


def main(*, control_server_certificate=False, after_bootstrap=None,
         verification_catalog=None, prepare=None, host_enrollment=False,
         host_permissions=None):
    root = Path(sys.argv[1]).resolve()
    port, initializer = sys.argv[2:4]
    bootstrap = str(Path(sys.argv[5]).resolve())
    pg_bin = Path(sys.argv[6]).resolve()
    assert root.name.startswith('kiarabot-p03-') and root.stat().st_uid == os.getuid()
    assert (root / 'p03-owned-marker').is_file() and not root.is_symlink()
    environment = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
    environment.update(PGHOST='localhost', PGPORT=port, PGUSER='p03_admin', PGDATABASE='postgres',
                       PGPASSWORD=(root / 'password').read_text().strip(), PGSSLMODE='verify-full',
                       PGSSLROOTCERT=str(root / 'server.pem'), PGCHANNELBINDING='require',
                       PGCONNECT_TIMEOUT='2')

    # Bootstrap drops ambient PG overrides before invoking the production adapter; keep a private
    # query environment so post-commit assertions still reach the owned cluster explicitly.
    query_environment = {
        'PGHOST': 'localhost', 'PGPORT': str(port), 'PGUSER': 'p03_admin',
        'PGDATABASE': 'postgres', 'PGPASSWORD': (root / 'password').read_text().strip(),
        'PGSSLMODE': 'verify-full', 'PGSSLROOTCERT': str(root / 'server.pem'),
        'PGCHANNELBINDING': 'require', 'PGCONNECT_TIMEOUT': '2'}

    def sql(statement, database='postgres'):
        environment_for_query = dict(query_environment)
        environment_for_query['PGDATABASE'] = database
        reply = subprocess.run([str(pg_bin / 'psql'), '-X', '-qAt', '-v', 'ON_ERROR_STOP=1'],
                               input=statement, text=True, env=environment_for_query,
                               capture_output=True, timeout=8)
        assert reply.returncode == 0, 'owned fixture SQL failed; diagnostics redacted'
        return reply.stdout.strip()

    # Check the server's real identity before any role/database creation.
    assert Path(sql('SHOW data_directory;')).resolve() == root / 'data', 'fixture backend mismatch'
    migration = (root / 'password-p03_identity_migration').read_text().strip()
    runtime = (root / 'password-p03_identity_runtime').read_text().strip()
    assert all(re.fullmatch('[a-f0-9]{64}', value) for value in [migration, runtime])
    constraints = 'LOGIN NOINHERIT NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS'
    sql(f"CREATE ROLE p03_identity_migration {constraints} PASSWORD '{migration}';\n"
        f"CREATE ROLE p03_identity_runtime {constraints} PASSWORD '{runtime}';\n"
        'CREATE DATABASE p03_identity OWNER p03_identity_migration;\n'
        'REVOKE ALL ON DATABASE p03_identity FROM PUBLIC;\n'
        'GRANT CONNECT ON DATABASE p03_identity TO p03_identity_runtime;\n')
    tls_root = root / 'bootstrap-tls'
    tls_root.mkdir(mode=0o700)
    config = certificates(tls_root, 'ec')
    # Use an explicit URI SAN for enrollment; the server certificate remains separate.
    extensions = tls_root / 'control.ext'
    browser_names = ',DNS:localhost,IP:127.0.0.1' if control_server_certificate else ''
    purposes = 'clientAuth,serverAuth' if control_server_certificate else 'clientAuth'
    extensions.write_text(f'subjectAltName=URI:spiffe://kiarabot/control-test{browser_names}\n'
                          f'extendedKeyUsage={purposes}\nbasicConstraints=critical,CA:FALSE\n'
                          'keyUsage=critical,digitalSignature\nsubjectKeyIdentifier=hash\n'
                          'authorityKeyIdentifier=keyid:always\n')
    for arguments in [
        ['req', '-x509', '-key', 'ca.key', '-days', '1', '-subj', '/CN=P02-Test-CA',
         '-out', 'ca.pem', '-addext', 'basicConstraints=critical,CA:TRUE',
         '-addext', 'keyUsage=critical,keyCertSign,cRLSign',
         '-addext', 'subjectKeyIdentifier=hash'],
        ['req', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes',
         '-subj', '/CN=Control', '-keyout', 'control.key', '-out', 'control.csr'],
        ['x509', '-req', '-in', 'control.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key',
         '-CAcreateserial', '-days', '1', '-extfile', 'control.ext', '-out', 'control.pem'],
    ]:
        reply = subprocess.run(['openssl', *arguments], cwd=tls_root, capture_output=True, timeout=5)
        assert reply.returncode == 0, 'fixture certificate generation failed'
    # Runtime is both the Runtime gRPC server and the outbound mTLS Identity consumer.
    (tls_root / 'runtime.ext').write_text(
        'subjectAltName=URI:spiffe://kiarabot/runtime-test,DNS:localhost,IP:127.0.0.1\n'
        'extendedKeyUsage=clientAuth,serverAuth\nbasicConstraints=critical,CA:FALSE\n'
        'keyUsage=critical,digitalSignature\nsubjectKeyIdentifier=hash\n'
        'authorityKeyIdentifier=keyid:always\n')
    for arguments in [
        ['req', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes',
         '-subj', '/CN=Runtime', '-keyout', 'runtime.key', '-out', 'runtime.csr'],
        ['x509', '-req', '-in', 'runtime.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key',
         '-CAcreateserial', '-days', '1', '-extfile', 'runtime.ext', '-out', 'runtime.pem'],
    ]:
        reply = subprocess.run(['openssl', *arguments], cwd=tls_root, capture_output=True, timeout=5)
        assert reply.returncode == 0, 'fixture runtime certificate generation failed'
    (tls_root / 'runtime.key').chmod(0o600)
    database = {'host': 'localhost', 'port': int(port), 'name': 'p03_identity',
                'migration_role': 'p03_identity_migration', 'runtime_role': 'p03_identity_runtime',
                'password_file': str(root / 'password-p03_identity_runtime'),
                'ca_file': str(root / 'server.pem')}
    config['database'] = database
    config_file = root / 'identity.json'
    config_file.write_text(json.dumps(config))
    config_file.chmod(0o600)
    password = secrets.token_hex(32)
    password_file = root / 'admin-password'
    password_file.write_text(password + '\n')
    password_file.chmod(0o600)
    admin, control = str(uuid.uuid4()), str(uuid.uuid4())
    enrolled = {'schema_version': 1, 'administrator_id': admin,
                'password_credential_id': str(uuid.uuid4()), 'username': 'administrator',
                'control_id': control, 'control_credential_id': str(uuid.uuid4()),
                'control_certificate_chain_file': str(tls_root / 'control.pem'),
                'control_san': 'spiffe://kiarabot/control-test',
                'administrator_assignments': [
                    {'assignment_id': str(uuid.uuid4()), 'role': 'admin',
                     'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': admin}]}],
                'control_assignments': [
                    {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.delegation.issue'],
                     'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': control}]},
                    {'assignment_id': str(uuid.uuid4()),
                     'permissions': ['message.send', 'message.react', 'message.delete',
                                     'member.mute', 'member.kick', 'request.approve',
                                     'namespace.read', 'log.read', 'audit.read', 'health.read'],
                     'scope': [{'kind': 'RESOURCE_ACCOUNT', 'id': str(uuid.uuid4())}]}]}
    runtime_principal, runtime_credential = str(uuid.uuid4()), str(uuid.uuid4())
    enrolled['platform_peers'] = [{
        'service': 'runtime', 'principal_id': runtime_principal,
        'credential_id': runtime_credential,
        'certificate_chain_file': str(tls_root / 'runtime.pem'),
        'san': 'spiffe://kiarabot/runtime-test',
        'assignments': [
            {'assignment_id': str(uuid.uuid4()),
             'permissions': ['identity.authorization.check'],
             'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': runtime_principal}]}]}]
    if host_enrollment:
        # v1.1 Host enrollment: HostId is independent from PrincipalId and the peer must carry an
        # explicit RESOURCE_HOST binding. host-1 has the exact host.execute grant; host-2 does not,
        # so the real Identity gate must answer a denial decision instead of a fake allow.
        HOST_STATE.clear()
        enrolled_hosts = host_permissions or {'host-1': ['host.execute'],
                                              'host-2': ['host.read']}
        for name in ('host-1', 'host-2'):
            permissions = enrolled_hosts.get(name, ['host.read'])
            extension = tls_root / f'{name}.ext'
            # The real Host process uses this single material both as the enrolled HostBridge /
            # Identity client identity and as its owner mTLS server certificate, so the SANs must
            # also cover the local endpoint and the EKU must allow serverAuth.
            extension.write_text(f'subjectAltName=URI:spiffe://kiarabot/{name}-test,'
                                 'DNS:localhost,IP:127.0.0.1\n'
                                 'extendedKeyUsage=clientAuth,serverAuth\n'
                                 'basicConstraints=critical,CA:FALSE\n'
                                 'keyUsage=critical,digitalSignature\n'
                                 'subjectKeyIdentifier=hash\n'
                                 'authorityKeyIdentifier=keyid:always\n')
            for arguments in [
                    ['req', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes',
                     '-subj', f'/CN={name}', '-keyout', str(tls_root / f'{name}.key'),
                     '-out', str(tls_root / f'{name}.csr')],
                    ['x509', '-req', '-in', str(tls_root / f'{name}.csr'), '-CA',
                     str(tls_root / 'ca.pem'), '-CAkey', str(tls_root / 'ca.key'),
                     '-CAcreateserial', '-days', '1', '-extfile', str(extension),
                     '-out', str(tls_root / f'{name}.pem')]]:
                reply = subprocess.run(['openssl', *arguments], cwd=tls_root,
                                       capture_output=True, timeout=5)
                assert reply.returncode == 0, 'fixture host certificate generation failed'
            (tls_root / f'{name}.key').chmod(0o600)
            principal, credential = str(uuid.uuid4()), str(uuid.uuid4())
            host_id, assignment = str(uuid.uuid4()), str(uuid.uuid4())
            enrolled['platform_peers'].append({
                'service': 'host', 'principal_id': principal, 'credential_id': credential,
                'certificate_chain_file': str(tls_root / f'{name}.pem'),
                'san': f'spiffe://kiarabot/{name}-test', 'host_id': host_id,
                'assignments': [{'assignment_id': assignment, 'permissions': permissions,
                                 'scope': [{'kind': 'RESOURCE_HOST', 'id': host_id}]}]})
            HOST_STATE[name] = {
                'principal_id': principal, 'credential_id': credential, 'host_id': host_id,
                'assignment_id': assignment,
                'certificate_file': str(tls_root / f'{name}.pem'),
                'key_file': str(tls_root / f'{name}.key')}
        assert HOST_STATE['host-1']['principal_id'] != HOST_STATE['host-1']['host_id']
    if prepare is not None:
        prepare(root, enrolled)
    if verification_catalog is not None:
        enrolled['control_assignments'].append({
            'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.keys.read'],
            'scope': [{'kind': 'RESOURCE_CATALOG', 'id': verification_catalog}],
        })
    enrollment_file = root / 'enrollment.json'

    def write(value):
        enrollment_file.write_text(json.dumps(value))
        enrollment_file.chmod(0o600)

    def run(arguments, expected=0):
        reply = subprocess.run([bootstrap, *arguments], env=environment, capture_output=True,
                               text=True, timeout=8)
        if reply.returncode != expected:
            codes = re.findall(r'error_code=[A-Z_]+', reply.stderr)
            print(f'P03 assertion: bootstrap exit={reply.returncode} expected={expected} '
                  f'codes={codes}; diagnostics redacted', file=sys.stderr)
        assert reply.returncode == expected, 'bootstrap exit mismatch; diagnostics redacted'
        output = reply.stdout + reply.stderr
        for secret in [password, migration, runtime, 'PRIVATE KEY', str(root)]:
            assert secret not in output, 'bootstrap leaked private material'
        return output

    clean_environment = {key: value for key, value in environment.items() if not key.startswith('PG')}
    # The production adapter intentionally rejects ambient libpq overrides.
    environment.clear()
    environment.update(clean_environment)
    initialized = subprocess.run(
        [initializer, '--initialize-empty', 'identity', 'localhost', port, 'p03_identity',
         'p03_identity_migration', 'p03_identity_runtime', str(root / 'server.pem'),
         str(root / 'password-p03_identity_migration')],
        capture_output=True, text=True, env=environment, timeout=10)
    assert initialized.returncode == 0, 'owned initialization failed; diagnostics redacted'
    run(['--help'])
    run([], 2)
    run(['--config', str(config_file), '--status', '--status'], 2)
    status_arguments = ['--config', str(config_file), '--status']
    command = ['--config', str(config_file), '--enrollment', str(enrollment_file),
               '--password-file', str(password_file)]
    assert 'state=required version=1' in run(status_arguments)
    write({**enrolled, 'control_san': 'spiffe://kiarabot/wrong-control'})
    run(command, 1)
    assert 'state=required version=1' in run(status_arguments)
    write(enrolled)
    password_file.chmod(0o644)
    run(command, 1)
    password_file.chmod(0o600)
    assert 'state=required version=1' in run(status_arguments)
    enrollment_file.write_text(json.dumps(enrolled)[:-1] + ',"schema_version":1}')
    run(command, 1)
    assert 'state=required version=1' in run(status_arguments)
    tampered = json.loads(json.dumps(enrolled))
    tampered['platform_peers'][0]['san'] = 'spiffe://kiarabot/wrong-runtime'
    write(tampered)
    run(command, 1)
    assert 'state=required version=1' in run(status_arguments)
    invalid_permission = json.loads(json.dumps(enrolled))
    invalid_permission['control_assignments'].append({
        'assignment_id': str(uuid.uuid4()), 'permissions': ['not.a.permission'],
        'scope': [{'kind': 'RESOURCE_ACCOUNT', 'id': str(uuid.uuid4())}]})
    write(invalid_permission)
    run(command, 1)
    assert 'state=required version=1' in run(status_arguments)
    if host_enrollment:
        def peer_with(value, service):
            return next(peer for peer in value['platform_peers'] if peer['service'] == service)

        def host_tampered(mutate):
            value = json.loads(json.dumps(enrolled))
            mutate(peer_with(value, 'host'))
            return value

        write(host_tampered(lambda peer: peer.pop('host_id')))
        run(command, 1)
        assert 'state=required version=1' in run(status_arguments)
        write(host_tampered(lambda peer: peer.__setitem__('host_id', '')))
        run(command, 1)
        assert 'state=required version=1' in run(status_arguments)
        write(host_tampered(lambda peer: peer.__setitem__('host_id', 'h' * 129)))
        run(command, 1)
        assert 'state=required version=1' in run(status_arguments)
        runtime_with_host = json.loads(json.dumps(enrolled))
        peer_with(runtime_with_host, 'runtime')['host_id'] = str(uuid.uuid4())
        write(runtime_with_host)
        run(command, 1)
        assert 'state=required version=1' in run(status_arguments)
    write(enrolled)
    assert 'outcome=committed state=consumed' in run(command)
    assert 'state=consumed version=2' in run(status_arguments)
    assert sql(f"SELECT count(*) FROM kiarabot.principals WHERE id='{runtime_principal}';", 'p03_identity') == '1'
    assert sql(f"SELECT count(*) FROM kiarabot.credential_heads WHERE id='{runtime_credential}';", 'p03_identity') == '1'
    assert sql(f"SELECT count(*) FROM kiarabot.exact_assignments WHERE subject='{runtime_principal}';", 'p03_identity') == '1'
    assert sql("SELECT count(DISTINCT permission) FROM kiarabot.exact_permissions "
               "WHERE permission BETWEEN 41 AND 50;", 'p03_identity') == '10'
    if host_enrollment:
        for name in ('host-1', 'host-2'):
            host = HOST_STATE[name]
            assert sql(f"SELECT binding_kind||':'||binding_id FROM kiarabot.principals "
                       f"WHERE id='{host['principal_id']}';", 'p03_identity') == \
                '5:' + host['host_id']
            assert sql(f"SELECT platform_service::text FROM kiarabot.credential_certificates "
                       f"WHERE id='{host['credential_id']}';", 'p03_identity') == '2'
            # The base enrollment always creates the exact host grant; a real Host consumer may
            # additionally carry identity.authorization.check, so only presence is asserted.
            assert int(sql(f"SELECT count(*) FROM kiarabot.exact_assignments "
                           f"WHERE subject='{host['principal_id']}';", 'p03_identity')) >= 1
    assert 'CONFLICT' in run(command, 1)
    assert 'state=consumed version=2' in run(status_arguments)
    print('Real owned PG offline bootstrap, rejected enrollment, once-only state and redaction passed')
    if after_bootstrap is not None:
        after_bootstrap(root)


if __name__ == '__main__':
    try:
        main()
    except Exception:
        frames = traceback.extract_tb(sys.exc_info()[2])
        lines = ','.join(str(frame.lineno) for frame in frames[-2:])
        print(f'P03 assertion: bootstrap fixture lines {lines}; diagnostics redacted', file=sys.stderr)
        sys.exit(1)
