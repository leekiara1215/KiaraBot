#!/usr/bin/env python3
"""P11d-2 Control process REST acceptance for the Model owner management surface.

Runs the real Control binary with the real-TLS Identity/Model fixtures used by the p04 boundary
test and exercises the P11d Model REST forwarding paths (metadata/list/lifecycle/configuration/
secret rotation/call UNKNOWN query/resolve/operation). Fixtures serve canned typed responses; this
proves the Control transport/delegation/redaction contract, not provider execution.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'control/api/tests/architecture'))
from auth_https_test import main  # noqa: E402

if __name__ == '__main__':
    main(*sys.argv[1:])
