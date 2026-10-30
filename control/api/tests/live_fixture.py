"""Disposable C++ server fixture for the TypeScript integration suite."""
import json
import sys
import tempfile
from pathlib import Path
from conftest import Server

with tempfile.TemporaryDirectory(prefix="kiarabot-panel-") as directory:
    server = Server(Path(directory))
    try:
        print(json.dumps({"base": server.base, "password": server.password}), flush=True)
        sys.stdin.readline()
    finally:
        server.stop()
