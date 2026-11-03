#!/usr/bin/env python3
"""P05b-4 P11 single-chain e2e harness scaffold (real managed plugin -> Runtime -> Model -> HTTP).

Dependency: P05b-2 (task-144, Identity instance-subject parent delegation) must be merged before
the positive chain can be wired/asserted. This harness is prepared now and deliberately refuses
to claim a positive production chain until then:

  --mode self-check  validate binaries/provider fixture and print the planned phases (runnable now)
  --expect-fail-closed
                     run the negative assertions that are valid before P05b-2, specifically that
                     the production composition still fails closed (stable
                     UNAUTHENTICATED/UPSTREAM_UNAVAILABLE) and the local Provider fixture records
                     zero requests. Requires the orchestration functions below to be filled by
                     P05b-4's implementation phase; if a prerequisite is absent it exits 77 (skip).
  --expect-success   gated: exits 77 unless KIARABOT_P05B2_AVAILABLE=1 and the success chain is
                     implemented (this mode is the acceptance phase after P05b-2 + provider wiring).

Real Provider fixture used by every phase: tests/architecture/p05b_provider_fixture.py
(local HTTP server with auth/stream=false/usage/400/401/404/429/5xx/timeout/non-JSON modes).
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

PLANNED_PHASES = (
    'start disposable PG16.4 databases (identity/runtime/model)',
    'generate deployment PKI and offline platform peers (P05 fixture pattern)',
    'start real Identity, Runtime (production composition), Model',
    'start local HTTP Provider fixture and point Model private config at it',
    'start real managed HostBridge/kIarabot-plugin-host + SDK plugin child (P08e pattern)',
    'P05b-2: Control/admin triggers instance-subject parent delegation; Runtime exchanges to '
    'audience=model, hops=2, client_certificate=Runtime leaf',
    'send HostRequest.model_chat and assert HostResponse result + call record/usage owner=instance',
    'negative matrix: revoke/expire/cross-instance/wrong certificate/Identity offline/provider '
    'errors/timeout/non-JSON/4xx/429/5xx; Provider fixture request counts and no secret in logs',
)


class Failure(RuntimeError):
    pass


def require(condition, label):
    if not condition:
        raise Failure(label)


def load_provider_fixture(repo: Path):
    sys.path.insert(0, str(repo / 'tests/architecture'))
    import p05b_provider_fixture
    return p05b_provider_fixture


def check_binaries(args) -> None:
    for name in ('runtime', 'model', 'identity', 'initializer', 'openssl', 'pg_bin'):
        value = getattr(args, name)
        require(bool(value), f'missing --{name}')
        path = Path(value)
        require(path.exists(), f'--{name} not found: {path}')
    require(Path(args.provider_fixture).is_file(), 'provider fixture missing')
    require(Path(args.repo_source, 'sdk/python').is_dir(), 'repo-source must contain sdk/python')


def run_self_check(args) -> int:
    provider = load_provider_fixture(Path(args.repo_source))
    check_binaries(args)
    # The fixture matrix is the runnable prep evidence before P05b-2 lands.
    require(provider.self_test() == 0, 'provider fixture self-test')
    require('success' in provider.MODES and 'status_429' in provider.MODES and
            'timeout' in provider.MODES, 'provider fixture mode matrix')
    print('P05b-4 e2e self-check passed (binaries present, provider fixture matrix green)')
    print('P05b-4 planned positive phases:')
    for index, phase in enumerate(PLANNED_PHASES, 1):
        print(f'  {index}. {phase}')
    require(os.environ.get('KIARABOT_P05B2_AVAILABLE') != '1',
            'self-check must run before P05b-2 phase; use --expect-success for acceptance')
    return 0


def assert_fail_closed(error_code: str, provider_requests: int, label: str) -> None:
    """Negative assertion shared by the pre-P05b-2 phase: production must fail closed."""
    require(error_code in ('UNAUTHENTICATED', 'UPSTREAM_UNAVAILABLE'),
            f'{label}: expected stable fail-closed code, got {error_code}')
    require(provider_requests == 0,
            f'{label}: fail-closed path must not reach the HTTP Provider ({provider_requests})')


def assert_no_secret(text: str, secret: str, label: str) -> None:
    require(secret not in text, f'{label}: provider secret leaked into logs/output')


def run_expect_fail_closed(args) -> int:
    check_binaries(args)
    # Phase implementation lives in P05b-4 proper: this mode is intentionally a hard skip until
    # the P05b-2 dependency and the stack assembly are present, so CI never records a fake pass.
    if os.environ.get('KIARABOT_P05B2_AVAILABLE') == '1':
        raise Failure('fail-closed mode is for the pre-P05b-2 phase; use --expect-success')
    print('P05b-4 fail-closed assertions prepared; stack assembly dependency (P05b-2/task-144) '
          'not yet merged, skipping execution (exit 77)', file=sys.stderr)
    return 77


def run_expect_success(args) -> int:
    check_binaries(args)
    if os.environ.get('KIARABOT_P05B2_AVAILABLE') != '1':
        print('P05b-4 positive chain gated: P05b-2 (task-144 instance-subject parent delegation) '
              'is not merged; refusing to assert a positive production chain (exit 77).',
              file=sys.stderr)
        return 77
    raise Failure('P05b-4 success-chain orchestration not implemented in this prep commit')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime')
    parser.add_argument('--model')
    parser.add_argument('--identity')
    parser.add_argument('--initializer')
    parser.add_argument('--openssl')
    parser.add_argument('--pg-bin', dest='pg_bin')
    parser.add_argument('--repo-source', default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument('--provider-fixture', default=str(Path(__file__).with_name(
        'p05b_provider_fixture.py')))
    parser.add_argument('--mode', choices=('self-check', 'expect-fail-closed', 'expect-success'),
                        default='self-check')
    parser.add_argument('--state-dir', default=None)
    args = parser.parse_args()
    try:
        if args.mode == 'self-check':
            return run_self_check(args)
        if args.mode == 'expect-fail-closed':
            return run_expect_fail_closed(args)
        return run_expect_success(args)
    except Failure as error:
        print(f'P05b-4 e2e failure: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
