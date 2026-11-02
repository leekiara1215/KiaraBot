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
    control.json           # Control API config (listen/users/encryption_key), see deploy/control/control.example.json
    enrollment.json        # Identity offline enrollment input for kiarabot-identity-bootstrap
  tls/
    ca.pem                 # deployment CA used by owner mTLS / bridge client verification
    identity/{server.pem,server.key}
    runtime/{server.pem,server.key}
    scheduler/{server.pem,server.key}
    model/{server.pem,server.key}
    host/{server.pem,server.key}
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
    identity-verification-key.pem
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
