"""Generate only from the repository's canonical cross-language contracts."""
from pathlib import Path
import subprocess
import sys
import grpc_tools

project = Path(__file__).resolve().parents[1]
source = project.parents[1] / 'proto'
output = project / 'build' / 'generated'
output.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, '-m', 'grpc_tools.protoc', f'-I{source}',
               f'-I{Path(grpc_tools.__file__).parent / "_proto"}',
               f'--python_out={output}', f'--grpc_python_out={output}',
               *map(str, sorted((source / 'kiarabot' / 'v1').glob('*.proto')))], check=True)
