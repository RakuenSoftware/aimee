# Deployment

## Managed server

```bash
docker compose -f compose.server-managed.yaml up -d
```

The browser wizard launches the KB and inference containers. This requires the host Docker socket,
which gives the server Docker-host authority.

Use it for a trusted single-host install where browser-managed setup matters more than that larger
boundary.

## Split stack

```bash
docker compose -f deploy/compose/aimee.yaml up -d
```

Server, KB, and inference are declared together and no browser action needs to create them. This is
the safer default when the server must not control Docker.

## External PostgreSQL

Use `AIMEE_DB2_URL` only as first-boot input, seal it into the KB Vault with a
disposable container, then remove it before creating the long-lived service:

```bash
export AIMEE_DB2_URL='postgresql://...'
./scripts/aimee-compose-vault-bootstrap.sh -f compose.yaml kb
unset AIMEE_DB2_URL
docker compose -f compose.yaml up -d
```

The operator owns:

- PostgreSQL availability and backups;
- TLS and service identity;
- pgvector/pgvectorscale versions;
- connection limits and latency;
- migration and restore testing.

No long-lived server or KB container stores DB2 credentials in `Config.Env`.
The disposable `--rm` bootstrap streams first-boot values over stdin, seals
them synchronously, and exits before the service is created.

## Inference

`aimee-llm` serves embedding, reranking, and synthesis. Select a CPU or GPU tier with the deployment
settings. GPU models live in a persistent model volume; the CPU offline image may bake its model.

The KB must report explicit degradation when a configured inference stage is unavailable. It cannot
claim a dense, reranked, or synthesized result after silently skipping that stage.

## Network ports

Use the compose files and generated configuration as the source of truth. Typical defaults are:

| Service | Port | Exposure |
| --- | ---: | --- |
| browser | 8443 | user network, HTTPS |
| server `/v1` | 8743 | enrolled clients only |
| KB `/v1` | 8741 | deployment network only |
| inference | 8742 | deployment network only |

Do not publish PostgreSQL or inference ports unless a separate host needs them. Apply TLS and service
identity before crossing a trusted container network.

## Volumes and backup

Back up:

- server config and DB1;
- workflow SQLite and artifacts;
- KB PostgreSQL;
- vault root-key or external custody metadata;
- TLS enrollment and revocation state;
- WORM ledger, seals, and off-host anchor state;
- workspace mirrors when rebuilding them is expensive.

Use the KB export helper for the embedded database. Test a restore. `docker compose down -v` deletes
named volumes.

## Hardening

- change bootstrap browser credentials;
- keep server and KB networks private;
- verify TLS fingerprints and issue one client identity per machine;
- configure server ID, team ID, and the root-owned management-JWKS trust bundle;
- grant remote users individually and review revoked rows;
- use the split stack if the Docker socket is not required;
- keep delegates networkless by default;
- stream first-boot provider, git, database, and witness secrets into their owning Vault, then
  recreate/start long-lived services without credential environment mappings;
- ship WORM evidence to an off-host witness when host compromise is in scope;
- alert on failed health, audit verification, witness lag, bus drops, database pressure, and agent
  reaping.

## Upgrade

See [Upgrading from v0.2.192](UPGRADING.md). Deployment topology changes are data migrations, not
just compose edits.
