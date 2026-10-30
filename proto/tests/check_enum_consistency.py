#!/usr/bin/env python3
"""Check that the v1 enum vocabularies match both API contracts."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENUMS = {
    "ErrorCode": "common.proto",
    "HealthState": "common.proto",
    "PluginState": "plugins.proto",
    "ActionStatus": "actions.proto",
}


def proto_values(name: str, file_name: str) -> list[str]:
    source = (ROOT / "proto" / "kiarabot" / "v1" / file_name).read_text()
    match = re.search(rf"\benum\s+{name}\s*\{{([^}}]*)\}}", source, re.DOTALL)
    if not match:
        raise ValueError(f"missing Protobuf enum {name}")
    return re.findall(r"(?m)^\s*([A-Z][A-Z0-9_]*)\s*=\s*-?\d+\s*;", match.group(1))


def api_values(path: Path, name: str) -> list[str]:
    source = path.read_text()
    match = re.search(
        rf"(?m)^    {name}:\s*\n\s+type:\s*string\s*\n\s+enum:\s*\[([^\]]+)\]",
        source,
    )
    if not match:
        raise ValueError(f"missing {name} schema in {path.relative_to(ROOT)}")
    return [value.strip() for value in match.group(1).split(",")]


def main() -> int:
    try:
        for name, file_name in ENUMS.items():
            expected = proto_values(name, file_name)
            for relative_path in ("api/openapi.yaml", "api/asyncapi.yaml"):
                actual = api_values(ROOT / relative_path, name)
                if actual != expected:
                    raise ValueError(
                        f"{name} differs in {relative_path}: expected {expected}, got {actual}"
                    )
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    print("Protobuf, OpenAPI and AsyncAPI enum vocabularies match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
