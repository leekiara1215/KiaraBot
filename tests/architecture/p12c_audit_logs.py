#!/usr/bin/env python3
"""P12c-6 Control /logs and /audit aggregation acceptance.

Reuses the real browser-TLS Control + real Identity/Model fixture harness from the P04/P11
acceptance and asserts: authentication/authorization gates, invalid filter/cursor 400s, and that
the aggregation routes never fabricate a successful empty page without authoritative owner sources.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'control/api/tests/architecture'))
from auth_https_test import main  # noqa: E402

if __name__ == '__main__':
    main(*sys.argv[1:])
