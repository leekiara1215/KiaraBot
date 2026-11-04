# Target deployment material (operator-owned, never committed)

This directory is the default host-side material root for `deploy/compose.infra.yml`,
`deploy/compose.core.yml` and `deploy/compose.exec.yml`. **Every file here except this
README is deployment-generated and must be gitignored** (the repository `.gitignore`
already ignores `deploy/.env` and secrets; keep target material outside the repo or in
`deploy/target/` which must never be committed).

Expected layout (all directories/files are mounted read-only into containers):

```text
deploy/target/
  config/
    identity.json          # ProcessConfig: service=identity, probe/grpc, tls, identity_security
    runtime.json           # ProcessConfig: service=runtime, bridge_listen, database(runtime), identity_endpoint
    scheduler.json         # ProcessConfig: service=scheduler, database(scheduler), identity_endpoint
    model.json             # ProcessConfig: service=model, database(model), model_security, identity_endpoint
    plugin-host.json       # ProcessConfig: service=plugin-host, host{host_id,sqlite_path,runtime_endpoint,...}
    control.json           # target Control ProcessConfig (tls, probe_listen, control_api)
    enrollment.json        # Identity offline enrollment input for kiarabot-identity-bootstrap
  tls/
    ca.pem                 # deployment CA used by owner mTLS / bridge client verification
    identity/{server.pem,server.key}
    runtime/{server.pem,server.key}
    scheduler/{server.pem,server.key}
    model/{server.pem,server.key}
    plugin-host/{server.pem,server.key}
    control/{server.pem,server.key}
    pg-ca.pem              # CA for verify-full PostgreSQL clients
    postgres/{server.crt,server.key,ca.crt}
    bridge-ca.pem          # CA the Adapter uses to verify the Runtime bridge certificate
  secrets/                 # files must be 0600, non-symlink, owned by the deployment user
    postgres-superuser.pass
    identity-migration.pass
    identity-runtime.pass
    runtime-migration.pass
    runtime-runtime.pass
    scheduler-migration.pass
    scheduler-runtime.pass
    model-migration.pass
    model-runtime.pass
    admin.pass             # Identity bootstrap administrator password
    token.hex, request.hex, receipt.hex    # Identity key files named by the generated config
    delegation.pem, delegation.key       # Identity delegation public/private signing pair
    model-root-key.hex     # 32-byte hex, 0600; read once by Model
    adapter.bootstrap      # P05/P06 bootstrap credential for the external adapter
    onebot-access-token
  data/host/               # Plugin Host SQLite journal (writable)
  packages/                # approved plugin packages/profile roots (read-only)
  backups/postgres/        # PG backup observations / operator copies (writable)
```

`deploy/target.env.example` lists the Compose variables. `scripts/deploy/target-smoke.sh`
checks the Compose/YAML/Dockerfile structure offline and, when Docker is available, runs
`docker compose config` and an optional image build; it never initializes databases or
starts services automatically.

Containers mount only the files declared for their service. For custom key rings, enrollment peers or sealed Runtime/Model delegation files, mount each additional file read-only into its consumer and use that path in its config. Never mount the entire material root or store a CA signing key there.

## BOOT-04 controlled bootstrap delivery (single host, 2026-11-04)

Runtime never writes a Host filesystem and HostBridge has no credential delivery frame. In the
controlled workflow the one-time bootstrap secret flows only through the Control delivery sink:

```text
Runtime credential intent + digest + version     (Runtime private PG, no plaintext)
  -> Create/Bind response plaintext once in memory (never REST/WS/log/audit/receipt)
  -> Control SecretDeliveryPort, fixed recipient  (0600 atomic file + no-secret intent/receipt)
  -> read-only mount into Host / Adapter consumer (0600, owner = consumer UID)
  -> white-listed credential environment -> SDK PluginBridge / Adapter bridge
```

`deploy/compose.target.yml` mounts `${KIARABOT_DELIVERY_DIR:-./target/delivery}` writable only into
Control (`/var/lib/kiarabot-control/delivery`) and read-only into Plugin Host
(`/var/lib/kiarabot-host/credentials`) and Adapter (`/run/secrets/delivery`). The Plugin Host
`host.credential_directory` config value and `BRIDGE_BOOTSTRAP_CREDENTIAL_FILE` must point at the
consumer-side mounts, not at the Control path.

Fixed recipient registry:
- `deploy/control/secret-recipients.example.json` is the offline, non-secret template. The Control
  composition loads the operator copy (0600). Each entry maps one `recipient_id` to an exact
  `resource_kind`/`resource_id`, the fixed peer file name `peer_id`, and the Control-side absolute
  delivery directory.
- A browser, plugin or delivery request can never supply a path, URL, owner, UID/GID, environment
  value or command; it only selects an existing `recipient_id` (or omits it and gets the legacy
  create/bind behavior).
- A recipient entry with a non-empty `resource_id` is **exact**: only that Runtime resource may use it
  (Adapter delivery validates the bound account id). An entry without `resource_id` is **kind-level**:
  it accepts any resource of `resource_kind` in the same fixed directory/owner, and the installed file
  name is derived from the exact delivery resource id (`<resource_id>.bootstrap`). Kind-level is the
  normal way to pre-register a deployment target for managed plugin instances whose UUID is allocated
  by Runtime at CreateInstance time; a reissue always names its exact resource again.
- After a lost create/bind response, resend the same idempotent request: Runtime returns the current
  no-secret delivery metadata (never the plaintext) so Control can recover the original `delivery_id`
  and query/report it.
- One recipient per exact resource and one file per peer. The file is
  `<delivery_directory>/<peer_id>.bootstrap`, mode 0600, regular file, no symlinks/hardlinks.
- Externally-managed plugins keep their own independent deployment secret and must not be placed in
  the Host `credential_directory`; only Host-managed plugin instances use the Host mount. The adapter
  keeps its own read-only bootstrap secret. QQ/NapCat passwords, Identity administrator passwords,
  Host mTLS keys and business delegation material never enter this port.

Operator verification (no secret or digest is printed):

```bash
# Deployment/Control side: full managed verification including receipt digest/version.
scripts/deploy/target_host_credential.py verify \
  --directory /var/lib/kiarabot-control/delivery/host-a/credentials \
  --recipients /etc/kiarabot/control/secret-recipients.json --managed

# Consumer side (read-only mount): 0600/owner/regular/single-link and recipient set only.
scripts/deploy/target_host_credential.py verify \
  --directory /var/lib/kiarabot-host/credentials \
  --recipients /etc/kiarabot-host/secret-recipients.json --require-read-only

# Never remove a registered active recipient. Fail closed on unexpected/stale peer files.
scripts/deploy/target_host_credential.py prune \
  --directory /var/lib/kiarabot-control/delivery/host-a/credentials \
  --recipients /etc/kiarabot/control/secret-recipients.json
```

`verify --managed` cross-checks the target file SHA-256 against the no-secret sink receipt and
requires the receipt's `credential_version` to match; a rotated delivery is only valid after an
explicit authorized Runtime reissue produced the next version. The helper refuses `store` for any
registered/controlled recipient or any directory that already has sink metadata, so a manual
one-time RPC response cannot impersonate the controlled chain. Legacy deployments without
recipients keep the previous `store --replace`/`prune --active` behavior; see P08d above.
