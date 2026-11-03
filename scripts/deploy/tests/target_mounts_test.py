"""Check the consumer boundary on a fully interpolated Compose project (JSON on stdin)."""
import json
import sys
from pathlib import PurePosixPath as Path

config = json.load(sys.stdin)
owners = {"identity", "runtime", "scheduler", "model"}
roles = {"identity", "identity-bootstrap"}
grants = {
    "admin.pass": {"identity-bootstrap"},
    "token.hex": roles, "request.hex": roles, "receipt.hex": roles,
    "delegation.pem": roles, "delegation.key": roles,
    "model-root-key.hex": {"model"},
    "postgres-superuser": {"postgres"},
    "onebot-access-token": {"adapter"}, "adapter-bootstrap": {"adapter"},
}
for owner in owners:
    grants[f"{owner}-runtime.pass"] = roles if owner == "identity" else {owner}
    grants[f"{owner}-migration.pass"] = {f"init-{owner}"}

aliases = {key.removesuffix(".pass"): key for key in grants}
aliases.update({
    "identity-token": "token.hex", "identity-request": "request.hex",
    "identity-receipt": "receipt.hex", "identity-verification-key": "delegation.pem",
    "identity-signing-key": "delegation.key", "model-root-key": "model-root-key.hex",
})

# Check the destination against the consumer policy, then check that no credential
# file is also exposed under another name or via a parent directory mount.
consumers = {}
for service, cfg in config["services"].items():
    for mount in cfg.get("volumes", []):
        if mount["type"] != "bind":
            continue
        target = Path(mount["target"])
        permitted = None
        if target == Path("/etc/postgresql/tls"):
            permitted = {"postgres"}
        elif target.parent == Path("/etc/kiarabot/secrets"):
            permitted = grants.get(target.name, set())
        elif target.name == "server.key":
            permitted = roles if target.parent.name == "identity" else {target.parent.name}
        elif target in map(Path, ("/etc/kiarabot", "/etc/kiarabot/secrets", "/etc/kiarabot/tls")):
            raise SystemExit(f"{service}: shared material directory visible at {target}")
        if permitted is not None:
            if not mount.get("read_only") or service not in permitted:
                raise SystemExit(f"{service}: unexpected access to {target}")
            consumers.setdefault(Path(mount["source"]), set(permitted)).intersection_update(permitted)
    for secret in cfg.get("secrets", []):
        name = secret["source"]
        source = Path(config["secrets"][name]["file"])
        permitted = grants.get(aliases.get(name), set())
        if service not in permitted:
            raise SystemExit(f"{service}: unexpected secret {name}")
        consumers.setdefault(source, set(permitted)).intersection_update(permitted)

for service, cfg in config["services"].items():
    files = [Path(v["source"]) for v in cfg.get("volumes", []) if v["type"] == "bind"]
    files += [Path(config["secrets"][s["source"]]["file"]) for s in cfg.get("secrets", [])]
    for source in files:
        for credential, allowed in consumers.items():
            if service not in allowed and (source == credential or source in credential.parents or credential in source.parents):
                raise SystemExit(f"{service}: unauthorized credential mount {source.name}")
