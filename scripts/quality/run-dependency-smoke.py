#!/usr/bin/env python3
"""P00-only disposable TLS PG/JWT dependency probe; never accepts an external DSN."""
import argparse
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pg-bin', type=Path, required=True)
    parser.add_argument('--openssl', type=Path, required=True)
    parser.add_argument('--smoke', type=Path, required=True)
    args = parser.parse_args()
    pg_bin, openssl, smoke = args.pg_bin.resolve(), args.openssl.resolve(), args.smoke.resolve()
    def run(command, **kwargs):
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=60, **kwargs)
        if result.returncode:
            raise RuntimeError(f'toolchain fixture command failed: {Path(command[0]).name}; exit={result.returncode}; '
                               + (result.stderr.decode() if Path(command[0]).name == 'kiarabot_dependency_smoke' else 'redacted'))
        return result
    with tempfile.TemporaryDirectory(prefix='kiarabot-toolchain-pg-') as temporary:
        root = Path(temporary).resolve()
        root.chmod(0o700)
        password = secrets.token_hex(32)
        password_file = root / 'password'
        password_file.write_text(password + '\n')
        password_file.chmod(0o600)
        # Explicitly isolated cluster: no default PGHOST/PGDATABASE or external connection accepted.
        environment = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
        environment['LC_ALL'] = 'C'
        run([str(pg_bin / 'initdb'), '-D', str(root / 'data'), '-A', 'scram-sha-256',
             '-U', 'kiarabot_toolchain', '--pwfile', str(password_file), '--no-locale'], env=environment)
        run([str(openssl), 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes', '-days', '1',
             '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1',
             '-keyout', str(root / 'server.key'), '-out', str(root / 'server.pem')])
        (root / 'server.key').chmod(0o600)
        run([str(openssl), 'genpkey', '-algorithm', 'ED25519', '-out', str(root / 'jwt.key')])
        (root / 'jwt.key').chmod(0o600)
        run([str(openssl), 'pkey', '-in', str(root / 'jwt.key'), '-pubout',
             '-out', str(root / 'public.pem')])
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        with (root / 'data/postgresql.conf').open('a') as stream:
            stream.write(f"\nlisten_addresses='127.0.0.1'\nport={port}\nssl=on\n"
                         f"ssl_cert_file='{root}/server.pem'\nssl_key_file='{root}/server.key'\n"
                         f"unix_socket_directories='{root}'\n")
        started = False
        try:
            run([str(pg_bin / 'pg_ctl'), '-D', str(root / 'data'), '-l', str(root / 'postgres.log'),
                 '-w', 'start'], env=environment)
            started = True
            environment['KIARABOT_TOOLCHAIN_TEST_DSN'] = (
                f'host=127.0.0.1 port={port} dbname=postgres user=kiarabot_toolchain '
                f'password={password} sslmode=verify-full channel_binding=require sslrootcert={root}/server.pem')
            result = run([str(smoke), str(root / 'public.pem'), str(root / 'jwt.key')], env=environment)
            print(result.stdout.decode(), end='')
        finally:
            if started or (root / "data/postmaster.pid").exists():
                run([str(pg_bin / 'pg_ctl'), '-D', str(root / 'data'), '-m', 'fast', '-w', 'stop'],
                    env=environment)
    print('Temporary PG cluster and private material removed; not P03 Repository acceptance')


if __name__ == '__main__':
    main()
