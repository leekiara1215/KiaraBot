"""mTLS startup rejection and real gRPC status checks against explicit P02 binaries."""
import copy
import importlib
import json
import sys
from pathlib import Path

from process_support import (AcceptanceFailure, ServiceProcess, assert_closed, assert_equal,
                             assert_exit, assert_probe, run_once, service_config, write_config)

OWNERS = {'identity': 'IdentityService', 'runtime': 'RuntimeService',
          'plugin-host': 'HostService', 'scheduler': 'SchedulerService', 'model': 'ModelService'}


def load_grpc():
    lock = json.loads(Path(__file__).with_name('grpc_environment.json').read_text())
    actual_python = '.'.join(map(str, sys.version_info[:3]))
    if actual_python != lock['python']:
        raise AcceptanceFailure('TLS group requires locked Python ' + lock['python'] + ', got ' + actual_python)
    try:
        grpc = importlib.import_module('grpc')
    except ImportError as error:
        raise AcceptanceFailure('TLS group requires installed locked grpcio ' + lock['grpcio']) from error
    if grpc.__version__ != lock['grpcio']:
        raise AcceptanceFailure('TLS group requires grpcio ' + lock['grpcio'] + ', got ' + grpc.__version__)
    return grpc


def expected_rpc_error(grpc, channel, path, code, streaming=False, bidi=False):
    try:
        if bidi:
            next(channel.stream_stream(path)(iter(()), timeout=2))
        elif streaming:
            next(channel.unary_stream(path)(b'', timeout=2))
        else:
            channel.unary_unary(path)(b'', timeout=2)
    except grpc.RpcError as error:
        assert_equal(error.code(), code, path + ' gRPC status')
    except StopIteration as error:
        raise AcceptanceFailure(path + ' returned successful empty stream instead of refusal') from error
    else:
        raise AcceptanceFailure(path + ' unexpectedly returned success')


def credentials(grpc, ca, client=None):
    return grpc.ssl_channel_credentials(
        root_certificates=ca.certificate.read_bytes(),
        private_key=client.key.read_bytes() if client else None,
        certificate_chain=client.certificate.read_bytes() if client else None)


def tls_acceptance(binaries, pki, directory):
    grpc = load_grpc()  # Fail before spawning services if the locked environment is absent.
    root = Path(__file__).resolve().parents[2]
    methods = json.loads((root / 'proto/tests/target_spec.json').read_text())['methods']
    for service, binary in binaries.items():
        baseline = service_config(service, pki)
        malformed = Path(directory) / 'not-a-certificate.pem'
        malformed.write_text('not a certificate or private key')
        faults = [
            {'certificate_file': str(Path(directory) / 'absent-certificate.pem')},
            {'private_key_file': str(Path(directory) / 'absent-key.pem')},
            {'ca_file': str(Path(directory) / 'absent-ca.pem')},
            {'certificate_file': str(malformed)},
            {'private_key_file': str(malformed)},
            {'ca_file': str(malformed)},
            {'private_key_file': str(pki.client.key)},
            {'certificate_file': str(pki.expired_server.certificate),
             'private_key_file': str(pki.expired_server.key)},
            {'ca_file': str(pki.foreign_ca.certificate)},
        ]
        for fault in faults:
            config = copy.deepcopy(baseline)
            config['tls'].update(fault)
            path = write_config(directory, service, config)
            assert_exit(run_once(binary, ['--config', path], directory), 1,
                        r'tls|certificat|private.key|credential|ca.file|ssl')
            for key in ('probe_listen', 'grpc_listen'):
                if key in baseline:
                    assert_closed(baseline[key])
        if service == 'control':
            print('control: missing/malformed/mismatched/expired/untrusted TLS config refused', flush=True)
            continue
        config = service_config(service, pki)
        with ServiceProcess(binary, config, directory, service) as child:
            child.wait_live()
            endpoint = config['grpc_listen']
            owner_methods = [m for m in methods if m['service'] == OWNERS[service]]
            if not owner_methods:
                raise AcceptanceFailure('owner has no declared RPC cases')
            first = '/kiarabot.v1.' + OWNERS[service] + '/' + owner_methods[0]['method']
            with grpc.secure_channel(endpoint, credentials(grpc, pki.ca, pki.client)) as channel:
                try:
                    grpc.channel_ready_future(channel).result(timeout=3)
                except grpc.FutureTimeoutError as error:
                    raise AcceptanceFailure('trusted mTLS channel did not become ready') from error
                for method in owner_methods:
                    path = '/kiarabot.v1.' + method['service'] + '/' + method['method']
                    expected_rpc_error(grpc, channel, path, grpc.StatusCode.UNIMPLEMENTED,
                                       streaming=method['server_streaming'])
                if service == 'runtime':
                    for bridge in ('AdapterBridge', 'PluginBridge', 'HostBridge'):
                        expected_rpc_error(grpc, channel, '/kiarabot.v1.' + bridge + '/Connect',
                                           grpc.StatusCode.UNIMPLEMENTED, bidi=True)
                for ca, client in ((pki.ca, None), (pki.ca, pki.foreign_client),
                                   (pki.ca, pki.expired_client), (pki.foreign_ca, pki.client)):
                    with grpc.secure_channel(endpoint, credentials(grpc, ca, client)) as denied:
                        expected_rpc_error(grpc, denied, first, grpc.StatusCode.UNAVAILABLE,
                                           streaming=owner_methods[0]['server_streaming'])
                    # A broken service cannot masquerade as a successful authentication rejection.
                    expected_rpc_error(grpc, channel, first, grpc.StatusCode.UNIMPLEMENTED,
                                       streaming=owner_methods[0]['server_streaming'])
                    assert_probe(config['probe_listen'], '/health/live', 200, 'HEALTHY')
            child.stop()
        print(f'{service}: mTLS refusal and {len(owner_methods)} owner RPC statuses passed', flush=True)
