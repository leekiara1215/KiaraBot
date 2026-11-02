"""Shared fixtures: one disposable TLS server identity per test session."""
import pytest

from tls_fixture import write_server_tls


@pytest.fixture(scope='session')
def server_tls(tmp_path_factory):
    directory = tmp_path_factory.mktemp('server-tls')
    return write_server_tls(directory)
