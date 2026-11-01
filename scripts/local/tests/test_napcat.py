"""Deployment-driver configuration and ownership, without real QQ accounts."""
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import napcat


def test_prepare_preserves_credentials_and_existing_login_configuration(tmp_path):
    state = napcat.state_directory(str(tmp_path / 'owned'))
    napcat.prepare(state)
    token = napcat.read(state / 'onebot-token').strip()
    webui = napcat.read(state / 'webui-password').strip()
    assert token != webui and len(token) >= 32
    config = json.loads(napcat.read(state / 'container/config/onebot11.json'))
    ws = config['network']['websocketServers'][0]
    assert ws['token'] == token and ws['enable'] and ws['messagePostFormat'] == 'array'
    assert not ws['reportSelfMessage'] and not ws['debug']
    assert not config['network']['websocketClients']
    for name in ['onebot-token', 'webui-password', 'container/config/onebot11.json']:
        assert (state / name).stat().st_mode & 0o777 == 0o600
    custom = state / 'container/config/webui.json'
    napcat.write(custom, json.dumps({'token': 'updated-by-operator', 'port': 6099}))
    napcat.prepare(state)
    assert napcat.read(state / 'onebot-token').strip() == token
    assert json.loads(napcat.read(custom))['token'] == 'updated-by-operator'


def test_rejects_symlink_configuration_without_overwriting_target(tmp_path):
    state = napcat.state_directory(str(tmp_path / 'owned'))
    napcat.prepare(state)
    victim = tmp_path / 'victim'
    victim.write_text('unchanged')
    config = state / 'container/config/onebot11.json'
    config.unlink()
    config.symlink_to(victim)
    with pytest.raises(napcat.Failure):
        napcat.prepare(state)
    assert victim.read_text() == 'unchanged'


def test_refuses_foreign_container_and_non_loopback_ports(tmp_path, monkeypatch):
    state = napcat.state_directory(str(tmp_path / 'owned'))
    owner = json.loads(napcat.read(state / 'owner.json'))['owner']
    record = {'Config': {'Labels': {napcat.OWNER_LABEL: 'foreign'}},
              'HostConfig': {'PortBindings': {
                  str(port) + '/tcp': [{'HostIp': '127.0.0.1', 'HostPort': str(port)}]
                  for port in [6099, 3001]}}}

    def inspect(*args, **kwargs):
        return subprocess.CompletedProcess(args, 0, stdout=json.dumps([record]))

    monkeypatch.setattr(napcat.subprocess, 'run', inspect)
    with pytest.raises(napcat.Failure, match='foreign_container'):
        napcat.container(state, {})
    record['Config']['Labels'][napcat.OWNER_LABEL] = owner
    napcat.container(state, {})
    record['HostConfig']['PortBindings']['6099/tcp'][0]['HostIp'] = '0.0.0.0'
    with pytest.raises(napcat.Failure, match='port_exposure'):
        napcat.container(state, {})


@pytest.mark.parametrize('online', [False, None])
def test_connect_never_starts_business_processes_without_confirmed_online(tmp_path, monkeypatch, online):
    state = napcat.state_directory(str(tmp_path / 'owned'))
    monkeypatch.setattr(napcat, 'container', lambda *args: ('owned', {'State': {'Running': True}}))
    monkeypatch.setattr(napcat, 'login_info', lambda *args: {'account': '123', 'online': online})

    def unexpected_launch(*args, **kwargs):
        pytest.fail('must not launch Core/Adapter before platform is confirmed online')

    monkeypatch.setattr(napcat.subprocess, 'run', unexpected_launch)
    with pytest.raises(napcat.Failure, match='qq_login_or_authenticated_onebot_not_ready'):
        napcat.operate(state, SimpleNamespace(command='connect', conversation='456', ui=False))
