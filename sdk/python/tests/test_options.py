import pytest

from kiarabot.v1 import common_pb2 as common
from kiarabot_sdk import PluginError
from kiarabot_sdk.__main__ import options_from_environment

BASE = {'PLUGIN_CORE_ENDPOINT': '127.0.0.1:1234', 'PLUGIN_AUTH_TOKEN': 't' * 32}


def test_defaults_match_the_library_defaults():
    options = options_from_environment(dict(BASE))
    assert (options.io_timeout, options.hook_timeout, options.heartbeat_interval,
            options.retry_delay) == (5.0, 5.0, 1.0, 1.0)


def test_environment_overrides_runtime_parameters():
    options = options_from_environment(dict(BASE, PLUGIN_IO_TIMEOUT='2.5',
                                            PLUGIN_HOOK_TIMEOUT='0.5',
                                            PLUGIN_HEARTBEAT_INTERVAL='0.25',
                                            PLUGIN_RETRY_DELAY='3'))
    assert (options.io_timeout, options.hook_timeout, options.heartbeat_interval,
            options.retry_delay) == (2.5, 0.5, 0.25, 3.0)


@pytest.mark.parametrize('change', [
    {'PLUGIN_IO_TIMEOUT': 'nope'},
    {'PLUGIN_HOOK_TIMEOUT': '0'},
    {'PLUGIN_HEARTBEAT_INTERVAL': 'nan'},
    {'PLUGIN_RETRY_DELAY': '-1'},
    {'PLUGIN_RETRY_DELAY': 'inf'},
    {'PLUGIN_CORE_ENDPOINT': ''},
    {'PLUGIN_AUTH_TOKEN': 'short'},
])
def test_invalid_runtime_configuration_fails_with_stable_code(change):
    with pytest.raises(PluginError) as failure:
        options_from_environment(dict(BASE, **change))
    assert failure.value.code == common.INVALID_ARGUMENT


def test_missing_required_variables_are_invalid():
    missing_endpoint = dict(BASE)
    missing_endpoint.pop('PLUGIN_CORE_ENDPOINT')
    for environ in (missing_endpoint, {key: value for key, value in BASE.items()
                                       if key != 'PLUGIN_AUTH_TOKEN'}):
        with pytest.raises(PluginError) as failure:
            options_from_environment(environ)
        assert failure.value.code == common.INVALID_ARGUMENT
