"""Configuration/TLS contract checks; all certificate material lives in a temporary directory."""
import copy
import json
from datetime import datetime, timezone
import os
import select
import socket
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def certificates(root, key_kind="rsa"):
    def openssl(*arguments):
        subprocess.run(["openssl", *arguments], check=True, cwd=root,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=P02-Test-CA", "-keyout", "ca.key", "-out", "ca.pem")
    key_options = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256"] if key_kind == "ec" else ["-newkey", "rsa:2048"]
    openssl("req", *key_options, "-nodes", "-subj", "/CN=localhost",
            "-keyout", "server.key", "-out", "server.csr")
    (root / "extensions").write_text(
        "subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth,clientAuth\n")
    for name, days in [("server.pem", "1")]:
        openssl("x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
                "-CAcreateserial", "-days", days, "-extfile", "extensions", "-out", name)
    (root / "index").touch()
    (root / "serial").write_text("1000\n")
    (root / "ca.cnf").write_text(
        "[ca]\ndefault_ca=local\n[local]\ndatabase=index\nserial=serial\n"
        "new_certs_dir=.\ncertificate=ca.pem\nprivate_key=ca.key\ndefault_md=sha256\n"
        "policy=policy\n[policy]\ncommonName=supplied\n")
    openssl("ca", "-batch", "-config", "ca.cnf", "-in", "server.csr",
            "-out", "expired.pem", "-startdate", "200101000000Z", "-enddate", "200102000000Z")
    os.chmod(root / "server.key", 0o600)
    return {
        "schema_version": 1, "service": "identity", "probe_listen": "127.0.0.1:0",
        "grpc_listen": "127.0.0.1:0",
        "tls": {"certificate_file": str(root / "server.pem"),
                "private_key_file": str(root / "server.key"), "ca_file": str(root / "ca.pem")},
    }


def main(driver, identity=None):
    with tempfile.TemporaryDirectory(prefix="p02-config-") as directory:
        root = Path(directory)
        config = certificates(root)
        path = root / "config.json"

        def expect(expected, value=None, raw=None, service=None):
            path.write_text(raw if raw is not None else json.dumps(value or config))
            command = [driver, str(path)]
            if service is not None:
                command.extend([str(int(time.time())), service])
            result = subprocess.run(command, capture_output=True, text=True, timeout=3)
            assert result.returncode == expected, (expected, result.returncode, result.stderr)
            if expected == 0:
                encoded_security = json.loads(path.read_text()).get('identity_security') is not None
                assert result.stdout.strip() == f'identity_security={str(encoded_security).lower()}', result.stdout
            assert "PRIVATE KEY" not in result.stderr and str(root) not in result.stderr
            assert 'never-print-database-password' not in result.stderr

        expect(0)
        path.write_text(json.dumps(config))
        for flag, adjustment, expected in [("-startdate", 0, 0), ("-startdate", -1, 1),
                                           ("-enddate", 0, 1)]:
            encoded = subprocess.check_output(
                ["openssl", "x509", "-in", str(root / "server.pem"), "-noout", flag], text=True)
            instant = datetime.strptime(encoded.strip().split("=", 1)[1], "%b %d %H:%M:%S %Y %Z")
            epoch = int(instant.replace(tzinfo=timezone.utc).timestamp()) + adjustment
            result = subprocess.run([driver, str(path), str(epoch)], capture_output=True, timeout=3)
            assert result.returncode == expected, (flag, adjustment, result.returncode)

        expect(2, {**config, "service": "runtime"})
        expect(2, {**config, "secret": "never-print-this"})
        expect(2, {**config, "probe_listen": "0.0.0.0:1234"})
        expect(2, {**config, "schema_version": 2})
        expect(2, raw=json.dumps(config)[:-1] + ',"service":"identity"}')
        expect(2, {**config, "tls": {**config["tls"], "extra": True}})
        expect(1, {**config, "tls": {**config["tls"], "certificate_file": str(root / "expired.pem")}})
        expect(1, {**config, "tls": {**config["tls"], "private_key_file": str(root / "ca.key")}})
        expect(1, {**config, "tls": {**config["tls"], "ca_file": str(root / "server.pem")}})
        expect(1, {**config, "tls": {**config["tls"], "ca_file": str(root / "missing")}})
        os.chmod(root / "server.key", 0o644)
        expect(1)
        os.chmod(root / "server.key", 0o600)
        (root / "link.key").symlink_to(root / "server.key")
        expect(1, {**config, "tls": {**config["tls"], "private_key_file": str(root / "link.key")}})
        expect(2, raw="{invalid secret-config}")
        os.mkfifo(root / "fifo")
        for field in ["certificate_file", "private_key_file", "ca_file"]:
            expect(1, {**config, "tls": {**config["tls"], field: str(root / "fifo")}})
        fifo_result = subprocess.run([driver, str(root / "fifo")], capture_output=True, timeout=3)
        assert fifo_result.returncode == 2, fifo_result.returncode
        ec_root = root / "ec"
        ec_root.mkdir()
        expect(0, certificates(ec_root, "ec"))
        password = root / 'pg-password'
        password.write_text('never-print-database-password\n')
        password.chmod(0o600)
        database = {'host': 'localhost', 'port': 5432, 'name': 'private_identity',
                    'migration_role': 'identity_migration', 'runtime_role': 'identity_runtime',
                    'password_file': str(password), 'ca_file': str(root / 'ca.pem')}
        database_config = {**config, 'database': database}
        expect(0, database_config)
        for service in ['runtime', 'scheduler', 'model']:
            expect(0, {**database_config, 'service': service}, service=service)
        for service, name in [('host', 'plugin-host'), ('control', 'control')]:
            value = {**config, 'service': name}
            if service == 'control':
                value.pop('grpc_listen')
            expect(0, value, service=service)
            expect(2, {**value, 'database': database}, service=service)
        control = {**config, 'service': 'control'}
        control.pop('grpc_listen')
        endpoint = {'listen': '0.0.0.0:8443', 'identity_endpoint': 'identity.internal:7443'}
        expect(0, {**control, 'control_api': endpoint}, service='control')
        expect(2, {**config, 'control_api': endpoint})
        for update in [{'identity_endpoint': 'https://identity:443'},
                       {'identity_endpoint': 'identity:0'}, {'identity_endpoint': 'identity:65536'},
                       {'identity_endpoint': 'one,two:443'}, {'identity_endpoint': 'x:443/path'},
                       {'identity_endpoint': 'x:443?verify=false'}, {'identity_endpoint': ':443'},
                       {'identity_endpoint': 'x:443\u0000'}, {'listen': 'localhost:443'},
                       {'listen': True}, {'identity_endpoint': 443}, {'extra': True}]:
            expect(2, {**control, 'control_api': {**endpoint, **update}}, service='control')
        expect(2, raw=json.dumps({**control, 'control_api': endpoint})[:-1] +
               ',"control_api":' + json.dumps(endpoint) + '}', service='control')
        for update in [{'port': 0}, {'port': -1}, {'port': 65536}, {'port': True},
                       {'name': 'db name'}, {'host': '/tmp/socket'}, {'host': 'one,two'},
                       {'host': 'bad host'}, {'runtime_role': 'identity_migration'},
                       {'password': 'forbidden'}, {'sslmode': 'disable'}]:
            expect(2, {**config, 'database': {**database, **update}})
        for key in database:
            expect(2, {**config, 'database': {k: v for k, v in database.items() if k != key}})
        password.chmod(0o644)
        expect(1, database_config)
        password.chmod(0o600)
        for secret in ['', 'two\nlines\n', 'x' * 4097, 'x' * 4096 + '\n', 'embedded\0nul']:
            password.write_text(secret)
            expect(1 if not secret else 2, database_config)
        password.write_text('never-print-database-password\n')
        (root / 'pg-password-link').symlink_to(password)
        for key in ['password_file', 'ca_file']:
            expect(1, {**config, 'database': {**database, key: str(root / 'fifo')}})
        expect(1, {**config, 'database': {**database, 'password_file': str(root / 'pg-password-link')}})
        security = {
            'token_key': {'id': 'token-v1', 'file': str(root / 'token.hex')},
            'request_key': {'id': 'request-v1', 'file': str(root / 'request.hex')},
            'receipt_key': {'id': 'receipt-v1', 'file': str(root / 'receipt.hex')},
            'delegation_key': {'id': 'delegation-v1', 'public_key_file': str(root / 'public.pem'),
                               'private_key_file': str(root / 'private.pem')},
            'issuer': 'kiarabot-identity',
            'clock': {'trusted': True, 'maximum_known_skew_ms': 2000},
            'verification': {'catalog_id': '11111111-1111-4111-8111-111111111111',
                             'version': 1, 'key_version': 1,
                             'valid_until_epoch_seconds': 4102444800},
        }
        secured = {**database_config, 'identity_security': security}
        # Only trusted references are parsed here; the production composition loads keys securely.
        expect(0, secured)
        zero_skew = copy.deepcopy(secured)
        zero_skew['identity_security']['clock']['maximum_known_skew_ms'] = 0
        expect(0, zero_skew)
        expect(0, raw=json.dumps(secured).replace('"maximum_known_skew_ms": 2000', '"maximum_known_skew_ms": -0'))
        expect(2, {**config, 'identity_security': security})
        for service, name in [('runtime', 'runtime'), ('scheduler', 'scheduler'), ('model', 'model'),
                              ('host', 'plugin-host'), ('control', 'control')]:
            wrong_owner = {**secured, 'service': name}
            if service == 'control':
                wrong_owner.pop('grpc_listen')
            expect(2, wrong_owner, service=service)
        for section in security:
            missing = copy.deepcopy(secured)
            missing['identity_security'].pop(section)
            expect(2, missing)
        expect(2, {**database_config, 'identity_security': {**security, 'unexpected': True}})
        expect(2, {**database_config, 'identity_security': True})
        for section in ['token_key', 'request_key', 'receipt_key', 'delegation_key', 'clock', 'verification']:
            for key in security[section]:
                missing = copy.deepcopy(secured)
                missing['identity_security'][section].pop(key)
                expect(2, missing)
            unknown = copy.deepcopy(secured)
            unknown['identity_security'][section]['unexpected'] = True
            expect(2, unknown)
        for field in ['token_key', 'request_key', 'receipt_key']:
            for update in [{'id': ''}, {'id': 'x' * 1025}, {'id': 'bad\0key'}, {'id': True},
                           {'file': ''}, {'file': 'x' * 4097}, {'file': 'bad\0path'}, {'file': 123}]:
                malformed = copy.deepcopy(secured)
                malformed['identity_security'][field].update(update)
                expect(2, malformed)
        for update in [{'trusted': False}, {'trusted': 1}, {'trusted': 'true'},
                       {'maximum_known_skew_ms': -1}, {'maximum_known_skew_ms': 2001},
                       {'maximum_known_skew_ms': True}, {'maximum_known_skew_ms': 0.0},
                       {'maximum_known_skew_ms': 18446744073709551615}]:
            malformed = copy.deepcopy(secured)
            malformed['identity_security']['clock'].update(update)
            expect(2, malformed)
        for field in ['version', 'key_version', 'valid_until_epoch_seconds']:
            for value in [0, -1, True, 1.0, '1', None]:
                malformed = copy.deepcopy(secured)
                malformed['identity_security']['verification'][field] = value
                expect(2, malformed)
        for value in [9223372037, 18446744073709551615]:
            malformed = copy.deepcopy(secured)
            malformed['identity_security']['verification']['valid_until_epoch_seconds'] = value
            expect(2, malformed)
        for value in ['', 'all', 'keys', '11111111-1111-1111-8111-111111111111',
                      '11111111-1111-4111-7111-111111111111',
                      'AAAAAAAA-1111-4111-8111-111111111111', True]:
            malformed = copy.deepcopy(secured)
            malformed['identity_security']['verification']['catalog_id'] = value
            expect(2, malformed)
        maximum = copy.deepcopy(secured)
        maximum['identity_security']['verification'].update(
            version=18446744073709551615, key_version=18446744073709551615,
            valid_until_epoch_seconds=9223372036)
        expect(0, maximum)
        expect(2, raw=json.dumps(secured).replace('"key_version": 1',
                                                '"key_version": 1, "key_version": 2'))
        for issuer in ['', True, 'x' * 1025, 'bad\0issuer']:
            malformed = copy.deepcopy(secured)
            malformed['identity_security']['issuer'] = issuer
            expect(2, malformed)
        for field in ['id', 'public_key_file', 'private_key_file']:
            for value in ['', True, 'bad\0path']:
                malformed = copy.deepcopy(secured)
                malformed['identity_security']['delegation_key'][field] = value
                expect(2, malformed)
        expect(2, raw=json.dumps(secured).replace('"id": "token-v1"', '"id": "token-v1", "id": "other"'))
        expect(2, raw=json.dumps(secured).replace('"trusted": true', '"trusted": false, "trusted": true'))
        if identity is not None:
            # Missing crypto material must fail before touching the database or announcing a listener.
            with socket.socket() as trap:
                trap.bind(('127.0.0.1', 0))
                trap.listen(1)
                unavailable_keys = copy.deepcopy(secured)
                unavailable_keys['database']['host'] = '127.0.0.1'
                unavailable_keys['database']['port'] = trap.getsockname()[1]
                path.write_text(json.dumps(unavailable_keys))
                refused = subprocess.run([identity, '--config', str(path)], capture_output=True, text=True, timeout=3)
                assert refused.returncode == 1, refused
                assert 'probe_port=' not in refused.stdout and 'grpc_port=' not in refused.stdout, refused.stdout
                assert not select.select([trap], [], [], 0)[0], 'unsafe configuration contacted the database'
                assert 'never-print-database-password' not in refused.stderr and str(root) not in refused.stderr
        print("Service configuration/TLS and Identity security schema checks passed")


if __name__ == "__main__":
    main(*sys.argv[1:])
