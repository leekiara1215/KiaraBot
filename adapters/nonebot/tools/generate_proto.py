"""Generate from the Core repository's sole protocol source, never copy schemas."""
import argparse
from pathlib import Path
import subprocess
import sys

import grpc_tools

parser = argparse.ArgumentParser()
parser.add_argument('proto_root', type=Path)
args = parser.parse_args()
root = args.proto_root.resolve()
output = Path(__file__).resolve().parents[1] / 'generated'
output.mkdir(exist_ok=True)
files = sorted((root / 'kiarabot' / 'v1').glob('*.proto'))
if not files:
    parser.error('No kiarabot/v1 protocol sources found')
subprocess.run([
    sys.executable, '-m', 'grpc_tools.protoc', f'-I{root}',
    f'-I{Path(grpc_tools.__file__).parent / "_proto"}',
    f'--python_out={output}', f'--grpc_python_out={output}',
    *map(str, files),
], check=True)
