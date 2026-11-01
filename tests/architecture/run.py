#!/usr/bin/env python3
"""P02 real-service black-box acceptance. Missing binaries/dependencies are failures."""
import argparse
import copy
import os
import signal
import tempfile
from pathlib import Path

from fixtures import SERVICES, TestPki
from process_support import (AcceptanceFailure, ServiceProcess, assert_closed, assert_equal,
                             assert_exit, assert_probe, run_once, service_config, write_config)


def lifecycle(binaries, pki, directory):
    config_error = r'config|argument|usage|schema|json|parse|address|INVALID_ARGUMENT'
    for service, binary in binaries.items():
        before_help = set(Path(directory).iterdir())
        help_result = run_once(binary, ['--help'], directory)
        assert_exit(help_result, 0, '')
        assert_equal(set(Path(directory).iterdir()), before_help, service + ' help file side effects')
        if b'--config' not in help_result.stdout + help_result.stderr:
            raise AcceptanceFailure(service + ' help did not describe --config')
        # No config means there is no valid listener to start; no file side effects either.
        for arguments in ([], ['--config'], ['--unknown'], ['--config', 'absent.json'],
                          ['--help', '--unknown']):
            assert_exit(run_once(binary, arguments, directory), 2, config_error)
        baseline = service_config(service, pki)
        invalid = []
        for key, value in [('schema_version', 0), ('schema_version', 2), ('service', 'unknown'),
                           ('service', 'runtime' if service != 'runtime' else 'identity'),
                           ('unknown_field', 'test'), ('probe_listen', baseline['probe_listen'].replace('127.0.0.1', '0.0.0.0'))]:
            config = copy.deepcopy(baseline)
            config[key] = value
            invalid.append(config)
        for key in ['schema_version', 'service', 'probe_listen', 'tls']:
            config = copy.deepcopy(baseline)
            del config[key]
            invalid.append(config)
        config = copy.deepcopy(baseline)
        config['tls']['unknown_field'] = 'test'
        invalid.append(config)
        if service != 'control':
            config = copy.deepcopy(baseline)
            del config['grpc_listen']
            invalid.append(config)
        for config in invalid:
            path = write_config(directory, service, config)
            assert_exit(run_once(binary, ['--config', path], directory), 2, config_error)
            assert_closed(baseline['probe_listen'])
            if 'grpc_listen' in baseline:
                assert_closed(baseline['grpc_listen'])
        malformed = Path(directory) / 'malformed.json'
        malformed.write_text('{"schema_version":')
        assert_exit(run_once(binary, ['--config', malformed], directory), 2, config_error)
        for stop_signal in (signal.SIGTERM, signal.SIGINT):
            config = service_config(service, pki)
            with ServiceProcess(binary, config, directory, service) as child:
                child.wait_live()
                assert_probe(config['probe_listen'], '/health/live', 200, 'HEALTHY')
                assert_probe(config['probe_listen'], '/health/ready', 503, 'UNHEALTHY')
                assert_probe(config['probe_listen'], '/unknown-p02-path', 404)
                if service == 'control':
                    for path in ('/api/v1/health', '/api/v1/auth/login', '/ws/control'):
                        status, _ = __import__('process_support').http_probe(config['probe_listen'], path)
                        if status < 400:
                            raise AcceptanceFailure('P02 Control unexpectedly exposed business route')
                child.stop(stop_signal)
        print(service + ': CLI/configuration/live/not-ready/TERM/INT passed', flush=True)


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openssl', default=os.environ.get('KIARABOT_P02_OPENSSL'), type=Path)
    parser.add_argument('--group', choices=['lifecycle', 'tls', 'isolation', 'all'], default='all')
    binaries = {}
    for service in SERVICES:
        parser.add_argument('--' + service, default=os.environ.get(
            'KIARABOT_P02_BINARY_' + service.upper().replace('-', '_')), type=Path)
    options = parser.parse_args()
    if options.openssl is None or not options.openssl.is_file() or not os.access(options.openssl, os.X_OK):
        parser.error('an explicit executable --openssl / KIARABOT_P02_OPENSSL is required')
    for service in SERVICES:
        path = getattr(options, service.replace('-', '_'))
        if path is None or not path.is_file() or not os.access(path, os.X_OK):
            parser.error('missing executable for ' + service + '; all six binaries are required')
        binaries[service] = path.resolve()
    if len(set(binaries.values())) != len(SERVICES):
        parser.error('each service must have its own explicit executable path')
    return options, binaries


def main():
    options, binaries = arguments()
    try:
        if options.group in ('all', 'tls'):
            from tls_acceptance import load_grpc
            load_grpc()
        with TestPki(options.openssl) as pki, tempfile.TemporaryDirectory(prefix='kiarabot-p02-run-') as directory:
            print('Test certificate generator: ' + pki.version, flush=True)
            if options.group in ('all', 'lifecycle'):
                lifecycle(binaries, pki, directory)
            if options.group in ('all', 'tls'):
                from tls_acceptance import tls_acceptance
                tls_acceptance(binaries, pki, directory)
            if options.group in ('all', 'isolation'):
                from isolation_acceptance import isolation_acceptance
                isolation_acceptance(binaries, pki, directory)
    except (AcceptanceFailure, OSError, RuntimeError) as error:
        print('P02 acceptance FAILED: ' + str(error), file=__import__('sys').stderr)
        return 1
    print('P02 ' + options.group + ' real-service acceptance passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
