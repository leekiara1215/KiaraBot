"""Real six-process parallelism, port conflicts, and single-child crash isolation."""
import copy
import signal
from contextlib import ExitStack

from fixtures import SERVICES, reserved_listener
from process_support import (AcceptanceFailure, ServiceProcess, assert_closed, assert_equal,
                             assert_exit, assert_probe, assert_sanitized, run_once,
                             service_config, write_config)


def kill_child(process):
    if process.poll() is not None:
        raise AcceptanceFailure('crash target exited before SIGKILL fault injection')
    process.kill()
    assert_equal(process.wait(timeout=5), -signal.SIGKILL, 'injected crash exit')


def assert_survivors(children, victim):
    for service, child in children.items():
        if service == victim:
            continue
        assert_equal(child.process.poll(), None, service + ' survived peer crash')
        assert_probe(child.config['probe_listen'], '/health/live', 200, 'HEALTHY')
        assert_probe(child.config['probe_listen'], '/health/ready', 503, 'UNHEALTHY')


def isolation_acceptance(binaries, pki, directory):
    for service, binary in binaries.items():
        baseline = service_config(service, pki)
        keys = ('probe_listen',) if service == 'control' else ('probe_listen', 'grpc_listen')
        for key in keys:
            with reserved_listener() as occupied:
                config = copy.deepcopy(baseline)
                config[key] = '127.0.0.1:' + str(occupied.getsockname()[1])
                path = write_config(directory, service, config)
                assert_exit(run_once(binary, ['--config', path], directory), 1,
                            r'bind|listen|address|port')
                for other in keys:
                    if other != key:
                        assert_closed(config[other])
        print(service + ': held ephemeral probe/gRPC port rejected without leaked listener', flush=True)

    # Hold every chosen endpoint until its child is about to launch. This prevents our
    # own service configs from accidentally selecting the same released ephemeral port.
    with ExitStack() as stack:
        configs, reservations, children = {}, {}, {}
        for service in SERVICES:
            config = service_config(service, pki)
            reservations[service] = []
            for key in ('probe_listen', 'grpc_listen'):
                if key in config:
                    listener = stack.enter_context(reserved_listener())
                    config[key] = '127.0.0.1:' + str(listener.getsockname()[1])
                    reservations[service].append(listener)
            configs[service] = config
        for service in SERVICES:
            for listener in reservations[service]:
                listener.close()
            children[service] = stack.enter_context(
                ServiceProcess(binaries[service], configs[service], directory, service))
        for child in children.values():
            child.wait_live()
            assert_probe(child.config['probe_listen'], '/health/ready', 503, 'UNHEALTHY')
        assert_equal(len({child.process.pid for child in children.values()}), 6, 'independent child PIDs')
        for service in SERVICES:
            victim = children[service]
            kill_child(victim.process)
            assert_sanitized(victim.output())
            for key in ('probe_listen', 'grpc_listen'):
                if key in victim.config:
                    assert_closed(victim.config[key])
            # Check repeatedly, not just once before a delayed coupled shutdown fires.
            for _ in range(3):
                assert_survivors(children, service)
                __import__('time').sleep(0.1)
            restarted = stack.enter_context(
                ServiceProcess(binaries[service], configs[service], directory, service))
            restarted.wait_live()
            children[service] = restarted
            assert_survivors(children, None)
            print(service + ': SIGKILL isolated; other five remain live; isolated restart passed', flush=True)
        for child in children.values():
            child.stop()
