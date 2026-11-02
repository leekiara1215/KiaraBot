import pytest

from kiarabot.v1 import common_pb2 as common
from kiarabot_sdk import PluginError
from kiarabot_sdk.__main__ import options_from_environment


def base(tmp_path):
    tls_root = tmp_path / 'ca.pem'
    tls_root.write_bytes(b'-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----\n')
    return {'PLUGIN_CORE_ENDPOINT': '127.0.0.1:1234',
            'PLUGIN_AUTH_TOKEN': 't' * 32,
            'PLUGIN_TLS_ROOT': str(tls_root)}


def test_defaults_match_the_library_defaults(tmp_path):
    options = options_from_environment(base(tmp_path))
    assert (options.io_timeout, options.hook_timeout, options.heartbeat_interval,
            options.retry_delay, options.lease_renew_interval) == (5.0, 5.0, 5.0, 1.0, 10.0)
    assert options.tls_root_certificate


def test_environment_overrides_runtime_parameters(tmp_path):
    options = options_from_environment(dict(
        base(tmp_path), PLUGIN_IO_TIMEOUT='2.5', PLUGIN_HOOK_TIMEOUT='0.5',
        PLUGIN_HEARTBEAT_INTERVAL='0.25', PLUGIN_RETRY_DELAY='3',
        PLUGIN_LEASE_RENEW_INTERVAL='1', PLUGIN_INSTANCE_ID='instance-1',
        PLUGIN_TLS_SERVER_NAME='runtime.example'))
    assert (options.io_timeout, options.hook_timeout, options.heartbeat_interval,
            options.retry_delay, options.lease_renew_interval) == (2.5, 0.5, 0.25, 3.0, 1.0)
    assert options.instance_id == 'instance-1'
    assert options.tls_server_name == 'runtime.example'


@pytest.mark.parametrize('change', [
    {'PLUGIN_IO_TIMEOUT': 'nope'},
    {'PLUGIN_HOOK_TIMEOUT': '0'},
    {'PLUGIN_HEARTBEAT_INTERVAL': 'nan'},
    {'PLUGIN_RETRY_DELAY': '-1'},
    {'PLUGIN_RETRY_DELAY': 'inf'},
    {'PLUGIN_LEASE_RENEW_INTERVAL': '0'},
    {'PLUGIN_CORE_ENDPOINT': ''},
    {'PLUGIN_CORE_ENDPOINT': 'localhost'},
    {'PLUGIN_CORE_ENDPOINT': '127.0.0.1:0'},
    {'PLUGIN_AUTH_TOKEN': 'short'},
    {'PLUGIN_TLS_ROOT': '/missing/ca.pem'},
])
def test_invalid_runtime_configuration_fails_with_stable_code(tmp_path, change):
    with pytest.raises(PluginError) as failure:
        options_from_environment(dict(base(tmp_path), **change))
    assert failure.value.code == common.INVALID_ARGUMENT


def test_missing_required_variables_are_invalid(tmp_path):
    for key in ('PLUGIN_CORE_ENDPOINT', 'PLUGIN_AUTH_TOKEN', 'PLUGIN_TLS_ROOT'):
        environ = base(tmp_path)
        environ.pop(key)
        with pytest.raises(PluginError) as failure:
            options_from_environment(environ)
        assert failure.value.code == common.INVALID_ARGUMENT


def test_managed_host_bootstrap_credential_environment(tmp_path):
    """P08c: the Host injects KIARABOT_PLUGIN_BOOTSTRAP instead of PLUGIN_AUTH_TOKEN."""
    environ = base(tmp_path)
    environ.pop('PLUGIN_AUTH_TOKEN')
    environ['KIARABOT_PLUGIN_BOOTSTRAP'] = 'bootstrap-' + 'b' * 40
    options = options_from_environment(environ)
    assert options.token == 'bootstrap-' + 'b' * 40

    # An explicitly provided bootstrap credential wins over the externally-managed alias.
    both = dict(environ, PLUGIN_AUTH_TOKEN='t' * 32)
    assert options_from_environment(both).token == 'bootstrap-' + 'b' * 40

    # Neither source means the deployment contract is not satisfied.
    missing = base(tmp_path)
    missing.pop('PLUGIN_AUTH_TOKEN')
    with pytest.raises(PluginError) as failure:
        options_from_environment(missing)
    assert failure.value.code == common.INVALID_ARGUMENT

    # Short/invalid bootstrap credentials fail closed with the same stable code.
    environ['KIARABOT_PLUGIN_BOOTSTRAP'] = 'short'
    environ.pop('PLUGIN_AUTH_TOKEN', None)
    with pytest.raises(PluginError) as failure:
        options_from_environment(environ)
    assert failure.value.code == common.INVALID_ARGUMENT
