#!/usr/bin/env python3
"""P11h independent managed Host -> Runtime -> Model -> real HTTP Provider acceptance.

This is the P11 exit gate, kept separate from P05b-4's own CTest: it runs the merged real-chain
driver (canonical P05b-4 provider fixture, P05b-8 sealed reader, P05b-9 Identity delegation budget,
managed kiarabot-plugin-host + SDK child) in its own owned PG16.4/TLS fixture and asserts the
driver's explicit positive/negative success markers. It never references a build-deploy branch;
only merged main artifacts are used. Do not weaken the driver's assertions here.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DRIVER = REPO / 'tests/architecture/p05b_model_chain_e2e.py'
POSITIVE_MARKER = 'P05b model single chain: plugin->Runtime->Model->real HTTP provider passed'
FAIL_CLOSED_MARKER = 'P05b model chain fail-closed E2E passed'


def main() -> int:
    args = list(sys.argv[1:])
    mode = 'expect-success'
    if '--mode' in args:
        index = args.index('--mode')
        if index + 1 < len(args):
            mode = args[index + 1]
    process = subprocess.run([sys.executable, str(DRIVER), *args],
                             capture_output=True, text=True, timeout=360)
    sys.stdout.write(process.stdout)
    sys.stderr.write(process.stderr)
    if process.returncode != 0:
        print(f'P11h independent acceptance failed: driver rc={process.returncode}', file=sys.stderr)
        return process.returncode
    expected = FAIL_CLOSED_MARKER if mode == 'expect-fail-closed' else POSITIVE_MARKER
    if expected not in process.stdout:
        print(f'P11h independent acceptance marker missing: {expected!r}', file=sys.stderr)
        return 1
    if 'p05b-fixture-secret' in process.stdout:
        print('P11h independent acceptance leaked provider secret to stdout', file=sys.stderr)
        return 1
    print(f'P11h independent managed model acceptance passed ({mode})')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
