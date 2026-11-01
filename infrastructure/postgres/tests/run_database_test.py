#!/usr/bin/env python3
"""Disposable PG16.4 TLS fixture. No external DSN or production connection accepted."""
import argparse
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import tempfile

from commit_receipt_proxy import CommitReceiptProxy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pg-bin', type=Path, required=True)
    parser.add_argument('--openssl', type=Path, required=True)
    parser.add_argument('--test', type=Path, required=True)
    parser.add_argument('--initializer', type=Path, required=True)
    parser.add_argument('--test-argument', action='append', default=[],
                        help='Additional arguments for an owned local fixture driver')
    args = parser.parse_args()
    pg_bin, openssl, test = args.pg_bin.resolve(), args.openssl.resolve(), args.test.resolve()
    driver_names = ('database_test', 'database_test_sanitized', 'kiarabot_postgres_database_test')
    environment = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
    environment['LC_ALL'] = 'C'

    def run(command, timeout=45, expected=0):
        result = subprocess.run(command, env=environment, capture_output=True, timeout=timeout)
        if result.returncode != expected:
            if Path(command[0]).name in driver_names:
                for line in result.stderr.decode(errors='replace').splitlines():
                    if line.startswith('P03 assertion: '):
                        print(line, flush=True)
            raise RuntimeError(f'fixture command failed: {Path(command[0]).name}; '
                               f'exit={result.returncode}; output redacted')
        return result

    if not test.is_file() or not args.initializer.resolve().is_file():
        raise RuntimeError('compiled test and initializer required')
    version = run([str(pg_bin / 'postgres'), '--version']).stdout.decode().strip()
    if version != 'postgres (PostgreSQL) 16.4':
        raise RuntimeError('PG16.4 required')
    root = Path(tempfile.mkdtemp(prefix='kiarabot-p03-')).resolve()
    token = secrets.token_hex(32)
    marker = root / 'p03-owned-marker'
    marker.write_text(token)
    try:
        root.chmod(0o700)
        password_file = root / 'password'
        password_file.write_text(secrets.token_hex(32) + '\n')
        password_file.chmod(0o600)
        for service in ('identity', 'runtime', 'scheduler', 'model'):
            for identity in ('migration', 'runtime'):
                role_secret = root / f'password-p03_{service}_{identity}'
                role_secret.write_text(secrets.token_hex(32) + '\n')
                role_secret.chmod(0o600)
        initializer = str(args.initializer.resolve())
        run([initializer, '--help'])
        run([initializer], expected=2)
        cli = [initializer, '--initialize-empty', 'runtime', 'localhost', '1',
               'p03_runtime', 'p03_runtime_migration', 'p03_runtime_runtime',
               str(root / 'server.pem')]
        # File rejection is tested before a cluster exists; a FIFO must not block open().
        fifo = root / 'password-fifo'
        os.mkfifo(fifo, mode=0o600)
        run(cli + [str(fifo)], timeout=2, expected=2)
        link = root / 'password-link'
        link.symlink_to(password_file)
        run(cli + [str(link)], timeout=2, expected=2)
        password_file.chmod(0o644)
        run(cli + [str(password_file)], expected=2)
        password_file.chmod(0o600)
        multiline = root / 'password-multiline'
        multiline.write_text('invalid\nmultiline\n')
        multiline.chmod(0o600)
        run(cli + [str(multiline)], expected=2)
        run([str(pg_bin / 'initdb'), '-D', str(root / 'data'), '-A', 'scram-sha-256',
             '-U', 'p03_admin', '--pwfile', str(password_file), '--no-locale', '--encoding=UTF8'])
        run([str(openssl), 'req', '-x509', '-newkey', 'ec', '-pkeyopt',
             'ec_paramgen_curve:P-256', '-nodes', '-days', '1', '-subj', '/CN=localhost',
             '-addext', 'subjectAltName=DNS:localhost', '-keyout', str(root / 'server.key'),
             '-out', str(root / 'server.pem')])
        (root / 'server.key').chmod(0o600)
        # No trust/plain TCP authentication route, including fixture administrator.
        (root / 'data/pg_hba.conf').write_text(
            'hostssl all all 127.0.0.1/32 scram-sha-256\n'
            'hostnossl all all 127.0.0.1/32 reject\n')
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        with (root / 'data/postgresql.conf').open('a') as stream:
            stream.write(f"\nlisten_addresses='127.0.0.1'\nport={port}\nssl=on\n"
                         f"ssl_cert_file='{root}/server.pem'\nssl_key_file='{root}/server.key'\n"
                         f"unix_socket_directories='{root}'\n")
        run([str(pg_bin / 'pg_ctl'), '-D', str(root / 'data'), '-l', str(root / 'pg.log'),
             '-w', '-t', '20', 'start'])
        with CommitReceiptProxy(port, root) as proxy:
            if test.name in driver_names:
                mismatch = root / 'kiarabot-p03-backend-mismatch'
                mismatch.mkdir(mode=0o700)
                (mismatch / 'data').mkdir()
                (mismatch / 'p03-owned-marker').write_text('mismatched backend')
                shutil.copyfile(password_file, mismatch / 'password')
                (mismatch / 'password').chmod(0o600)
                shutil.copyfile(root / 'server.pem', mismatch / 'server.pem')
                refused = run([str(test), str(mismatch), str(port),
                               str(args.initializer.resolve()), str(proxy.port)], expected=1)
                if b'fixture backend directory mismatch' not in refused.stderr:
                    raise RuntimeError('test driver did not verify backend ownership before DDL')
            output = run([str(test), str(root), str(port), str(args.initializer.resolve()),
                          str(proxy.port), *args.test_argument])
            print(output.stdout.decode(), end='')
    finally:
        # No TemporaryDirectory auto-deletion: unknown ownership or failed bounded stop must
        # preserve the directory for inspection, including a potentially still-live cluster.
        if (root.is_symlink() or not root.name.startswith('kiarabot-p03-')
                or not marker.is_file() or marker.is_symlink() or marker.read_text() != token
                or root.stat().st_uid != os.getuid()):
            raise RuntimeError('fixture ownership changed; refusing cluster control and removal')
        if (root / 'data/postmaster.pid').exists():
            run([str(pg_bin / 'pg_ctl'), '-D', str(root / 'data'), '-m', 'immediate',
                 '-w', '-t', '20', 'stop'], timeout=25)
        shutil.rmtree(root)
    print('Owned temporary PG cluster and private material removed')


if __name__ == '__main__':
    main()
